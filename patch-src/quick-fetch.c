#define _GNU_SOURCE
#include <sys/socket.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/syscall.h>      /* Definition of SYS_* constants */
#include <pthread.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <linux/tcp.h>
#include <dirent.h>
#include <errno.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <signal.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
//#include <zlib.h> // compress2


/* Press the key so often in so many seconds to get out of quick mode,
 * ie. to get the USB console back. */
#define QUICK_FETCH_DEACT_TIMEOUT 4
#define QUICK_FETCH_DEACT_COUNT 3

/* TODO: tuneable because of overclocking? */
#define QUICK_FETCH_TOTAL_TIMEOUT 30

#define QUICK_FETCH_PER_PACKET_TIMEOUT 0.5

#define QUICK_FETCH_TCP_PORT 8001


// using gettid() means that GLIBC2.30 is required, which Hantek's libc6 doesn't provide
#define my_gettid(x) ((int)syscall(SYS_gettid))

// #define DEBUG (void)
#define DEBUG(fmt, ...) printf("%d:%d:" fmt, getpid(), my_gettid(), ## __VA_ARGS__)

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
	fd_set select_read_mask;
	fd_set select_send_mask;
	fd_set select_excp_mask;
	struct timeval timeout;
	uint32_t was_empty_transmission;
	int32_t error;
} __attribute__((packed)) ;


static uint32_t save_to_usb_no_udisk_beq = 0;
static uint32_t save_to_usb_code_space = 0;
static uint32_t (*scpi__priv_wave_d_all)(struct connection*) = 0;
static uint32_t *scpi__priv_wave_state = 0;
static uint32_t *scpi__data_all_len = 0;
static uint32_t *scpi__data_sum_len = 0;


static int32_t *usb_mode__is_peripheral = 0;



int global_debug = 0;

extern uint32_t anolis_make_toast(const char msg[]);
void *_show_some_alert(void* msg) {
	anolis_make_toast((char*)msg);
	return NULL;
}
void show_some_alert_async(const char msg[]) {
	pthread_t thr;

	if (0 == pthread_create(&thr, NULL, _show_some_alert, (void*)msg)) {
		/* Instead of join */
		DEBUG("alert(%s) in pthread %p\n", msg, thr);
		pthread_detach(thr);
	}
}



