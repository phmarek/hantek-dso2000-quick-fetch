/* Patch button "save to usb" to instead cause a ZMODEM transfer of the binary data. */

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/select.h>
#include <sys/syscall.h>      /* Definition of SYS_* constants */
#include <pthread.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
//#include <zlib.h> // compress2


/* Press the key so often in so many seconds to get out of quick mode,
 * ie. to get the USB console back. */
#define QUICK_FETCH_DEACT_TIMEOUT 2
#define QUICK_FETCH_DEACT_COUNT 3

/* TODO: tuneable because of overclocking? */
#define QUICK_FETCH_TOTAL_TIMEOUT 30

#define QUICK_FETCH_PER_PACKET_TIMEOUT 0.5

// #define DEBUG (void)
#define DEBUG(fmt, ...) printf("%d:" fmt, getpid(), ## __VA_ARGS__)

struct io_funcs {
	void *void1;
	void (*write_fn)();
	char pad[32];
} __attribute__((packed));

struct connection {
	uint32_t pad1[11];
	struct io_funcs *io_funcs;
	uint32_t write_count;
	uint32_t pad2[8];

	uint32_t fd __attribute__ ((aligned (8)));
	uint32_t my_write_counter;
	time_t start_time;
	uint32_t is_timeout;
	fd_set select_mask;
	struct timeval timeout;
	uint32_t was_empty_transmission;
} __attribute__((packed)) ;


int global_debug = 0;
extern uint32_t anolis_make_toast(const char msg[]);
void show_some_alert(const char msg[]) {
	anolis_make_toast(msg);
}


int my_write_fn(struct connection *conn, char *buf, uint32_t len) {
	int i;
	time_t now;

	if (conn->is_timeout)
		return len;

//	Single-byte writes for a comma? Perhaps some SCPI start signal?
	if (len == 1)
		return len;

	if (len == 11 && memcmp(buf, "#9000000000", 11) == 0) {
		DEBUG("empty packet.\n");
		conn->was_empty_transmission = 1;
		return len;
	}

	// Ignore repeated SCPI header
	if (len == 4128 && conn->my_write_counter) {
		len -= 128;
		buf += 128;
	}

	conn->my_write_counter += len;

	/* Timeout handling -- if the receiver crashes or just does not exist, we stop transmitting. */
	FD_SET(conn->fd, &conn->select_mask);
	conn->timeout.tv_sec = (int)(QUICK_FETCH_PER_PACKET_TIMEOUT);
	conn->timeout.tv_usec = (int)(QUICK_FETCH_PER_PACKET_TIMEOUT * 1000000) % 1000000;
	i = select(conn->fd+1, NULL, &conn->select_mask, NULL, &conn->timeout);
	if (!i) {
		DEBUG("Plain timeout, left %ld.%06ld.\n", conn->timeout.tv_sec, conn->timeout.tv_usec);
		conn->is_timeout = 1;
		show_some_alert("Timeout (1) sending data to Quick Fetch software.");
	}

	/* We expect the max. 8MSamples to be transferred in ~15 seconds; */
	time(&now);
	i = now - conn->start_time;
	if (i > QUICK_FETCH_TOTAL_TIMEOUT) {
		DEBUG("timeout over total data.\n");
		conn->is_timeout = 2;
		show_some_alert("Timeout (2) sending data to Quick Fetch software.");
	}

	if (conn->is_timeout)
		return len; // Abort!

	if(global_debug)
		DEBUG(" called for writing: %p, %p, %d\n", conn, buf, len);
	return write(conn->fd, buf, len);
}


static uint32_t save_to_usb_fn = 0;
static uint32_t (*scpi__priv_wave_d_all)(struct connection*) = 0;
static uint32_t *scpi__priv_wave_state = 0;
static uint32_t *scpi__data_all_len = 0;
static uint32_t *scpi__data_sum_len = 0;



static int console_is_stopped = 0;
static int communication_fd = -1;

const char communication_port[] = "/dev/ttyGS0";

const char init_msg[] = "INIT\n";
const char dump_msg[] = "DUMP\n";

