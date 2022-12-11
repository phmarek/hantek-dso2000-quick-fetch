
/* Creating MDNS packets ourselves.
 * Running the (available) avahi-daemon means 630kB RAM lost -
 * and it doesn't even have a good hostname (with serial#) ... */

#include <stdlib.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <netinet/in.h>


// Multicast Domain Name System (response)
static char mdns_packet[] = {
	0x00, 0x00, // Transaction ID: 0x0000
	0x84, 0x00, /* Flags: 0x8400 Standard query response, No error */
	0x00, 0x00, // Questions: 0
	0x00, 0x01, // Answer RRs: 1
	0x00, 0x00, //    Authority RRs: 0
	0x00, 0x00, //   Additional RRs: 0
		    //    Answers
	0x06, 'h', 'a', 'n', 't', 'e', 'k', // '-', 'a','a','a','a','a','a','a','a','a',
	0x05, 'l', 'o', 'c', 'a', 'l',
	0x00,
	0x00, 0x01, // Type: A (Host Address) (1)
	0x80, 0x01, // Class: IN (0x0001), Cache flush: True
	0x00, 0x00, 0x00, 0x78, // Time to live: 120 (2 minutes)
	0x00, 0x04, // Data length: 4
	0x00, 0x00, 0x00, 0x00 // Address
};
// 15 characters serial number in the name, with a PTR for the non-unique name?

#define IPv4_LEN 4


extern void * anolis_get_sysinfo(void *null);
extern char * anolis_sysinfo_get_serial(void *si);


void *push_mcast_packets(void *x) 
{
	int sock;
	const int len = sizeof(mdns_packet);
	int ret, prev;
	struct ifaddrs *ifap, *ifap2;
	time_t quick_time;
	struct sockaddr_in *si;
	char *ipv4;

	struct sockaddr_in src_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(5353)
	};
	// https://en.wikipedia.org/wiki/Multicast_DNS
	struct sockaddr_in dest_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(5353)
	};
	ipv4 = (void*)&dest_addr.sin_addr.s_addr;
	ipv4[0] = 224;
	ipv4[1] = 0;
	ipv4[2] = 0;
	ipv4[3] = 251;


	quick_time = time(NULL) + 60;
init:
	if (time(NULL) > quick_time) {
		DEBUG("MDNS init failed.\n");
		return NULL;
	}
	sleep(5);

	ret = getifaddrs(&ifap);
	if (ret < 0) {
		DEBUG("can't get local addresses\n");
		goto init;
	}

	ifap2 = ifap;
	/* find non-local IPv4 */
	while (1) {
		if (!ifap) {
			DEBUG("no more interface addresses.\n");
			freeifaddrs(ifap2);
			goto init;
		}

		/* IPv4 only */
		if (strcmp(ifap->ifa_name, "lo") != 0 &&
				ifap->ifa_addr && 
				ifap->ifa_addr->sa_family == AF_INET) {
			si = (void*)ifap->ifa_addr;
			ipv4 = (void*)&si->sin_addr;
			DEBUG("got if %p; addr %p; data %p; si %p; ipv4 %p; on %s %x\n",
					ifap, ifap->ifa_addr, ifap->ifa_addr->sa_data, si, ipv4,
					ifap->ifa_name, si->sin_addr.s_addr);
			memcpy(mdns_packet+len-IPv4_LEN, &si->sin_addr.s_addr, IPv4_LEN);
			freeifaddrs(ifap2);
			break;
		}

		ifap = ifap->ifa_next;
	}

need_sock:
	sock = socket(AF_INET, SOCK_DGRAM, 0); // UDP
	if (sock < 0) {
		DEBUG("Can't create UDP socket.\n");
		goto init;
	}


	ret = bind(sock, &src_addr, sizeof(src_addr));
	if (ret < 0) {
		DEBUG("can't bind src port\n");
		close(sock);
		goto init;
	}


	{ 
		void *si = anolis_get_sysinfo(0);
		if (si) {
			ipv4 = anolis_sysinfo_get_serial(si);
			DEBUG("got serial %c%c%c... %x %x %x",
					ipv4[0], ipv4[1], ipv4[2],
					ipv4[0], ipv4[1], ipv4[2]);
		}
	}


	prev = -9999;
	while (1) {
		// Ignore errors?
		ret = sendto(sock,
				mdns_packet, len, 
				0,
				&dest_addr, sizeof(dest_addr));
		if (ret != prev) {
			prev = ret;
			DEBUG("sendto gave result %d %d\n", ret, errno);
		}
		// more often after starting, to get a packet out as soon as the network is up
		if (time(NULL) <= quick_time)  {
			sleep(2);
		} else {
			sleep(20);
		}
	}
}
