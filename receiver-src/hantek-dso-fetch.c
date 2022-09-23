#include <stdio.h>
#include <glob.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <linux/limits.h>


#define ERROR_IF(cond, fmt, ...) do { if (cond) { fprintf(stderr, fmt, ## __VA_ARGS__); exit(1); } } while (0)

#define CHANNEL_COUNT 4
#define CHUNK_SIZE 2000
#define HEADER_SIZE 128
#define BUFFER_SIZE (4 * 1024 * 1024)

char device_pattern[] = "/dev/disk/by-id/usb-Waveform_Dump*-0:0";

char sep[10] = "\t";
int repeat = 0;

const int grid = 25;

struct channel {
	int8_t enabled;
	uint8_t index;
	int16_t offset;
	float scale;
	union {
		int8_t ival;
		uint8_t val;
	};
};


glob_t found_devs;


int atoiL(const char * const stg, int from, int len) {
	char buf[len+2];
	strncpy(buf, stg+from, len);
	buf[len+1] = 0;
	/* Need to force base 10 - the numbers have leading zeroes! */
	return strtol(buf, NULL, 10);
}
float atofL(const char * const stg, int from, int len) {
	char buf[len+2];
	strncpy(buf, stg+from, len);
	buf[len+1] = 0;
	return strtof(buf, NULL);
}

void parse_channel(const uint8_t *header, int i, struct channel ch[]) {
	ch[i].enabled = header[75+i] == '1';
	ch[i].scale = atofL((char*)header, 47+7*i, 7);
	ch[i].offset = header[39+2*i] | (header[39+2*i+1] << 8); // LE
}


int translate_data(const uint8_t * const src_data, uint32_t src_len, const char *dest_name) {
	int dest_fh;
	uint32_t total_len;
	int i, ch, ch_enabled, sample_count, sample_nr;
	struct channel ch_data[CHANNEL_COUNT];
	float sample_rate;
	const uint8_t *data;
	const uint8_t *end;
	FILE *output;
	static char *buffer = NULL;

	output = fopen(dest_name, "wt");
	ERROR_IF(!output, "Can't open \"%s\" for writing: %d, %s\n", dest_name, errno, strerror(errno));

	if (!buffer) {
		buffer = malloc(BUFFER_SIZE);
		ERROR_IF(!buffer, "Can't allocate output buffer");
	}
	i = setvbuf(output, buffer, _IOFBF, BUFFER_SIZE);
	printf("setvbuf: %d\n", i);

	/*
	dest_fh = open(dest_name, O_CREAT | O_WRONLY | O_TRUNC, 0700);
	ERROR_IF(dest_fh == -1, "Can't open \"%s\" for writing: %d, %s\n", dest_name, errno, strerror(errno));
	*/

	ERROR_IF(src_len < 4128, "data too small\n");
	ERROR_IF(src_data[0] != '#', "wrong header[0]\n");
	ERROR_IF(src_data[1] != '9', "wrong header[1]\n");

	total_len = atoiL((char*)src_data, 2+9, 9);
	ERROR_IF(total_len + HEADER_SIZE > src_len, "Length wrong\n");

	ch_enabled = 0;
	for(i = 0; i< CHANNEL_COUNT; i++) {
		parse_channel(src_data, i, ch_data);
		ch_data[i].index = ch_enabled;
		ch_enabled += ch_data[i].enabled;
	}

	ERROR_IF(ch_enabled < 1, "No channels active??\n");
	ERROR_IF(ch_enabled > 4, "Too many channels active??\n");

	sample_count = total_len / ch_enabled;
	ERROR_IF(sample_count != 4000 &&
			sample_count != 40000 &&
			sample_count != 400000 &&
			sample_count != 4000000,
			"Unexpected sample count %u?\n", sample_count);

	sample_rate = atofL((char*)src_data, 79, 9);
	data = src_data + HEADER_SIZE; // after header
	end = src_data + total_len;

	fprintf(output, "index%stime", sep);
	for (ch = 0; ch < CHANNEL_COUNT; ch++)
		if (ch_data[ch].enabled)
			fprintf(output, "%sch%d.raw", sep);
	for (ch = 0; ch < CHANNEL_COUNT; ch++)
		if (ch_data[ch].enabled)
			fprintf(output, "%sch%d.volt", sep);
	fprintf(output, "\n");


	sample_nr = 0;
	while (data < end) {
		for(i = 0; i < CHUNK_SIZE; i++) {
			fprintf(output, "%d%s%f", sample_nr, sep, sample_nr / sample_rate);

			for (ch = 0; ch < CHANNEL_COUNT; ch++)
				if (ch_data[ch].enabled) {
					ch_data[ch].val = data[ch_data[ch].index * CHUNK_SIZE + i];
					fprintf(output, "%s%d", sep, ch_data[ch].ival);
				}

			for (ch = 0; ch < CHANNEL_COUNT; ch++)
				if (ch_data[ch].enabled) {
					fprintf(output, "%s%f", sep,
							(ch_data[ch].ival - ch_data[ch].offset) 
							* ch_data[ch].scale
							/ grid);
				}

			fprintf(output, "\n");
			sample_nr ++;
		}
		data += CHUNK_SIZE * ch_enabled;
	}


	close(dest_fh);
}

int fetch_one(const char *src_dev, const char *dest_pattern) {
	char dest_name[PATH_MAX];
	int src_fh;

	uint8_t *src_data;
	uint32_t src_len;

	sprintf(dest_name, dest_pattern, time(NULL));

	src_fh = open(src_dev, O_RDONLY);
	ERROR_IF(src_fh == -1, "Can't open \"%s\": %d, %s\n", src_dev, errno, strerror(errno));

	src_len = lseek(src_fh, 0, SEEK_END);
	src_data = (uint8_t*)mmap(NULL, src_len, PROT_READ, MAP_SHARED, src_fh, 0);

	ERROR_IF(!src_data, "Can't mmap \"%s\": %d\n", src_dev, errno);

	translate_data(src_data, src_len, dest_name);

	munmap(src_data, src_len);
	close(src_fh);

	return 0;
}


int main(int argc, char *args[]) {
	int r;

	r = glob(device_pattern, GLOB_ERR | GLOB_NOSORT, NULL, &found_devs);
	ERROR_IF(r == GLOB_NOMATCH, "No Devices found.\n");
	ERROR_IF(r != 0, "Error looking for Hantek devices.");
	ERROR_IF(found_devs.gl_pathc != 1, "Not exactly one Hantek device found.");

	fetch_one(found_devs.gl_pathv[0], "/tmp/ff1.csv");
}