int is_process_using_device(pid_t pid, const char dev[])
{
	char buffer[80];
	DIR * fds;
	struct dirent *de;
	int ret;

	sprintf(buffer, "/proc/%u/fd/", pid);
	fds = opendir(buffer);
	if (fds) {
		while (1) {
			de = readdir(fds);
			if (!de) break;

			sprintf(buffer, "/proc/%u/fd/%s", pid, de->d_name);
			ret = readlink(buffer, buffer, sizeof(buffer)-1);
			if (ret >= 0) {
				buffer[ret] = 0;
//				DEBUG("pid %u fd %s is %s \n", pid, de->d_name, buffer);
				if (strcmp(buffer, dev) == 0) {
					closedir(fds);
					return 1;
				}
			}
		}
		closedir(fds);
	}

	/* May already have been gone again */
	return 0;
}


/* ttyACM0 / ttyGS0 doesn't seem to have any out-of-band signalling functions;
 * ie. if the PC closes ttyACM0, the DSO doesn't see an EOF on ttyGS0.
 *
 * So, to avoid messing with the USB console,
 * on activation of the quick fetch module we SIGSTOP any processes using ttyGS0;
 * and, by pressing the key a few times in quick succession, the user can
 * re-enable the processes to make the USB console usable again. */
int send_signal_to_console_processes(const char dev[], int signal) {
	DIR * processes;
	int count;
	int pid;
	struct dirent *de;

	count = 0;
	processes = opendir("/proc/");
	if (processes) {
		while (1) {
			de = readdir(processes);
			if (!de) break;

			if (de->d_type != DT_DIR)
				continue;

			/* Is that a process directory? */
			pid = atoi(de->d_name);
			if (!pid)
				continue;
			if (pid == getpid())
				continue;

//			DEBUG("process %d\n", pid);
			if (is_process_using_device(pid, dev)) {
				count++;
				DEBUG("sending signal %d to %d\n", signal, pid);
				kill(pid, signal);
			}
		}
		closedir(processes);
	}

	return count;
}


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
		.my_write_counter = 0,
	};
	time(&c.start_time);
	FD_ZERO(&c.select_mask);

	/* TODO: Put a "gzip" inbetween?
	 * Seems unnecessary, the USB file transfer mode does 8MB in <2secs */

	data = (void*)0x000e5000;
	len  = 0x00b0d000 - (uint32_t)data;
	fd = communication_fd;
	if (fd == -1) {
		DEBUG("No communication open!?!?!");
	} else if (fd >= 0) {
//		write(fd, dump_msg, strlen(dump_msg));

		c.fd = fd;
		*scpi__priv_wave_state = 1;
		i = 4;
		end = 0;
		DEBUG(" state %d; all %u, sum %u\n", *scpi__priv_wave_state, *scpi__data_all_len, *scpi__data_sum_len);
		do {
//			DEBUG("calling %d: state %x\n", i, *scpi__priv_wave_state);
			do {
				c.was_empty_transmission = 0;
				r = scpi__priv_wave_d_all(&c);
			}
			while (c.was_empty_transmission);

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

		/*
		r = scpi__priv_wave_d_all(&c);
		DEBUG(" after: got result %p, state %d\n", r, *scpi__priv_wave_state);
		*/

//		r = close(fd);
		DEBUG("close: %d\n", r);
	}
}

