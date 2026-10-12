/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/feature/netplay-net.h>

/* An in-memory network on a virtual clock. Reliable messages arrive in
 * order after the link's latency; datagrams arrive after the latency plus
 * jitter (so they can overtake each other) unless they are lost. Every
 * random choice comes from one seeded generator, so a test run with the
 * same seed and the same calls behaves identically every time. */

#define MAX_PENDING_ACCEPTS 8

struct SimPacket {
	struct SimPacket* next;
	uint64_t deliverAt;
	size_t size;
	uint8_t data[];
};

struct SimLink {
	struct mNetplayLink d;
	struct mNetplaySimNetwork* net;
	struct SimLink* remote;
	struct mNetplaySimLinkParams params;
	bool closed;
	/* When the remote end closed, as seen from here (after the latency). */
	bool remoteClosed;
	uint64_t remoteClosedAt;
	struct SimPacket* reliable;
	struct SimPacket* unreliable;
	/* Delivery time of the last reliable message sent to the remote. */
	uint64_t lastReliableAt;
};

struct SimListener {
	struct mNetplayListener d;
	struct mNetplaySimNetwork* net;
	struct SimLink* pending[MAX_PENDING_ACCEPTS];
	uint64_t pendingAt[MAX_PENDING_ACCEPTS];
	size_t nPending;
};

struct mNetplaySimNetwork {
	struct mNetplayClock clock;
	uint64_t now;
	uint32_t rng;
};

static uint32_t _random(struct mNetplaySimNetwork* net) {
	uint32_t x = net->rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	net->rng = x;
	return x;
}

static uint64_t _now(struct mNetplayClock* clock) {
	return ((struct mNetplaySimNetwork*) clock)->now;
}

struct mNetplaySimNetwork* mNetplaySimNetworkCreate(uint32_t seed) {
	struct mNetplaySimNetwork* net = calloc(1, sizeof(*net));
	if (!net) {
		return NULL;
	}
	net->clock.now = _now;
	net->rng = seed ? seed : 0x9E3779B9;
	/* Start away from zero, like a real clock. */
	net->now = 1000;
	return net;
}

void mNetplaySimNetworkDestroy(struct mNetplaySimNetwork* net) {
	free(net);
}

struct mNetplayClock* mNetplaySimNetworkClock(struct mNetplaySimNetwork* net) {
	return &net->clock;
}

void mNetplaySimNetworkAdvance(struct mNetplaySimNetwork* net, unsigned ms) {
	net->now += ms;
}

static void _freeQueue(struct SimPacket* packet) {
	while (packet) {
		struct SimPacket* next = packet->next;
		free(packet);
		packet = next;
	}
}

/* Inserts after every packet due no later, keeping equal times in order. */
static void _enqueue(struct SimPacket** queue, struct SimPacket* packet) {
	while (*queue && (*queue)->deliverAt <= packet->deliverAt) {
		queue = &(*queue)->next;
	}
	packet->next = *queue;
	*queue = packet;
}

static struct SimPacket* _packet(const void* data, size_t size, uint64_t deliverAt) {
	struct SimPacket* packet = malloc(sizeof(*packet) + size);
	if (!packet) {
		return NULL;
	}
	packet->next = NULL;
	packet->deliverAt = deliverAt;
	packet->size = size;
	if (size) {
		memcpy(packet->data, data, size);
	}
	return packet;
}

static bool _linkUsable(const struct SimLink* link) {
	return !link->closed && link->remote && !link->remote->closed;
}

static bool _simSendReliable(struct mNetplayLink* base, const void* data, size_t size) {
	struct SimLink* link = (struct SimLink*) base;
	if (!_linkUsable(link)) {
		return false;
	}
	uint64_t at = link->net->now + link->params.latencyMs;
	if (at < link->lastReliableAt) {
		at = link->lastReliableAt;
	}
	link->lastReliableAt = at;
	struct SimPacket* packet = _packet(data, size, at);
	if (!packet) {
		return false;
	}
	_enqueue(&link->remote->reliable, packet);
	return true;
}

static ssize_t _simRecvReliable(struct mNetplayLink* base, void** data) {
	struct SimLink* link = (struct SimLink*) base;
	*data = NULL;
	struct SimPacket* packet = link->reliable;
	if (packet && packet->deliverAt <= link->net->now) {
		link->reliable = packet->next;
		void* copy = malloc(packet->size ? packet->size : 1);
		if (!copy) {
			free(packet);
			return -1;
		}
		memcpy(copy, packet->data, packet->size);
		ssize_t size = packet->size;
		free(packet);
		*data = copy;
		return size;
	}
	if (link->closed || (link->remoteClosed && link->remoteClosedAt <= link->net->now && !link->reliable)) {
		return -1;
	}
	return 0;
}

