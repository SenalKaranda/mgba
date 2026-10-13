/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/feature/netplay-net.h>

#include <mgba-util/socket.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* Messages travel over TCP, each prefixed with its u32 length; keys travel
 * over UDP on the same port number. The host has one UDP socket for every
 * client, so on accepting a connection it hands the client a token over
 * TCP, and the client prefixes every datagram with it. The host replies to
 * whatever address a client's datagrams come from, which also works from
 * behind most NATs once the client has sent something. IPv4 only. */

#define TRANSPORT_MAGIC 0x544E4D4D /* "MMNT" */
#define MAX_MESSAGE (64 * 1024 * 1024)
#define MAX_DATAGRAM 2048
#define MAX_LINKS 16
#define MAX_QUEUED_DATAGRAMS 256
#define CLOSE_FLUSH_MS 500

#ifdef _WIN32
typedef int socklen_t_compat;
#else
typedef socklen_t socklen_t_compat;
#endif

struct Datagram {
	struct Datagram* next;
	size_t size;
	uint8_t data[];
};

struct SocketListener;

struct SocketLink {
	struct mNetplayLink d;
	Socket tcp;
	bool closed;
	bool eof;
	uint8_t* in;
	size_t inSize;
	size_t inCapacity;
	uint8_t* out;
	size_t outSize;
	size_t outCapacity;
	uint32_t token;

	/* Client side: a UDP socket connected to the host. */
	Socket udp;

	/* Host side: the listener's shared UDP socket and this client's
	 * address as last seen on it. */
	struct SocketListener* listener;
	struct sockaddr_in peer;
	bool havePeer;
	struct Datagram* queueHead;
	struct Datagram* queueTail;
	size_t queued;
};

struct SocketListener {
	struct mNetplayListener d;
	Socket tcp;
	Socket udp;
	unsigned port;
	uint32_t nextToken;
	struct SocketLink* links[MAX_LINKS];
};

/* --- Low level ------------------------------------------------------------ */

static ssize_t _send(Socket socket, const void* data, size_t size) {
#ifdef _WIN32
	return send(socket, (const char*) data, size, 0);
#elif defined(MSG_NOSIGNAL)
	/* A peer that hung up must not kill us with SIGPIPE. */
	return send(socket, data, size, MSG_NOSIGNAL);
#else
	return send(socket, data, size, 0);
#endif
}

static void _noSigpipe(Socket socket) {
#if defined(SO_NOSIGPIPE)
	int one = 1;
	setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
	UNUSED(socket);
#endif
}