int my_write_fn(struct connection *conn, char *buf, uint32_t len) {
	uint32_t i;
	time_t now;
	int ret;
	static char safe[12] = { 0 };
	char x;

	/* We expect the max. 8MSamples to be transferred in ~15 seconds; */
	time(&now);
	i = now - conn->start_time;
	if (i > QUICK_FETCH_TOTAL_TIMEOUT) {
		DEBUG("timeout over total data.\n");
		conn->is_timeout = 2;
		show_some_alert_async("Timeout (2) sending data to Quick Fetch software.");
		return len;
	}

	for (i = 0; i<sizeof(safe)-1; i++) {
		x = buf[i];

		safe[i] = i >= len ? '.' :
			(x >= ' ' && x <= 0x7e) ? x : '.';
	}

	if(global_debug)
		DEBUG(" writing at %d: %4d bytes, total %7d; timeout %d, state %d, buf '%s'\n", 
				(int)now, len, conn->my_write_counter,
				conn->is_timeout, 
				*scpi__priv_wave_state, 
				safe);

	if (conn->is_timeout)
		return len;

//	Single-byte writes for a comma? Perhaps some SCPI start signal?
	if (len == 1)
		return len;

	if (len == 11 && memcmp(buf, "#9000000000", 11) == 0) {
		if (global_debug)
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
	FD_SET(conn->fd, &conn->select_send_mask);
	FD_SET(conn->fd, &conn->select_excp_mask);
	conn->timeout.tv_sec = (int)(QUICK_FETCH_PER_PACKET_TIMEOUT);
	conn->timeout.tv_usec = (int)(QUICK_FETCH_PER_PACKET_TIMEOUT * 1000000) % 1000000;
	i = select(conn->fd+1, NULL, &conn->select_send_mask, &conn->select_excp_mask, &conn->timeout);
	if (!i) {
		DEBUG("Plain timeout, left %ld.%06ld.\n", 
				conn->timeout.tv_sec, conn->timeout.tv_usec);
		conn->is_timeout = 1;
		show_some_alert_async("Timeout (1) sending data to Quick Fetch software.");
		return len; // abort
	}
	if (FD_ISSET(conn->fd, &conn->select_send_mask)) {
		// Expected
	} else if (FD_ISSET(conn->fd, &conn->select_excp_mask)) {
		DEBUG("exception on USB fd\n");
		conn->is_timeout = 1;
		show_some_alert_async("Timeout (3) sending data to Quick Fetch software.");
		return len; // abort
	}

	ret = write(conn->fd, buf, len);
	if (ret < 0)
		conn->error = errno;

	return ret;
}



static int console_is_stopped = 0;
static int ttygs0_serial_fd = -1;
static int tcp_listening_port_fd = -1;
static int tcp_fd = -1;

#define communication_port "/dev/ttyGS0"

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


/* Find out whether there's a QF client connected. */
const char *ping_pong(struct connection *conn)
{
	const char ping[]="qf.ping ";
	const char pong[]="qf.pong";
	const char pung[]="qf.pung";
	char buf[400], *cp, *cp2;
	int got, len, half;
	int tag, my_tag;
	struct timespec rcvd_time;

	len = 0;
	half = sizeof(buf)/2;
	while (1) {
		/* Get last few bytes from the serial line; the QF client should have put its
		 * ping on there long ago. */
		FD_SET(conn->fd, &conn->select_read_mask);
		FD_SET(conn->fd, &conn->select_excp_mask);
		conn->timeout.tv_sec = 0;
		conn->timeout.tv_usec = 100;
		got = select(conn->fd+1, &conn->select_read_mask, NULL, &conn->select_excp_mask, &conn->timeout);
		if (!got) 
			break;

		/* USB port trouble? */
		if (!FD_ISSET(conn->fd, &conn->select_read_mask))
			return "Error reading line (2)";

		got = read(conn->fd, buf+len, half-1);
		if (got < 0)
			return "Error reading line";
		buf[got+len] = 0;
		DEBUG("ping data: had %d, got %d of %d: %s\n", len, got, half, buf);
		len += got;
		if (len > half) {
			/* 0        len        len+got 
			 * |OLDOLDold'NEWNEWNEW'____________|
			 *         /         /
			 *        /         /  move to low
			 *       /         /
			 * |old'NEWNEWNEW'__________________|
			 * 0            half
			 */
			memmove(buf, buf+len+got-half, half);
			len = half;
			buf[len] = 0;
			DEBUG("ping data moved to %d: %s\n", len, buf);
		}
	}

	if (len < sizeof(ping)+6)
		return "No Quickfetch client attached?";

	buf[len] = 0;
	/* Find last one */
	cp = strstr(buf, ping);
	while (cp) {
		cp2 = strstr(cp+sizeof(ping)-2, ping);
		if (!cp2) 
			break;
		cp = cp2;
	}

	if (!cp)
		return "No QF ping";

	tag = 0;
	got = sscanf(cp, "%*s%d", &tag);
	DEBUG("ping: got %d, tag %d\n", got, tag);
	if (!got || tag == 0)
		return "No QF ping tag";


	my_tag = random();
	len = sprintf(buf, "\n%s %d %d\n", pong, tag, my_tag);
	got = write(conn->fd, buf, len);
	DEBUG("pong: sent %d, %s\n", got, buf);
	if (got != len)
		return "Error sending QF pong";

	/* 30msec timeout */
	FD_SET(conn->fd, &conn->select_read_mask);
	FD_SET(conn->fd, &conn->select_excp_mask);
	conn->timeout.tv_sec = 0;
	conn->timeout.tv_usec = 30e3;
	got = select(conn->fd+1, &conn->select_read_mask, NULL, &conn->select_excp_mask, &conn->timeout);
	if (!got) 
		return "QF pong timeout";
	if (!FD_ISSET(conn->fd, &conn->select_read_mask))
		return "QF pong timeout 2";

	len = read(conn->fd, buf, sizeof(buf)-1);
	buf[len] = 0;
	DEBUG("pung: got %d, %s\n", len, buf);
	if (len < sizeof(pung))
		return "QF pong receive error";

	tag = 0;
	rcvd_time.tv_sec = 0;
	got = sscanf(buf, "%*s %d %ld", &tag, &rcvd_time.tv_sec);
	DEBUG("pung: tag %d, time %s\n", tag, ctime(&rcvd_time.tv_sec));
	if (!got || tag != my_tag)
		return "QF pong bad data";

	if (rcvd_time.tv_sec) {
		rcvd_time.tv_nsec = 0; /* Not really relevant here */
		clock_settime(CLOCK_REALTIME, &rcvd_time);
	}

	return NULL;
}



int actually_do_save_waveform(int fd, int do_pingpong) 
{
	void *data;
	time_t t;
	uint32_t len;
	int r;
	const char *err;

	static struct io_funcs ios = { .write_fn = (void*)my_write_fn };
	struct connection c = { 0 };
	FD_ZERO(&c.select_read_mask);
	FD_ZERO(&c.select_send_mask);
	FD_ZERO(&c.select_excp_mask);


	/* TODO: Put a "gzip" inbetween?
	 * Seems unnecessary, the USB file transfer mode does 8MB in <2secs */

	data = (void*)0x000e5000;
	len  = 0x00b0d000 - (uint32_t)data;
	if (fd == -1) {
		DEBUG("No communication open!?!?!");
		return 0;
	} else {
		c.io_funcs = &ios; 
		c.write_count = 1;
		c.my_write_counter = 0;
		c.error = 0;
		c.fd = fd;

		if (do_pingpong) {
			err = ping_pong(&c);
			if (err) {
				DEBUG("pingpong: %s\n", err);
				show_some_alert_async(err);
				return 0;
			}
		}

		/* Handshake fixed the time, so get new value only now. */
		time(&c.start_time);

		*scpi__priv_wave_state = 1;
		DEBUG(" state %d; all %u, sum %u\n", *scpi__priv_wave_state, *scpi__data_all_len, *scpi__data_sum_len);
		do {
			do {
				c.was_empty_transmission = 0;
				r = scpi__priv_wave_d_all(&c);
			}
			while (c.was_empty_transmission);

		} while ( *scpi__priv_wave_state != 1);

		if (c.error) {
			static char buf[200];
			sprintf(buf, "QF error during communication: %d, %s", 
					c.error,
					strerror(c.error));
			show_some_alert_async(buf);
			return 0;
		}

		// Success
		return 1;
	}
}

int is_socket_alive(int fd)
{
	struct tcp_info ti;
	socklen_t len = sizeof(ti);

	// not in that old linux/tcp.h
#ifndef SOL_TCP
#define SOL_TCP 6
#endif
	if (getsockopt(fd, SOL_TCP, TCP_INFO, &ti, &len) == -1) {
		DEBUG("can't get state for socket on %d\n", fd);
		return 0;
	}

	DEBUG("socket %d reported with state %d\n", fd, ti.tcpi_state);

	// https://unix.stackexchange.com/questions/470174/what-is-the-meaning-of-the-connection-state-constants-in-proc-net-tcp
	return ti.tcpi_state == 1;
}


int do_save_waveform() 
{
	int r;
	socklen_t len;
	struct pollfd pfd;
	struct sockaddr_in si;
	char remote[INET6_ADDRSTRLEN+1];

	/* We need to loop - there might be several TCP connections
	 * that got already closed again. */
	while (1) {
		/* Look for TCP connection */
		pfd.fd = tcp_listening_port_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		r = poll(&pfd, 1, 0);
		if (!(pfd.revents & POLLIN)) {
			DEBUG("Nothing to accept\n");
			break;
		}

		len = sizeof(si);
		tcp_fd = accept(tcp_listening_port_fd, &si, &len);
		if (tcp_fd == -1)
			break;

		/* TODO: ipv6? */
		if (si.sin_family == AF_INET)
			inet_ntop(AF_INET, &si.sin_addr, 
					remote, sizeof(remote));
		else
			break;

		// check for still active (ie. not closed again!)
		if (is_socket_alive(tcp_fd)) {

			// check for still active II
			pfd.fd = tcp_fd;
			pfd.events = POLLIN | POLLOUT | POLLERR | POLLHUP;
			pfd.revents = 0;
			r = poll(&pfd, 1, 0);
			if ((pfd.revents & POLLOUT) && !(pfd.revents & (POLLERR | POLLHUP))) {
				DEBUG("got a TCP connection to write to -- fd %d, [%s]:%d\n",
						tcp_fd, remote, ntohs(si.sin_port));
				r = actually_do_save_waveform(tcp_fd, 0);
				shutdown(tcp_fd, SHUT_RDWR);
				close(tcp_fd);
				tcp_fd = -1;
				return r;
			}

		}

		DEBUG("broken TCP connection fd %d [%s]:%d\n",
				tcp_fd, remote, ntohs(si.sin_port));
		// broken TCP connection
		close(tcp_fd);
		tcp_fd = -1;
	}

	// No valid TCP, try serial? 
	if (ttygs0_serial_fd) {
		DEBUG("Using serial connection on fd %d\n", ttygs0_serial_fd);
		return actually_do_save_waveform(ttygs0_serial_fd, 1);
	}

	DEBUG("No connection available\n");
	return 0;
}


int new_save_to_usb(void *x)
{
	static time_t pressed_time;
	static int pressed_count = 0;

	/* Check for USB connection to PC */
	if (*usb_mode__is_peripheral == 0) {
		show_some_alert_async("No USB device (stick or PC) found");
		return 0;
	}

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
				pressed_count = 0;
				DEBUG("leaving quick fetch mode\n");
				/* Kill the processes hard - init will restart a clean getty. */
				send_signal_to_console_processes(communication_port, SIGKILL);
				console_is_stopped = 0;
				if (ttygs0_serial_fd >= 0)
					close(ttygs0_serial_fd);
				ttygs0_serial_fd = -1;
				show_some_alert_async("Leaving quick fetch mode.");
				return 0;
			}
			/* Unless it was the third time in quick succession, return data. */
		} else {
			/* First click after some time; get a dump. */
			DEBUG("quick fetch do at %ld\n", now.tv_sec);
			pressed_time = now.tv_sec;
		}

		/* Only reset when successful. */
		if (do_save_waveform())
			pressed_count = 0;
	} else {
		/* Console is active; stop it (so that it doesn't get restarted by init) and ... */
		DEBUG("activating quick fetch mode\n");
		send_signal_to_console_processes(communication_port, SIGSTOP);
		console_is_stopped = 1;
		pressed_time = now.tv_sec;
		show_some_alert_async("Activating quick fetch mode.");

		/* Undo "damage" done by getty (eg crlf translation) */
		system("stty raw pass8 < " communication_port);
		ttygs0_serial_fd = open(communication_port, O_RDWR);
		//		write(ttygs0_serial_fd, init_msg, strlen(init_msg));
		/* TODO: start thread that waits for commands ?! */
		/* TODO: Message to screen saying now active */
	}

	return 1;
}

