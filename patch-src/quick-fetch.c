/* Patch button "save to usb" to instead cause a ZMODEM transfer of the binary data. */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

void *(fetch_data)(void *x);
// #define DEBUG (void)
#define DEBUG(fmt, ...) printf("%d" fmt, getpid(), ## __VA_ARGS__)

struct io_funcs {
	void *void1;
	void (*write_fn)();
	char pad[32];
} __attribute__((packed));

struct connection {
	char pad1[44];
	struct io_funcs *io_funcs;
	uint32_t write_count;
	char pad2[32];
	uint32_t fd;
	uint32_t repeated_write;
} __attribute__((packed)) ;


int my_write_fn(struct connection *conn, char *buf, uint32_t len) {
//	Single-byte writes for a comma?
	if (len == 1)
		return 1;
//	if (buf == scpi__ignore_write)
//		return 1;


	// Ignore repeated SCPI header
	if (len == 4128 && conn->repeated_write) {
		len -= 128;
		buf += 128;
	}

	conn->repeated_write = 1;

//	DEBUG(" called for writing: %p, %p, %d\n", conn, buf, len);
	return write(conn->fd, buf, len);
}


static uint32_t save_to_usb_fn = 0;
static uint32_t (*scpi__priv_wave_d_all)(struct connection*) = 0;
static uint32_t *scpi__priv_wave_state = 0;
static uint32_t *scpi__data_all_len = 0;
static uint32_t *scpi__data_sum_len = 0;



const char tmp_file[80] = "/tmp/quick-fetch.bin.tmp";
const char result_file[80] = "/tmp/quick-fetch.bin";

void do_save_waveform() {
	int fd;
	void *data;
	time_t t;
	uint32_t len;
	int r, i, end;

	struct io_funcs ios = { .write_fn = (void*)my_write_fn };
	struct connection c = { 
		.io_funcs = &ios, 
		.write_count = 1,
		.repeated_write = 0,
	};

	/* TODO: Put a "gzip" inbetween?
	 * Seems unnecessary, the USB file transfer mode does 8MB in <2secs */

	data = (void*)0x000e5000;
	len  = 0x00b0d000 - (uint32_t)data;
	fd = open( tmp_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd >= 0) {
		/*
		r = ftruncate(fd, 32*1024*1024);
		DEBUG("truncate: %d\n", r);

		time(&t);
		r = pwrite(fd, &t, sizeof(t), 0);
		DEBUG("write 1: %d\n", r);
		r = pwrite(fd, data, len, 0x1000);
		DEBUG("write 2: %d\n", r);
		*/

		c.fd = fd;
		*scpi__priv_wave_state = 1;
		i = 4;
		end = 0;
		do {
//			DEBUG("calling %d: state %x\n", i, *scpi__priv_wave_state);
			r = scpi__priv_wave_d_all(&c);
			if ((*scpi__data_sum_len + 4000) == *scpi__data_all_len)
				end = 1;
			/*
			if (end) {
				i--;
				DEBUG(" got result %p, time %d, state %d; all %u, sum %u\n", 
						r, i, *scpi__priv_wave_state,
						*scpi__data_all_len, *scpi__data_sum_len);
			}
			*/
			if (*scpi__priv_wave_state == 1) 
				break;
		} while (!(end && i==0));

		/* Pad to full 4KB block */
		len = ((*scpi__data_all_len) | (4*1024 -1)) +1;
		ftruncate(fd, len);


		/*
		r = scpi__priv_wave_d_all(&c);
		DEBUG(" after: got result %p, state %d\n", r, *scpi__priv_wave_state);
		*/

		r = close(fd);
		DEBUG("close: %d\n", r);

		r = rename(tmp_file, result_file);
		DEBUG("rename: %d\n", r);

		fd = open("/sys/kernel/config/usb_gadget/g1/functions/mass_storage.0/lun.0/file", O_WRONLY);
		if (fd >= 0) {
			write(fd, result_file, sizeof(result_file) -1);
			close(fd);
		}
	}
}