static uint64_t _monotonicMs(void) {
#ifdef _WIN32
	return GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static uint64_t _clockNow(struct mNetplayClock* clock) {
	UNUSED(clock);
	return _monotonicMs();
}

static struct mNetplayClock _clock = { .now = _clockNow };

struct mNetplayClock* mNetplaySocketClock(void) {
	return &_clock;
}

static void _writeU32(uint8_t* out, uint32_t value) {
	out[0] = value;
	out[1] = value >> 8;
	out[2] = value >> 16;
	out[3] = value >> 24;
}

static uint32_t _readU32(const uint8_t* in) {
	return in[0] | (in[1] << 8) | (in[2] << 16) | ((uint32_t) in[3] << 24);
}

static bool _grow(uint8_t** buffer, size_t* capacity, size_t needed) {
	if (needed <= *capacity) {
		return true;
	}
	size_t capacity2 = *capacity ? *capacity : 4096;
	while (capacity2 < needed) {
		capacity2 *= 2;
	}
	uint8_t* grown = realloc(*buffer, capacity2);
	if (!grown) {
		return false;
	}
	*buffer = grown;
	*capacity = capacity2;
	return true;
}

/* --- TCP framing ---------------------------------------------------------- */

static void _flush(struct SocketLink* link) {
	while (!link->closed && link->outSize) {
		ssize_t sent = _send(link->tcp, link->out, link->outSize);
		if (sent < 0) {
			if (!SocketWouldBlock()) {
				link->closed = true;
			}
			return;
		}
		memmove(link->out, &link->out[sent], link->outSize - sent);
		link->outSize -= sent;
	}
}

static void _fill(struct SocketLink* link) {
	while (!link->closed && !link->eof) {
		if (!_grow(&link->in, &link->inCapacity, link->inSize + 65536)) {
			link->closed = true;
			return;
		}
		ssize_t got = SocketRecv(link->tcp, &link->in[link->inSize], link->inCapacity - link->inSize);
		if (got == 0) {
			link->eof = true;
			return;
		}
		if (got < 0) {
			if (!SocketWouldBlock()) {
				link->eof = true;
			}
			return;
		}
		link->inSize += got;
	}
}

/* Takes one whole framed message out of the input buffer, if there is one. */
static ssize_t _takeMessage(struct SocketLink* link, void** data) {
	if (link->inSize < 4) {
		return 0;
	}
	uint32_t size = _readU32(link->in);
	if (size > MAX_MESSAGE) {
		link->closed = true;
		return -1;
	}
	if (link->inSize < 4 + (size_t) size) {
		return 0;
	}
	void* message = malloc(size ? size : 1);
	if (!message) {
		link->closed = true;
		return -1;
	}
	memcpy(message, &link->in[4], size);
	memmove(link->in, &link->in[4 + size], link->inSize - 4 - size);
	link->inSize -= 4 + size;
	*data = message;
	return size;
}

static bool _queueMessage(struct SocketLink* link, const void* data, size_t size) {
	if (size > MAX_MESSAGE || !_grow(&link->out, &link->outCapacity, link->outSize + 4 + size)) {
		return false;
	}
	_writeU32(&link->out[link->outSize], size);
	memcpy(&link->out[link->outSize + 4], data, size);
	link->outSize += 4 + size;
	return true;
}

static bool _sendReliable(struct mNetplayLink* base, const void* data, size_t size) {
	struct SocketLink* link = (struct SocketLink*) base;
	if (link->closed || !_queueMessage(link, data, size)) {
		return false;
	}
	_flush(link);
	return !link->closed;
}

static ssize_t _recvReliable(struct mNetplayLink* base, void** data) {
	struct SocketLink* link = (struct SocketLink*) base;
	*data = NULL;
	_flush(link);
	_fill(link);
	ssize_t size = _takeMessage(link, data);
	if (size) {
		return size;
	}
	return link->closed || link->eof ? -1 : 0;
}

static bool _isOpen(struct mNetplayLink* base) {
	struct SocketLink* link = (struct SocketLink*) base;
	_flush(link);
	return !link->closed && !link->eof;
}

/* --- UDP ------------------------------------------------------------------- */

static Socket _openUdp(unsigned port) {
	Socket udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (SOCKET_FAILED(udp)) {
		return udp;
	}
	if (port) {
		struct sockaddr_in bindInfo;
		memset(&bindInfo, 0, sizeof(bindInfo));
		bindInfo.sin_family = AF_INET;
		bindInfo.sin_port = htons(port);
		bindInfo.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(udp, (struct sockaddr*) &bindInfo, sizeof(bindInfo)) < 0) {
			SocketCloseQuiet(udp);
			return INVALID_SOCKET;
		}
	}
#ifdef _WIN32
	/* Otherwise an ICMP "port unreachable" from one peer makes the next
	 * recvfrom() on this socket fail. */
	BOOL reset = FALSE;
	DWORD bytes = 0;
	WSAIoctl(udp, _WSAIOW(IOC_VENDOR, 12), &reset, sizeof(reset), NULL, 0, &bytes, NULL, NULL);
#endif
	SocketSetBlocking(udp, false);
	return udp;
}

static bool _sendDatagramTo(Socket udp, const struct sockaddr_in* to, uint32_t token, const void* data, size_t size) {
	uint8_t buffer[MAX_DATAGRAM];
	if (size + 4 > sizeof(buffer)) {
		return false;
	}
	_writeU32(buffer, token);
	memcpy(&buffer[4], data, size);
	ssize_t sent;
	if (to) {
		sent = sendto(udp, (const char*) buffer, size + 4, 0, (const struct sockaddr*) to, sizeof(*to));
	} else {
		sent = send(udp, (const char*) buffer, size + 4, 0);
	}
	/* Datagrams may be dropped anyway; only a broken socket is an error. */
	return sent >= 0 || SocketWouldBlock();
}