static int did_patch = 0;

#define OPCODE_UNCOND_JUMP (0xea000000)
#define OPCODE_UNCOND_CALL (0xeb000000)
#define OPCODE_IF_EQU_JUMP (0x0a000000)

int patch_a_code(int fh, uint32_t my_code, uint32_t patch_addr)
{
	uint8_t *ptr = (void*)patch_addr;
	int i;

	i = pwrite64(fh, &my_code, sizeof(my_code), (uint64_t)patch_addr);
	DEBUG("wrote %d bytes (0x%x) at 0x%x: now 0x%x 0x%x 0x%x 0x%x\n",
			i, my_code, patch_addr,
			ptr[0],
			ptr[1],
			ptr[2],
			ptr[3]);
	if (i != 4)
		return 0;

	return 1;
}


int patch_a_jump(int fh, void* my_addr, uint32_t patch_addr, int opcode)
{
	int64_t dist;
	uint32_t jmp_val;
	uint32_t jmp;
	int i;

	/* Current address + 4  (next IP)
	 * + 4* delta           (offset)
	 * + 4                  (IP increment)
	 * */

	dist = ((unsigned long)my_addr - 4 - (patch_addr + 4));
	jmp_val = (unsigned long)dist / 4;
	DEBUG("fh %d, from %p jump to %p, dist 0x%llx, jmp 0x%x\n",
			fh, patch_addr, my_addr, dist, jmp_val);

	if (abs(dist) >= 0x7fff00*4) {
		DEBUG("distance too big\n");
		return 0;
	}

	// "b", ie. branch always == 0xea
	jmp = (jmp_val & 0xffffff) | opcode;

	return patch_a_code(fh, jmp, patch_addr);
}


