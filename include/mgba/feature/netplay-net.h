/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_FEATURE_NETPLAY_NET_H
#define M_FEATURE_NETPLAY_NET_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/feature/netplay.h>

/* Netplay over a network: every site runs the same mNetplaySession and
 * only keys travel. The host (always slot 0) accepts the other players,
 * checks they run the same build and ROM, collects every battery save,
 * picks the input delay from measured round trips, and starts the game;
 * afterwards it relays every player's keys to the others and repairs any
 * site whose state hashes disagree with its own by sending a snapshot.
 *
 * Keys sampled when a site runs frame F take effect at frame F + delay,
 * so a frame can run as soon as every player's keys for it have arrived.
 */

#define mNETPLAY_PROTOCOL_VERSION 1
#define mNETPLAY_DEFAULT_PORT 5738

/* --- Transports ---------------------------------------------------------- */

/* One host-client connection: a reliable, ordered message channel and an
 * unreliable datagram channel. All calls are non-blocking. */
struct mNetplayLink {
	bool (*sendReliable)(struct mNetplayLink*, const void* data, size_t size);
	/* Returns the size of the next whole message and stores a malloc()ed
	 * copy in *data; 0 when none is waiting; -1 once the link is closed. */
	ssize_t (*recvReliable)(struct mNetplayLink*, void** data);
	bool (*sendUnreliable)(struct mNetplayLink*, const void* data, size_t size);
	/* Returns the size of the next datagram, 0 when none is waiting. */
	ssize_t (*recvUnreliable)(struct mNetplayLink*, void* buffer, size_t size);
	bool (*isOpen)(struct mNetplayLink*);
	void (*destroy)(struct mNetplayLink*);
};

struct mNetplayListener {
	/* Returns a newly connected link, or NULL when nobody is waiting. */
	struct mNetplayLink* (*accept)(struct mNetplayListener*);
	void (*destroy)(struct mNetplayListener*);
};

struct mNetplayClock {
	uint64_t (*now)(struct mNetplayClock*); /* milliseconds */
};

/* A deterministic in-memory network on a virtual clock, for tests. */
struct mNetplaySimNetwork;

struct mNetplaySimLinkParams {
	unsigned latencyMs; /* one way */
	unsigned jitterMs; /* added to datagrams, uniformly 0..jitterMs */
	unsigned lossPermille; /* datagrams dropped, per 1000 */
};

struct mNetplaySimNetwork* mNetplaySimNetworkCreate(uint32_t seed);
void mNetplaySimNetworkDestroy(struct mNetplaySimNetwork*);
struct mNetplayClock* mNetplaySimNetworkClock(struct mNetplaySimNetwork*);
void mNetplaySimNetworkAdvance(struct mNetplaySimNetwork*, unsigned ms);
struct mNetplayListener* mNetplaySimListen(struct mNetplaySimNetwork*);
struct mNetplayLink* mNetplaySimConnect(struct mNetplaySimNetwork*, struct mNetplayListener*, const struct mNetplaySimLinkParams*);
/* Cuts every link between the listener's side and this client's side. */
void mNetplaySimSever(struct mNetplayLink* clientLink);

/* TCP for messages and UDP for keys, on the same port number. */
struct mNetplayListener* mNetplaySocketListen(unsigned port);
struct mNetplayLink* mNetplaySocketConnect(const char* host, unsigned port, unsigned timeoutMs);
struct mNetplayClock* mNetplaySocketClock(void);

/* --- Peers --------------------------------------------------------------- */

enum mNetplayPeerState {
	mNETPLAY_PEER_CONNECTING,
	mNETPLAY_PEER_LOBBY,
	mNETPLAY_PEER_RUNNING,
	mNETPLAY_PEER_ENDED,
	mNETPLAY_PEER_FAILED,
};

enum mNetplayPeerStep {
	mNETPLAY_PEER_STEP_OK,
	/* Some player's keys for the next frame have not arrived; nothing ran
	 * and the local keys were not consumed. Try again shortly. */
	mNETPLAY_PEER_STEP_STALLED,
	mNETPLAY_PEER_STEP_NOT_RUNNING,
	mNETPLAY_PEER_STEP_ENDED,
	mNETPLAY_PEER_STEP_FAILED,
};

struct mNetplayPeerConfig {
	const void* rom;
	size_t romSize;
	/* This player's battery save; may be empty. */
	const void* save;
	size_t saveSize;
	/* Host only: */
	unsigned players;
	int64_t rtcEpochMs; /* 0: the current time */
	unsigned inputDelay; /* 0: chosen from measured round trips */
	/* Every site stops after this frame; 0 runs until the host ends it. */
	uint32_t endFrame;
	/* Overrides the build identity sent in the handshake (tests only). */
	const char* buildId;
	/* Called for every core once the session exists, just before it starts
	 * (so before the cores' first reset). A frontend can give the local
	 * core a video buffer here; it must not change anything that affects
	 * emulation. Runs inside mNetplayPeerPoll(). */
	void (*prepareCore)(void* context, struct mCore* core, unsigned slot, bool local);
	void* context;
};

struct mNetplayPeerStats {
	uint32_t stalls;
	uint64_t stalledMs;
	uint32_t desyncs;
	uint32_t resyncs;
	unsigned rttMs; /* worst current round trip to the host or any client */
};

struct mNetplayPeer;

struct mNetplayPeer* mNetplayPeerCreateHost(const struct mNetplayPeerConfig*, struct mNetplayListener*, struct mNetplayClock*);
struct mNetplayPeer* mNetplayPeerCreateClient(const struct mNetplayPeerConfig*, struct mNetplayLink*, struct mNetplayClock*);
/* Takes ownership of the listener or link passed at creation. */
void mNetplayPeerDestroy(struct mNetplayPeer*);

/* Handles network traffic; call often (every millisecond or so while
 * waiting, and at least once per frame). Never blocks. */
void mNetplayPeerPoll(struct mNetplayPeer*);
enum mNetplayPeerState mNetplayPeerGetState(const struct mNetplayPeer*);
const char* mNetplayPeerError(const struct mNetplayPeer*);

/* Runs the next frame if every player's keys for it are known. localKeys
 * are this player's keys for frame mNetplayPeerLocalInputFrame(). */
enum mNetplayPeerStep mNetplayPeerRunFrame(struct mNetplayPeer*, uint16_t localKeys);

unsigned mNetplayPeerLocalSlot(const struct mNetplayPeer*);
unsigned mNetplayPeerPlayers(const struct mNetplayPeer*);
/* Players in the lobby so far, the host included. */
unsigned mNetplayPeerJoined(const struct mNetplayPeer*);
unsigned mNetplayPeerInputDelay(const struct mNetplayPeer*);
uint32_t mNetplayPeerFrame(const struct mNetplayPeer*);
uint32_t mNetplayPeerLocalInputFrame(const struct mNetplayPeer*);
/* Bit per slot whose keys for the next frame are still missing. */
uint32_t mNetplayPeerWaitingMask(const struct mNetplayPeer*);
/* How many frames this site is ahead of the slowest other site, as far
 * as it can tell; a frontend can sleep a little while this exceeds 1. */
int32_t mNetplayPeerFrameAdvantage(const struct mNetplayPeer*);
void mNetplayPeerGetStats(const struct mNetplayPeer*, struct mNetplayPeerStats*);
/* The running session (NULL before the game starts). */
struct mNetplaySession* mNetplayPeerSession(struct mNetplayPeer*);

CXX_GUARD_END

#endif