static void _enqueueDatagram(struct SocketLink* link, const uint8_t* data, size_t size) {
	if (link->queued >= MAX_QUEUED_DATAGRAMS) {
		return;
	}
	struct Datagram* datagram = malloc(sizeof(*datagram) + size);
	if (!datagram) {
		return;
	}
	datagram->next = NULL;
	datagram->size = size;
	memcpy(datagram->data, data, size);
	if (link->queueTail) {
		link->queueTail->next = datagram;
	} else {
		link->queueHead = datagram;
	}
	link->queueTail = datagram;
	++link->queued;
}

/* Host: hands every waiting datagram to the link whose token it carries. */
static void _pumpListener(struct SocketListener* listener) {
	uint8_t buffer[MAX_DATAGRAM];
	unsigned errors = 0;
	while (true) {
		struct sockaddr_in from;
		socklen_t_compat fromLength = sizeof(from);
		ssize_t got = recvfrom(listener->udp, (char*) buffer, sizeof(buffer), 0, (struct sockaddr*) &from, &fromLength);
		if (got < 0) {
			/* Errors here are ICMP reports about some peer; skip a few, but
			 * never spin on a socket that keeps failing. */
			if (SocketWouldBlock() || ++errors > 16) {
				return;
			}
			continue;
		}
		if (got < 4) {
			continue;
		}
		uint32_t token = _readU32(buffer);
		size_t i;
		for (i = 0; i < MAX_LINKS; ++i) {
			struct SocketLink* link = listener->links[i];
			if (link && link->token == token) {
				link->peer = from;
				link->havePeer = true;
				_enqueueDatagram(link, &buffer[4], got - 4);
				break;
			}
		}
	}
}

static bool _sendUnreliable(struct mNetplayLink* base, const void* data, size_t size) {
	struct SocketLink* link = (struct SocketLink*) base;
	if (link->closed) {
		return false;
	}
	if (link->listener) {
		/* Nothing to send to until the client's first datagram arrives. */
		if (!link->havePeer) {
			return true;
		}
		return _sendDatagramTo(link->listener->udp, &link->peer, link->token, data, size);
	}
	return _sendDatagramTo(link->udp, NULL, link->token, data, size);
}

static ssize_t _recvUnreliable(struct mNetplayLink* base, void* buffer, size_t size) {
	struct SocketLink* link = (struct SocketLink*) base;
	if (link->listener) {
		_pumpListener(link->listener);
		struct Datagram* datagram = link->queueHead;
		if (!datagram) {
			return 0;
		}
		link->queueHead = datagram->next;
		if (!link->queueHead) {
			link->queueTail = NULL;
		}
		--link->queued;
		size_t copied = datagram->size < size ? datagram->size : size;
		memcpy(buffer, datagram->data, copied);
		free(datagram);
		return copied;
	}
	uint8_t incoming[MAX_DATAGRAM];
	unsigned errors = 0;
	while (!SOCKET_FAILED(link->udp)) {
		ssize_t got = recv(link->udp, (char*) incoming, sizeof(incoming), 0);
		if (got < 0) {
			if (SocketWouldBlock() || ++errors > 16) {
				return 0;
			}
			continue;
		}
		if (got < 4 || _readU32(incoming) != link->token) {
			continue;
		}
		size_t copied = (size_t) got - 4 < size ? (size_t) got - 4 : size;
		memcpy(buffer, &incoming[4], copied);
		return copied;
	}
	return 0;
}

/* --- Links ------------------------------------------------------------------ */

