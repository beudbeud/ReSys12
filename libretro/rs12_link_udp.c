// ReSys12: cabinet link transport for the kartduel network board (s12_link.c), after the System 23 core's S23Link:
// hellos broadcast on the link port and the 7 ports above it (cabinets on one machine bind the next free one), frames
// unicast to the cabinet heard, a sequence number drops a copy. With a lobby (sys12_comm.cfg), the Recalbox link-lobby
// pairs the cabinets over TCP and names the other one, as in the System 23 core. GPL-2.0-or-later.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
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

// lobby mode: the lobby, not the broadcast, names the other cabinet
#define LOBBY_PROTO 3
#define LOBBY_DATA_VERSION 1 // bump when the frames on the wire change
static char s_lobby_host[128], s_room[64], s_nick[64], s_guid[24], s_game[16];
static uint16_t s_lobby_port, s_own_port;
static int s_lobby = -1;
static char s_lobby_in[1024];
static int s_lobby_n;
static int64_t s_lobby_ping_ms, s_lobby_pump_ms;
int rs12_link_ring_id; // cabinet number the lobby gave (1 = master), 0 before; the frontend turns it into the PCB ID
static int s_lobby_gave_up; // no lobby, room full, other game: play alone
static int64_t s_hold_ms;

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
	if (s_lobby_host[0])
		return;
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


static void lobby_send(const char *line)
{
	if (s_lobby < 0)
		return;
	char out[512];
	const int n = snprintf(out, sizeof(out), "%s\n", line);
	send(s_lobby, out, (size_t)n, MSG_NOSIGNAL);
}

static void lobby_line(const char *line)
{
	int id = 0, count = 0, port = 0;
	char host[64] = "", out[96];
	if (sscanf(line, "RING %d %d %63[^:]:%d", &id, &count, host, &port) == 4) {
		struct in_addr a;
		if (inet_pton(AF_INET, host, &a) != 1 || port <= 0 || port > 65535)
			return;
		memset(&s_peer, 0, sizeof(s_peer));
		s_peer.sin_family = AF_INET;
		s_peer.sin_addr = a;
		s_peer.sin_port = htons((uint16_t)port);
		s_have_peer = 1;
		s_peer_seq = 0;
		rs12_link_ring_id = id;
		fprintf(stderr, "ReSys12: lobby: cabinet %d/%d, the other one is %s:%d\n", id, count, host, port);
	} else if (!strncmp(line, "RESET", 5) || !strncmp(line, "WAIT", 4)) {
		s_have_peer = 0; // unpaired until the next RING: the game sees the other cabinet go quiet
		if (line[1] == 'E')
			lobby_send("READY");
	} else if (!strncmp(line, "ERROR", 5)) {
		fprintf(stderr, "ReSys12: lobby: %s, playing alone\n", line); // room full, other game, other version
		s_lobby_gave_up = 1;
	} else if (!strncmp(line, "ASK guid=", 9) && !s_have_peer) {
		// unpaired, the master has nothing to interrupt: the knocking cabinet is the one it waits for
		const int n = (int)strcspn(line + 9, " ");
		snprintf(out, sizeof(out), "ADMIT %.*s", n < 32 ? n : 32, line + 9);
		lobby_send(out);
	}
}

static void lobby_pump(int64_t t)
{
	if (s_lobby < 0 || t - s_lobby_pump_ms < 100)
		return;
	s_lobby_pump_ms = t;
	if (t - s_lobby_ping_ms >= 10000) { // the lobby drops a cabinet silent for 30 s
		s_lobby_ping_ms = t;
		lobby_send("PING");
	}
	for (;;) {
		const ssize_t n = recv(s_lobby, s_lobby_in + s_lobby_n, sizeof(s_lobby_in) - 1 - (size_t)s_lobby_n, MSG_DONTWAIT);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (n <= 0) { // the frames go peer to peer: a paired cabinet keeps its partner
			fprintf(stderr, "ReSys12: lobby connection lost\n");
			close(s_lobby);
			s_lobby = -1;
			s_lobby_gave_up = 1;
			return;
		}
		s_lobby_n += (int)n;
		if (s_lobby_n >= (int)sizeof(s_lobby_in) - 1)
			s_lobby_n = 0; // no line is that long: not a lobby
	}
	s_lobby_in[s_lobby_n] = 0;
	char *line = s_lobby_in, *eol;
	while (s_lobby >= 0 && (eol = strchr(line, '\n'))) {
		*eol = 0;
		if (eol > line && eol[-1] == '\r')
			eol[-1] = 0;
		lobby_line(line);
		line = eol + 1;
	}
	s_lobby_n -= (int)(line - s_lobby_in);
	memmove(s_lobby_in, line, (size_t)s_lobby_n);
}