static bool _simSendUnreliable(struct mNetplayLink* base, const void* data, size_t size) {
	struct SimLink* link = (struct SimLink*) base;
	if (!_linkUsable(link)) {
		return false;
	}
	struct mNetplaySimNetwork* net = link->net;
	if (link->params.lossPermille && _random(net) % 1000 < link->params.lossPermille) {
		return true;
	}
	uint64_t at = net->now + link->params.latencyMs;
	if (link->params.jitterMs) {
		at += _random(net) % (link->params.jitterMs + 1);
	}
	struct SimPacket* packet = _packet(data, size, at);
	if (packet) {
		_enqueue(&link->remote->unreliable, packet);
	}
	return true;
}

static ssize_t _simRecvUnreliable(struct mNetplayLink* base, void* buffer, size_t size) {
	struct SimLink* link = (struct SimLink*) base;
	struct SimPacket* packet = link->unreliable;
	if (!packet || packet->deliverAt > link->net->now) {
		return 0;
	}
	link->unreliable = packet->next;
	size_t copied = packet->size < size ? packet->size : size;
	memcpy(buffer, packet->data, copied);
	free(packet);
	return copied;
}

static bool _simIsOpen(struct mNetplayLink* base) {
	struct SimLink* link = (struct SimLink*) base;
	if (link->closed) {
		return false;
	}
	return !(link->remoteClosed && link->remoteClosedAt <= link->net->now);
}

static void _closeRemoteView(struct SimLink* link, uint64_t at) {
	struct SimLink* remote = link->remote;
	if (!remote) {
		return;
	}
	remote->remoteClosed = true;
	remote->remoteClosedAt = at;
	remote->remote = NULL;
	link->remote = NULL;
}

static void _simDestroy(struct mNetplayLink* base) {
	struct SimLink* link = (struct SimLink*) base;
	_closeRemoteView(link, link->net->now + link->params.latencyMs);
	_freeQueue(link->reliable);
	_freeQueue(link->unreliable);
	free(link);
}

static struct SimLink* _createLink(struct mNetplaySimNetwork* net, const struct mNetplaySimLinkParams* params) {
	struct SimLink* link = calloc(1, sizeof(*link));
	if (!link) {
		return NULL;
	}
	link->d.sendReliable = _simSendReliable;
	link->d.recvReliable = _simRecvReliable;
	link->d.sendUnreliable = _simSendUnreliable;
	link->d.recvUnreliable = _simRecvUnreliable;
	link->d.isOpen = _simIsOpen;
	link->d.destroy = _simDestroy;
	link->net = net;
	if (params) {
		link->params = *params;
	}
	return link;
}

static struct mNetplayLink* _simAccept(struct mNetplayListener* base) {
	struct SimListener* listener = (struct SimListener*) base;
	if (!listener->nPending || listener->pendingAt[0] > listener->net->now) {
		return NULL;
	}
	struct SimLink* link = listener->pending[0];
	--listener->nPending;
	memmove(&listener->pending[0], &listener->pending[1], listener->nPending * sizeof(*listener->pending));
	memmove(&listener->pendingAt[0], &listener->pendingAt[1], listener->nPending * sizeof(*listener->pendingAt));
	return &link->d;
}

static void _simListenerDestroy(struct mNetplayListener* base) {
	struct SimListener* listener = (struct SimListener*) base;
	size_t i;
	for (i = 0; i < listener->nPending; ++i) {
		_simDestroy(&listener->pending[i]->d);
	}
	free(listener);
}

struct mNetplayListener* mNetplaySimListen(struct mNetplaySimNetwork* net) {
	struct SimListener* listener = calloc(1, sizeof(*listener));
	if (!listener) {
		return NULL;
	}
	listener->d.accept = _simAccept;
	listener->d.destroy = _simListenerDestroy;
	listener->net = net;
	return &listener->d;
}

struct mNetplayLink* mNetplaySimConnect(struct mNetplaySimNetwork* net, struct mNetplayListener* base, const struct mNetplaySimLinkParams* params) {
	struct SimListener* listener = (struct SimListener*) base;
	if (listener->nPending == MAX_PENDING_ACCEPTS) {
		return NULL;
	}
	struct SimLink* client = _createLink(net, params);
	struct SimLink* server = _createLink(net, params);
	if (!client || !server) {
		free(client);
		free(server);
		return NULL;
	}
	client->remote = server;
	server->remote = client;
	listener->pending[listener->nPending] = server;
	listener->pendingAt[listener->nPending] = net->now + (params ? params->latencyMs : 0);
	++listener->nPending;
	return &client->d;
}

void mNetplaySimSever(struct mNetplayLink* base) {
	struct SimLink* link = (struct SimLink*) base;
	struct SimLink* remote = link->remote;
	uint64_t now = link->net->now;
	link->closed = true;
	if (remote) {
		remote->closed = true;
		remote->remoteClosed = true;
		remote->remoteClosedAt = now;
		remote->remote = NULL;
	}
	link->remoteClosed = true;
	link->remoteClosedAt = now;
	link->remote = NULL;
}