static void _destroy(struct mNetplayLink* base) {
	struct SocketLink* link = (struct SocketLink*) base;
	/* Give a last message (a goodbye, a rejection) a moment to get out. */
	uint64_t deadline = _monotonicMs() + CLOSE_FLUSH_MS;
	while (!link->closed && link->outSize && _monotonicMs() < deadline) {
		_flush(link);
		if (link->outSize) {
			Socket writes[] = { link->tcp, INVALID_SOCKET };
			SocketPoll(1, NULL, writes, NULL, 10);
		}
	}
	/* Then close gracefully. Closing a socket that still holds unread data
	 * sends a reset, and a reset can make the other side throw away what it
	 * has not read yet, the goodbye included (Windows does). So say we are
	 * done sending, and read and discard until the other side closes too. */
	if (!link->closed && !link->eof && !SOCKET_FAILED(link->tcp)) {
#ifdef _WIN32
		shutdown(link->tcp, SD_SEND);
#else
		shutdown(link->tcp, SHUT_WR);
#endif
		uint8_t discard[4096];
		deadline = _monotonicMs() + CLOSE_FLUSH_MS;
		while (_monotonicMs() < deadline) {
			ssize_t got = SocketRecv(link->tcp, discard, sizeof(discard));
			if (got == 0 || (got < 0 && !SocketWouldBlock())) {
				break;
			}
			if (got < 0) {
				Socket reads[] = { link->tcp, INVALID_SOCKET };
				SocketPoll(1, reads, NULL, NULL, 10);
			}
		}
	}
	if (link->listener) {
		size_t i;
		for (i = 0; i < MAX_LINKS; ++i) {
			if (link->listener->links[i] == link) {
				link->listener->links[i] = NULL;
			}
		}
	}
	while (link->queueHead) {
		struct Datagram* next = link->queueHead->next;
		free(link->queueHead);
		link->queueHead = next;
	}
	if (!SOCKET_FAILED(link->tcp)) {
		SocketClose(link->tcp);
	}
	if (!SOCKET_FAILED(link->udp)) {
		SocketClose(link->udp);
	}
	free(link->in);
	free(link->out);
	free(link);
}

static struct SocketLink* _createLink(Socket tcp) {
	struct SocketLink* link = calloc(1, sizeof(*link));
	if (!link) {
		return NULL;
	}
	link->d.sendReliable = _sendReliable;
	link->d.recvReliable = _recvReliable;
	link->d.sendUnreliable = _sendUnreliable;
	link->d.recvUnreliable = _recvUnreliable;
	link->d.isOpen = _isOpen;
	link->d.destroy = _destroy;
	link->tcp = tcp;
	link->udp = INVALID_SOCKET;
	SocketSetBlocking(tcp, false);
	SocketSetTCPPush(tcp, 1);
	_noSigpipe(tcp);
	return link;
}

/* --- Listener --------------------------------------------------------------- */

static struct mNetplayLink* _accept(struct mNetplayListener* base) {
	struct SocketListener* listener = (struct SocketListener*) base;
	Socket tcp = SocketAccept(listener->tcp, NULL);
	if (SOCKET_FAILED(tcp)) {
		return NULL;
	}
	size_t slot;
	for (slot = 0; slot < MAX_LINKS && listener->links[slot]; ++slot);
	struct SocketLink* link = slot < MAX_LINKS ? _createLink(tcp) : NULL;
	if (!link) {
		SocketClose(tcp);
		return NULL;
	}
	link->listener = listener;
	/* Unpredictable enough to keep strangers' datagrams out of the game. */
	link->token = (++listener->nextToken * 0x9E3779B9u) ^ (uint32_t) _monotonicMs() ^ (uint32_t) (uintptr_t) link;
	listener->links[slot] = link;

	uint8_t hello[10];
	_writeU32(hello, TRANSPORT_MAGIC);
	_writeU32(&hello[4], link->token);
	hello[8] = listener->port;
	hello[9] = listener->port >> 8;
	_sendReliable(&link->d, hello, sizeof(hello));
	return &link->d;
}

static void _listenerDestroy(struct mNetplayListener* base) {
	struct SocketListener* listener = (struct SocketListener*) base;
	size_t i;
	for (i = 0; i < MAX_LINKS; ++i) {
		if (listener->links[i]) {
			listener->links[i]->listener = NULL;
			listener->links[i]->closed = true;
		}
	}
	SocketClose(listener->tcp);
	SocketClose(listener->udp);
	free(listener);
}