int new_save_to_usb(void *x)
{
	static time_t pressed_time;
	static int pressed_count = 0;

	struct timespec now;
	/* Not unlikely that someone/something sets the wall clock time;
	 * then time() would be wrong. */
	clock_gettime(CLOCK_MONOTONIC, &now);

	if (console_is_stopped) {
		DEBUG("key: old time %ld, now %ld, count %d\n", pressed_time, now.tv_sec, pressed_count);
		/* Does the user want the USB console back? */
		if (pressed_time + QUICK_FETCH_DEACT_TIMEOUT >= now.tv_sec) {
			pressed_count++;
			if (pressed_count >= QUICK_FETCH_DEACT_COUNT) {
				DEBUG("leaving quick fetch mode\n");
				/* Kill the processes hard - init will restart a clean getty. */
				send_signal_to_console_processes(communication_port, SIGKILL);
				console_is_stopped = 0;
				if (communication_fd >= 0)
					close(communication_fd);
				communication_fd = -1;
				show_some_alert("Leaving quick fetch mode.");
			}
			return 0;
		} else {
			/* First click after some time; get a dump. */
			DEBUG("quick fetch do at %ld\n", now.tv_sec);
			pressed_time = now.tv_sec;
			pressed_count = 0;
			do_save_waveform();
		}
	} else {
		/* Console is active; stop it (so that it doesn't get restarted by init) and ... */
		DEBUG("activating quick fetch mode\n");
		show_some_alert("Activating quick fetch mode.");
		send_signal_to_console_processes(communication_port, SIGSTOP);
		console_is_stopped = 1;
		pressed_time = now.tv_sec;

		communication_fd = open(communication_port, O_RDWR);
//		write(communication_fd, init_msg, strlen(init_msg));
		/* TODO: start thread that waits for commands ?! */
		/* TODO: Message to screen saying now active */
	}

	return 1;

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
	int64_t dist;
	uint32_t jmp_val;
	uint32_t jmp;
	uint8_t *ptr = (void*)patch_addr;
	int i;

	/* Current address + 4  (next IP)
	 * + 4* delta           (offset)
	 * + 4                  (IP increment)
	 * */

	dist = ((unsigned long)my_addr - 4 - (patch_addr + 4));
	jmp_val = (unsigned long)dist / 4;
	DEBUG("fh %d, from %p jump to %p, dist 0x%x, jmp 0x%x\n", fh, patch_addr, my_addr, dist, jmp_val);

	if (abs(dist) >= 0x7fff00) {
		DEBUG("distance too big\n");
		return 0;
	}

	// "b", ie. branch always == 0xea
	jmp = (jmp_val & 0xffffff) | 0xea000000;

	i = pwrite64(fh, &jmp, 4, (uint64_t)patch_addr);
	DEBUG("wrote %d = 0x%x: now 0x%x 0x%x 0x%x 0x%x\n",
			i, jmp,
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

	show_some_alert("This firmware version is not supported "
			"by the quick fetch patch.");
	return 0;
}


void switch_debug_output(int x)
{
	global_debug = 1- global_debug;
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

			/* Patch slowdown during LWF file write to USB stick
			 *   0006a03c 5e c1 fe eb    bl   <EXTERNAL>::fwrite                               size_t fwrite(void * __ptr, size
			 *   0006a040 2c 01 9f e5    ldr  out_file=>DAT_000c5f54,[PTR_DAT_0006a174]        = 000c5f54
			 *                                                                                 = 73h    s
			 *   0006a044 5e bf fe eb    bl   <EXTERNAL>::system                               int system(char * __command)
			 *   0006a048 fa 0f a0 e3    mov  out_file,#0x3e8
			 *   0006a04c 09 80 88 e0    add  r8,r8,r9
			 *   0006a050 a1 c1 fe eb    bl   <EXTERNAL>::usleep                               int usleep(__useconds_t __usecon
			 *   0006a054 ee ff ff ea    b    LAB_0006a014
			 */
			patch_a_jump(fh, (void*)0x6a014, 0x6a054);

			break;
	}
	close(fh);
	did_patch = 1;

	/* Cleanup old USB console processes */
	send_signal_to_console_processes(communication_port, SIGKILL);

	signal(SIGPOLL, (void*)do_save_waveform);
	/* Auto reap children */
	signal(SIGCHLD, SIG_DFL);
	signal(SIGUSR2, switch_debug_output);

	return;
}


void *detect_and_patch(void * ignore)
{
	int i;

	// Wait until hardware got initialized, to not interfere.
	sleep(8);
	i = detect();
	if (i) {

		my_patch_init(i);
		//show_some_alert("Quick-fetch patch active");
	}
	return NULL;
}

void __attribute__((constructor)) my_init()  {
	pthread_t thr;
	if (0 == pthread_create(&thr, NULL, detect_and_patch, NULL)) {
		pthread_detach(thr);
	}
}

/* vim: set ts=4 sw=4 noet  : */