// once, at content load: a bounded connect, then the socket is pumped from link_poll
static void lobby_connect(void)
{
	struct addrinfo hints, *found = NULL;
	char service[8], line[512];
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(service, sizeof(service), "%u", s_lobby_port);
	if (getaddrinfo(s_lobby_host, service, &hints, &found) != 0 || !found) {
		fprintf(stderr, "ReSys12: lobby %s: cannot resolve, playing alone\n", s_lobby_host);
		s_lobby_gave_up = 1;
		return;
	}
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	int ok = 0;
	if (fd >= 0) {
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
		ok = connect(fd, found->ai_addr, found->ai_addrlen) == 0;
		if (!ok && errno == EINPROGRESS) {
			struct pollfd p = { fd, POLLOUT, 0 };
			int err = 0;
			socklen_t l = sizeof(err);
			ok = poll(&p, 1, 3000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) == 0 && !err;
		}
	}
	freeaddrinfo(found);
	if (!ok) { // ponytail: no reconnect, a lobby down at load means this cabinet plays alone
		fprintf(stderr, "ReSys12: lobby %s:%u unreachable, playing alone\n", s_lobby_host, s_lobby_port);
		if (fd >= 0)
			close(fd);
		s_lobby_gave_up = 1;
		return;
	}
	const int on = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	s_lobby = fd;
	s_lobby_n = 0;
	s_lobby_ping_ms = now_ms();
	// the address this machine reaches the lobby from is its LAN one: the lobby hands it to a cabinet behind the same NAT
	struct sockaddr_in local;
	socklen_t ll = sizeof(local);
	memset(&local, 0, sizeof(local));
	getsockname(fd, (struct sockaddr *)&local, &ll);
	snprintf(line, sizeof(line), "HELLO proto=%d guid=%s room=%s system=namcos12 game=%s ver=%d port=%u nick=%s lan=%s",
		LOBBY_PROTO, s_guid, s_room, s_game, LOBBY_DATA_VERSION, s_own_port, s_nick, inet_ntoa(local.sin_addr));
	lobby_send(line);
	fprintf(stderr, "ReSys12: lobby %s:%u joined, room '%s'\n", s_lobby_host, s_lobby_port, s_room);
}

// with a lobby the cabinet holds its boot until the lobby pairs it (the game reads its PCB ID and sets the link up at
// boot), up to RS12_LINK_HOLD seconds (60), as the System 23 core does; 1 while holding
int rs12_link_holding(void)
{
	if (!s_lobby_host[0] || s_lobby_gave_up || s_hold_ms < 0)
		return 0;
	const int64_t t = now_ms();
	if (!s_hold_ms)
		s_hold_ms = t;
	s_lobby_pump_ms = 0;
	lobby_pump(t);
	const int64_t limit = getenv("RS12_LINK_HOLD") ? atoi(getenv("RS12_LINK_HOLD")) * 1000LL : 60000;
	if (!rs12_link_ring_id && !s_lobby_gave_up && t - s_hold_ms < limit)
		return 1;
	fprintf(stderr, "ReSys12: link: %s, booting\n", rs12_link_ring_id ? "paired" : "no other cabinet");
	s_hold_ms = -1;
	return 0;
}

static void trim(char *s)
{
	char *b = s + strspn(s, " \t\r\n");
	size_t n = strlen(b);
	while (n && strchr(" \t\r\n", b[n - 1]))
		n--;
	memmove(s, b, n);
	s[n] = 0;
}