struct mNetplayListener* mNetplaySocketListen(unsigned port) {
	SocketSubsystemInit();
	struct SocketListener* listener = calloc(1, sizeof(*listener));
	if (!listener) {
		return NULL;
	}
	listener->d.accept = _accept;
	listener->d.destroy = _listenerDestroy;
	listener->port = port;
	listener->tcp = SocketOpenTCP(port, NULL);
	if (SOCKET_FAILED(listener->tcp) || SOCKET_FAILED(SocketListen(listener->tcp, 4))) {
		if (!SOCKET_FAILED(listener->tcp)) {
			SocketClose(listener->tcp);
		}
		free(listener);
		return NULL;
	}
	SocketSetBlocking(listener->tcp, false);
	listener->udp = _openUdp(port);
	if (SOCKET_FAILED(listener->udp)) {
		SocketClose(listener->tcp);
		free(listener);
		return NULL;
	}
	return &listener->d;
}

/* --- Connecting ----------------------------------------------------------- */

/* Only IPv4: a name with both kinds of address (Tailscale's MagicDNS names,
 * for one) must not come back as IPv6 first. */
static bool _resolve(const char* host, unsigned port, struct sockaddr_in* address) {
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo* result = NULL;
	if (getaddrinfo(host, NULL, &hints, &result) || !result) {
		return false;
	}
	bool found = result->ai_family == AF_INET && result->ai_addrlen >= sizeof(*address);
	if (found) {
		memcpy(address, result->ai_addr, sizeof(*address));
		address->sin_port = htons(port);
	}
	freeaddrinfo(result);
	return found;
}

/* Connects without blocking for longer than timeoutMs, unlike connect(). */
static Socket _connectTcp(const struct sockaddr_in* address, unsigned timeoutMs) {
	Socket tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (SOCKET_FAILED(tcp)) {
		return tcp;
	}
	SocketSetBlocking(tcp, false);
	if (connect(tcp, (const struct sockaddr*) address, sizeof(*address)) < 0) {
#ifdef _WIN32
		bool pending = SocketWouldBlock();
#else
		bool pending = SocketError() == EINPROGRESS;
#endif
		/* A failed attempt shows up as an error on Windows and as writable
		 * with SO_ERROR set elsewhere. */
		Socket writes[] = { tcp };
		Socket errors[] = { tcp };
		int error = 0;
		socklen_t_compat length = sizeof(error);
		if (!pending || SocketPoll(1, NULL, writes, errors, timeoutMs) <= 0 || SOCKET_FAILED(writes[0]) ||
		    getsockopt(tcp, SOL_SOCKET, SO_ERROR, (char*) &error, &length) < 0 || error) {
			SocketCloseQuiet(tcp);
			return INVALID_SOCKET;
		}
	}
	return tcp;
}

struct mNetplayLink* mNetplaySocketConnect(const char* host, unsigned port, unsigned timeoutMs) {
	SocketSubsystemInit();
	struct sockaddr_in to;
	if (!_resolve(host, port, &to)) {
		return NULL;
	}
	uint64_t deadline = _monotonicMs() + timeoutMs;
	Socket tcp = _connectTcp(&to, timeoutMs);
	if (SOCKET_FAILED(tcp)) {
		return NULL;
	}
	struct SocketLink* link = _createLink(tcp);
	if (!link) {
		SocketClose(tcp);
		return NULL;
	}

	/* The host's first message names our token and its UDP port. */
	void* hello = NULL;
	ssize_t size = 0;
	while (!size && _monotonicMs() < deadline) {
		size = _recvReliable(&link->d, &hello);
		if (!size) {
			Socket reads[] = { tcp, INVALID_SOCKET };
			SocketPoll(1, reads, NULL, NULL, 20);
		}
	}
	if (size != 10 || _readU32(hello) != TRANSPORT_MAGIC) {
		free(hello);
		_destroy(&link->d);
		return NULL;
	}
	link->token = _readU32((uint8_t*) hello + 4);
	unsigned udpPort = ((uint8_t*) hello)[8] | (((uint8_t*) hello)[9] << 8);
	free(hello);

	link->udp = _openUdp(0);
	to.sin_port = htons(udpPort);
	if (SOCKET_FAILED(link->udp) || connect(link->udp, (struct sockaddr*) &to, sizeof(to)) < 0) {
		_destroy(&link->d);
		return NULL;
	}
	return &link->d;
}
