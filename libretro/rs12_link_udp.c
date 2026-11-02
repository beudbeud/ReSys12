// ReSys12: cabinet link transport for the kartduel network board (s12_link.c), after the System 23 core's S23Link:
// hellos broadcast on the link port and the 7 ports above it (cabinets on one machine bind the next free one), frames
// unicast to the cabinet heard, a sequence number drops a copy. GPL-2.0-or-later.
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "s12_link.h"

enum { HELLO = 1, FRAME = 2, PORTS = 8 };
#define MAGIC 0x52313231u // "R121"

static int s_sock = -1;
static uint16_t s_port;
static uint32_t s_id, s_seq, s_peer_seq;
static struct sockaddr_in s_peer;
static int s_have_peer;
static int64_t s_hello_ms, s_peer_seen_ms;
static int s_expect;   // a frame went out: the ring answers within microseconds on the real board
static int s_wait_ms = 2; // how long the emulation waits for that answer (RS12_LINK_WAIT_MS)

static int64_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void send_to(const uint8_t *pkt, int n, uint32_t addr, uint16_t port)
{
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = addr;
	to.sin_port = htons(port);
	sendto(s_sock, pkt, (size_t)n, 0, (struct sockaddr *)&to, sizeof(to));
}

static void hello(void)
{
	uint8_t pkt[13];
	put32(pkt, MAGIC);
	pkt[4] = HELLO;
	put32(pkt + 5, s_id);
	put32(pkt + 9, 0);
	for (int i = 0; i < PORTS; i++) {
		send_to(pkt, sizeof(pkt), htonl(INADDR_BROADCAST), (uint16_t)(s_port + i));
		send_to(pkt, sizeof(pkt), htonl(INADDR_LOOPBACK), (uint16_t)(s_port + i));
	}
}

static void link_send(const uint8_t *data, int count)
{
	uint8_t pkt[13 + 256];
	if (!s_have_peer || count > 256)
		return;
	put32(pkt, MAGIC);
	pkt[4] = FRAME;
	put32(pkt + 5, s_id);
	put32(pkt + 9, ++s_seq);
	memcpy(pkt + 13, data, (size_t)count);
	sendto(s_sock, pkt, (size_t)(13 + count), 0, (struct sockaddr *)&s_peer, sizeof(s_peer));
	s_expect = 1;
}

static void link_poll(void)
{
	uint8_t pkt[13 + 256];
	const int64_t t = now_ms();
	if (t - s_hello_ms >= 500) {
		s_hello_ms = t;
		hello();
		if (s_have_peer && t - s_peer_seen_ms > 5000)
			s_have_peer = 0; // (the other cabinet quit)
	}
	int got = 0;
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(s_sock, pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
		if (n < 13 && s_expect && !got && s_have_peer) {
			// ponytail: the emulation stalls up to s_wait_ms per frame sent; lockstep if links over the internet matter
			struct pollfd p = { s_sock, POLLIN, 0 };
			s_expect = 0;
			if (poll(&p, 1, s_wait_ms) > 0)
				n = recvfrom(s_sock, pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
		}
		if (n < 13)
			break;
		const uint32_t id = get32(pkt + 5), seq = get32(pkt + 9);
		if (get32(pkt) != MAGIC || id == s_id)
			continue;
		if (!s_have_peer || s_peer.sin_addr.s_addr != from.sin_addr.s_addr || s_peer.sin_port != from.sin_port) {
			if (s_have_peer && t - s_peer_seen_ms < 5000)
				continue; // ponytail: two cabinets (Kart Duel's ring of 2); more needs the ring order
			s_peer = from;
			s_have_peer = 1;
			s_peer_seq = 0;
			fprintf(stderr, "ReSys12: link peer %s:%u\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
		}
		s_peer_seen_ms = t;
		if (pkt[4] == FRAME && seq != s_peer_seq) {
			got = 1;
			s_expect = 0;
			s_peer_seq = seq;
#ifdef RS12_HARNESS
			if (getenv("RS12_UDPLOG"))
				fprintf(stderr, "udp rx from %u id %08x seq %u type %02x n %d\n", ntohs(from.sin_port), id, seq, pkt[13], (int)(n - 13));
#endif
			s12_link_receive(pkt + 13, (int)(n - 13));
		}
	}
}

// binds the link port or one of the 7 above it; 0 when none is free
int rs12_link_start(uint16_t port)
{
	s_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (s_sock < 0)
		return 0;
	const int on = 1;
	setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
	for (int i = 0; i < PORTS; i++) {
		struct sockaddr_in a;
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		a.sin_port = htons((uint16_t)(port + i));
		if (bind(s_sock, (struct sockaddr *)&a, sizeof(a)) == 0) {
			struct timespec t;
			clock_gettime(CLOCK_REALTIME, &t);
			s_port = port;
			s_id = (uint32_t)t.tv_nsec ^ (uint32_t)getpid() << 16 ^ (uint32_t)i;
			if (getenv("RS12_LINK_WAIT_MS"))
				s_wait_ms = atoi(getenv("RS12_LINK_WAIT_MS"));
			s12_link_send_hook = link_send;
			s12_link_poll_hook = link_poll;
			fprintf(stderr, "ReSys12: link on udp port %u\n", port + i);
			return 1;
		}
	}
	close(s_sock);
	s_sock = -1;
	return 0;
}

void rs12_link_stop(void)
{
	if (s_sock >= 0)
		close(s_sock);
	s_sock = -1;
	s_have_peer = 0;
	s12_link_send_hook = NULL;
	s12_link_poll_hook = NULL;
}