int detect()
{
	uint32_t *ptr;
	char buffer[80];
	int i;
	const char v200_202205[] = "2.0.0(220517.00)";
	const char v200_202210[] = "2.0.0(221028.00)";
	const char v300_202303[] = "3.0.0(230327.00)";


	buffer[0] = 0;
	i = readlink("/proc/self/exe", buffer, sizeof(buffer)-1);
	buffer[i] = 0;
	if (strcmp(buffer, "/dso/app/phoenix") != 0) {
		//		DEBUG("wrong exe: %s\n", buffer);
		return 0;
	}

	DEBUG("right exe: %s\n", buffer);

	if (strncmp((void*)0xc5f28, v300_202303, sizeof(v300_202303)) == 0) {
		DEBUG("found %s\n", v300_202303);

		save_to_usb_no_udisk_beq = 0x342d8;
		ptr = (void*)save_to_usb_no_udisk_beq;
		if (*ptr != 0x0a000078) {
			DEBUG("wrong bytes at %p!! 0x%x\n", ptr, *ptr);
			return 0;
		}
		save_to_usb_code_space = 0x344fc;

		scpi__priv_wave_d_all   = (void*)0x93938;
		scpi__priv_wave_state   = (void*)0xe1604;
		scpi__data_all_len      = (void*)0x9aed7c;
		scpi__data_sum_len      = (void*)0x9a645c;
		usb_mode__is_peripheral = (void*)0x9aed4c;

		return 3;
	}

	if (strncmp((void*)0xc5d30, v200_202210, sizeof(v200_202210)) == 0) {
		DEBUG("found %s\n", v200_202210);

		save_to_usb_no_udisk_beq = 0x34354;
		ptr = (void*)save_to_usb_no_udisk_beq;
		if (*ptr != 0x0a000078) {
			DEBUG("wrong bytes at %p!! 0x%x\n", ptr, *ptr);
			return 0;
		}
		save_to_usb_code_space = 0x34578;

		scpi__priv_wave_d_all   = (void*)0x93970;
		scpi__priv_wave_state   = (void*)0xf0648;
		scpi__data_all_len      = (void*)0x9bddac;
		scpi__data_sum_len      = (void*)0x9b548c;
		usb_mode__is_peripheral = (void*)0x9bdd7c;

		return 2;
	}

	show_some_alert_async("This firmware version is not supported "
			"by the quick fetch patch.");
	return 0;
}