// the lobby, as the System 23 core reads it: <save dir>/sys12_comm.cfg ("lobby = host[:port]", "room = name",
// "nick = name", '#' comments), RS12_LOBBY / RS12_ROOM / RS12_NICK on top; the identity is kept in sys12_guid.txt
void rs12_link_lobby_config(const char *save_dir, const char *game, const char *user)
{
	char path[1024], line[256], lobby[128] = "", room[64] = "", nick[64] = "";
	snprintf(path, sizeof(path), "%s/sys12_comm.cfg", save_dir);
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "#")] = 0;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		trim(line);
		trim(eq + 1);
		if (!strcmp(line, "lobby")) snprintf(lobby, sizeof(lobby), "%s", eq + 1);
		else if (!strcmp(line, "room")) snprintf(room, sizeof(room), "%s", eq + 1);
		else if (!strcmp(line, "nick")) snprintf(nick, sizeof(nick), "%s", eq + 1);
	}
	if (f)
		fclose(f);
	if (getenv("RS12_LOBBY")) snprintf(lobby, sizeof(lobby), "%s", getenv("RS12_LOBBY"));
	if (getenv("RS12_ROOM")) snprintf(room, sizeof(room), "%s", getenv("RS12_ROOM"));
	if (getenv("RS12_NICK")) snprintf(nick, sizeof(nick), "%s", getenv("RS12_NICK"));
	if (!lobby[0])
		return;
	char *colon = strrchr(lobby, ':');
	s_lobby_port = 16000;
	if (colon) {
		s_lobby_port = (uint16_t)strtoul(colon + 1, NULL, 10);
		*colon = 0;
	}
	snprintf(s_lobby_host, sizeof(s_lobby_host), "%s", lobby);
	snprintf(s_room, sizeof(s_room), "%s", room[0] ? room : "default");
	snprintf(s_nick, sizeof(s_nick), "%s", nick[0] ? nick : user && user[0] ? user : "player");
	for (char *c = s_nick; *c; c++) // the protocol is blank separated
		if (*c <= ' ')
			*c = '_';
	for (char *c = s_room; *c; c++)
		if (*c <= ' ')
			*c = '_';
	snprintf(s_game, sizeof(s_game), "%s", game);
	snprintf(path, sizeof(path), "%s/sys12_guid.txt", save_dir);
	s_guid[0] = 0;
	if ((f = fopen(path, "r"))) {
		if (fscanf(f, "%16s", s_guid) != 1)
			s_guid[0] = 0;
		fclose(f);
	}
	if (!s_guid[0]) { // kept, so a master's refusal still holds after a restart
		struct timespec t;
		clock_gettime(CLOCK_REALTIME, &t);
		snprintf(s_guid, sizeof(s_guid), "%08x%08x", (unsigned)t.tv_nsec ^ (unsigned)getpid() << 16, (unsigned)t.tv_sec);
		if ((f = fopen(path, "w"))) {
			fprintf(f, "%s\n", s_guid);
			fclose(f);
		}
	}
}

static void link_poll(void)
{
	uint8_t pkt[13 + 256];
	const int64_t t = now_ms();
	lobby_pump(t);
	if (t - s_hello_ms >= 500 && !s_lobby_host[0]) {
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
		if (s_lobby_host[0] && (!s_have_peer || from.sin_addr.s_addr != s_peer.sin_addr.s_addr))
			continue; // through the lobby, only the cabinet it named (its address: a NAT may remap the port)
		if (!s_lobby_host[0] && (!s_have_peer || s_peer.sin_addr.s_addr != from.sin_addr.s_addr || s_peer.sin_port != from.sin_port)) {
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
			s_own_port = (uint16_t)(port + i);
			s_id = (uint32_t)t.tv_nsec ^ (uint32_t)getpid() << 16 ^ (uint32_t)i;
			if (getenv("RS12_LINK_WAIT_MS"))
				s_wait_ms = atoi(getenv("RS12_LINK_WAIT_MS"));
			s12_link_send_hook = link_send;
			s12_link_poll_hook = link_poll;
			fprintf(stderr, "ReSys12: link on udp port %u\n", port + i);
			if (s_lobby_host[0])
				lobby_connect();
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
	if (s_lobby >= 0) {
		lobby_send("BYE");
		close(s_lobby);
	}
	s_lobby = -1;
	s_lobby_host[0] = 0;
	rs12_link_ring_id = 0;
	s_lobby_gave_up = 0;
	s_hold_ms = 0;
	s12_link_send_hook = NULL;
	s12_link_poll_hook = NULL;
}