int new_save_to_usb(void *x)
{
	do_save_waveform();
	return 0;

	/* Using a fresh process would be nice,
	 * to _not_ disturb the real process.
	 * Sadly that doesn't work,
	 * as we copy some (locked) mutexes, too. */
	static pid_t last_child_pid;

	if (last_child_pid)
		kill(last_child_pid, SIGKILL);

	/* Don't disturb working process! */
	last_child_pid = fork();
	if (last_child_pid == -1) {
		DEBUG("can't fork child: %d\n", errno);
		return 0;
	}
	if (last_child_pid)
		return 0; // Parent

	/* Child */
	do_save_waveform();

	/* Avoid ANY cleanups */
	kill(getpid(), SIGKILL);
}

static int did_patch = 0;

int patch_a_jump(int fh, void* my_addr, uint32_t patch_addr)
{
	int32_t diff;
	uint8_t *ptr = (void*)patch_addr;
	int i;

	/* Current address + 4  (next IP)
	 * + 4* delta           (offset)
	 * + 4                  (IP increment)
	 * */

	diff = ((unsigned long)my_addr - 4 - (patch_addr + 4)) / 4;
	DEBUG("fh %d, from %p jump to %p, diff 0x%x\n", fh, patch_addr, my_addr, diff);

	if (diff >= 0x800000 || diff < -0x800000) {
		DEBUG("distance too big\n");
		return 0;
	}

	// "b", ie. branch always == 0xea
	diff = (diff & 0xffffff) | 0xea000000;

	i = pwrite64(fh, &diff, 4, (uint64_t)patch_addr);
	DEBUG("wrote %d = 0x%x: now 0x%x 0x%x 0x%x 0x%x\n",
			i, diff,
			ptr[0],
			ptr[1],
			ptr[2],
			ptr[3]);
	if (i != 4)
		return 0;

	return 1;
}


int detect()
{
	unsigned char *ptr;
	char buffer[80];
	int i;
	const char v200[] = "2.0.0(220517.00)";


	buffer[0] = 0;
	i = readlink("/proc/self/exe", buffer, sizeof(buffer)-1);
	buffer[i] = 0;
	if (strcmp(buffer, "/dso/app/phoenix") != 0) {
//		DEBUG("wrong exe: %s\n", buffer);
		return 0;
	}

	DEBUG("right exe: %s\n", buffer);

	if (strncmp((void*)0xc5da0, v200, sizeof(v200)) == 0) {
		DEBUG("found %s\n", v200);

		save_to_usb_fn = 0x342cc;
		ptr = (void*)save_to_usb_fn;
		if (
				ptr[0] != 0xf0 ||
				ptr[1] != 0x43 ||
				ptr[2] != 0x2d ||
				ptr[3] != 0xe9
		   ) {
			DEBUG("wrong bytes at %p!! %x %x %x %x\n", ptr, ptr[0], ptr[1], ptr[2], ptr[3]);
			return 0;
		}

		scpi__priv_wave_d_all = (void*)0x939f4;
		scpi__priv_wave_state = (void*)0xe1600;
		scpi__data_all_len    = (void*)0x9aed64;
		scpi__data_sum_len    = (void*)0x9a6444;
//		scpi__ignore_write    = (void*)0xab06c;
		return 1;
	}

	return 0;
}



void my_patch_init(int version) {
	int fh;

	if (did_patch)
		return;

	fh = open("/proc/self/mem", O_RDWR);
	if (fh < 0)
		return;


	patch_a_jump(fh, new_save_to_usb, save_to_usb_fn);

	switch (version) {
		case 1:
			/* Remove debug output - that being written to the serial console slows everything down. */
			patch_a_jump(fh, (void*)0x93e70, 0x93e60);
			patch_a_jump(fh, (void*)0x93ad8, 0x93acc);
			patch_a_jump(fh, (void*)0x93b20, 0x93b08);
			break;
	}
	close(fh);
	did_patch = 1;

	signal(SIGPOLL, (void*)do_save_waveform);
	/* Auto reap children */
	signal(SIGCHLD, SIG_DFL);

	return;
}


void __attribute__((constructor)) my_init() 
{
	int i;

	i = detect();
	if (!i)
		return;

	my_patch_init(i);
}