void switch_debug_output(int x)
{
	global_debug = 1- global_debug;
	DEBUG("verbose debug flag now %d\n", global_debug);
}


void *patch_init_delayed(void * ignore)
{
	// Wait until hardware got initialized, to not interfere.
	sleep(4);

	/* Cleanup USB console processes.
	 * At this point after reboot, just a newly started getty. */
	send_signal_to_console_processes(communication_port, SIGKILL);

	signal(SIGPOLL, (void*)do_save_waveform);
	/* Auto reap children */
	signal(SIGCHLD, SIG_DFL);
	signal(SIGUSR2, switch_debug_output);

	/* Needed for TCP sockets, mostly. */
	signal(SIGPIPE, SIG_IGN);

	return NULL;
}

#include "mdns.c"

void my_patch_init(int version) {
	int fh;
	pthread_t thr;
	struct sockaddr_in si = { 0 };
	int ret;

	if (did_patch)
		return;

	fh = open("/proc/self/mem", O_RDWR);
	if (fh < 0)
		return;

	// We patch the code immediately before it's being used... */

	/* "Save to USB" key: If no USB stick present, use the new code to save to a PC. */
	/* Fix up stack: We need to fix the pushed registers, and the local variables.
	 * anolis_is_udisk_mounted() changes R0 and R3, which wouldn't matter;
	 * but the call changes LR which we need to restore, so just adding to SP
	 * isn't enough.
	 * And the error path comes in here, too, so there's not enough space for
	 * an LDMIA, an ADD, and a jump.
	 *
	 * But in the same functions there's a check for the display;
	 * on this embedded system, I don't think this will ever trigger, so we
	 * reuse that space. 
	 *     beq space */
	patch_a_jump(fh, (void*)save_to_usb_code_space, save_to_usb_no_udisk_beq, OPCODE_IF_EQU_JUMP);
	/* 	   add   sp,sp,#0x234
	 *     ldmia sp!, {r4, r5, r6, r7, r8, r9, lr}
	 *     b new_save_to_usb */
	patch_a_code(fh, 0xe28ddf8d,      save_to_usb_code_space +0);
	patch_a_code(fh, 0xe8bd43f0,      save_to_usb_code_space +4);
	patch_a_jump(fh, new_save_to_usb, save_to_usb_code_space +8, OPCODE_UNCOND_JUMP);

	switch (version) {
		case 2:
			/* Remove debug output - that being written to the serial console slows everything down. */
			patch_a_jump(fh, (void*)0x93de8, 0x93dd8, OPCODE_UNCOND_JUMP);
			patch_a_jump(fh, (void*)0x93a50, 0x93a44, OPCODE_UNCOND_JUMP);
			patch_a_jump(fh, (void*)0x93a98, 0x93a80, OPCODE_UNCOND_JUMP);

			/* Patch slowdown during LWF file write to USB stick
			 *   0006a03c 5e c1 fe eb    bl   <EXTERNAL>::fwrite                               size_t fwrite(void * __ptr, size
			 *   0006a040 2c 01 9f e5    ldr  out_file=>DAT_000c5f54,[PTR_DAT_0006a174]        = 000c5f54
			 *                                                                                 = 73h    s
			 *   0006a044 5e bf fe eb    bl   <EXTERNAL>::system                               int system(char * __command)
			 *   0006a048 fa 0f a0 e3    mov  out_file,#0x3e8
			 *
			 * We need to keep this in:
			 *   0006a04c 09 80 88 e0    add  r8,r8,r9
			 *
			 * This gets removed again:
			 *   0006a050 a1 c1 fe eb    bl   <EXTERNAL>::usleep                               int usleep(__useconds_t __usecon
			 *   0006a054 ee ff ff ea    b    LAB_0006a014
			 *
			 * Yeah, we could move the ADD up and write a single jump.
			 */			patch_a_jump(fh, (void*)0x6a09c, 0x6a090, OPCODE_UNCOND_JUMP);
			patch_a_jump(fh, (void*)0x6a064, 0x6a0a0, OPCODE_UNCOND_JUMP);

			patch_a_jump(fh, (void*)0x6a1a4, 0x6a1a0, OPCODE_UNCOND_JUMP);
			break;
		case 3:
			patch_a_jump(fh, (void*)0x93db0, 0x93da0, OPCODE_UNCOND_JUMP);
			patch_a_jump(fh, (void*)0x93a18, 0x93a0c, OPCODE_UNCOND_JUMP);
			patch_a_jump(fh, (void*)0x93a60, 0x93a48, OPCODE_UNCOND_JUMP);

			patch_a_jump(fh, (void*)0x6a04c, 0x6a040, OPCODE_UNCOND_JUMP);
			patch_a_jump(fh, (void*)0x6a014, 0x6a050, OPCODE_UNCOND_JUMP);

			patch_a_jump(fh, (void*)0x6a154, 0x6a150, OPCODE_UNCOND_JUMP);
			break;
	}
	close(fh);
	did_patch = 1;

	if (0 == pthread_create(&thr, NULL, patch_init_delayed, NULL)) {
		pthread_detach(thr);
	}
	if (0 == pthread_create(&thr, NULL, push_mcast_packets, NULL)) {
		pthread_detach(thr);
	}

	tcp_listening_port_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (tcp_listening_port_fd != -1) {
		si.sin_family = AF_INET;
		// si.sin_addr  is 0.0.0.0
		si.sin_port = htons(QUICK_FETCH_TCP_PORT);

		ret = bind(tcp_listening_port_fd, &si, sizeof(si));
		if (ret != 0) {
			DEBUG("Can't bind TCP port %d\n", QUICK_FETCH_TCP_PORT);
			close(tcp_listening_port_fd);
			tcp_listening_port_fd = -1;
		} else {
			// Multiple connections could be waiting if the client was aborted!!
			ret = listen(tcp_listening_port_fd, 10);
			if (ret != 0) {
				DEBUG("Can't listen on TCP port %d\n", QUICK_FETCH_TCP_PORT);
				close(tcp_listening_port_fd);
				tcp_listening_port_fd = -1;
			} else {
				DEBUG("TCP port %d waiting\n", QUICK_FETCH_TCP_PORT);
			}
		}
	}

	return;
}



void *detect_and_patch(void * ignore)
{
	int i;

	i = detect();
	if (i) {
		my_patch_init(i);
	}
	return NULL;
}

void __attribute__((constructor)) my_init()  {
	detect_and_patch(NULL);
}

/* vim: set ts=4 sw=4 noet  : */
