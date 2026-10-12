/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/feature/netplay-net.h>

#ifdef M_CORE_GBA

#include <mgba/core/log.h>
#include <mgba/core/version.h>
#include <mgba-util/sha1.h>
#include <mgba-util/vfs.h>

#include "feature/netplay/input.h"
#include "feature/netplay/protocol.h"

#include <time.h>

mLOG_DECLARE_CATEGORY(NETPLAY);

#define HASH_INTERVAL 60
#define HASH_HISTORY 64
#define LOBBY_PING_MS 50
#define KEEPALIVE_MS 100
#define RESEND_MS 16
#define TIMEOUT_MS 20000
#define MIN_RTT_SAMPLES 5
#define START_SETTLE_MS 500
#define FRAME_US 16743
#define MIN_DELAY 2
#define MAX_DELAY 12
#define MAX_SAVE_SIZE (1024 * 1024)

/* One connection: on the host, one per client slot; on a client, the host. */
struct Remote {
	struct mNetplayLink* link;
	unsigned slot;
	bool welcomed;
	uint64_t lastHeard;
	uint64_t lastSent;

	/* Round trips, from the echo fields of datagrams. */
	uint32_t echoTime;
	uint64_t echoReceivedAt;
	bool haveEcho;
	unsigned rttMs;
	unsigned rttMax;
	unsigned rttSamples;

	/* Keys the other side reports having, per slot. */
	uint32_t acked[mNETPLAY_MAX_PLAYERS];
	uint32_t frame;
	uint64_t frameAt;

	/* Host side: the client's battery save, and desync bookkeeping. */
	void* save;
	size_t saveSize;
	bool reportPending;
	uint32_t reportFrame;
	uint32_t reportHashes[mNETPLAY_MAX_PLAYERS];
	bool resyncInFlight;
	uint32_t resyncFrame;
};

struct mNetplayPeer {
	bool host;
	struct mNetplayPeerConfig config;
	struct mNetplayClock* clock;
	struct mNetplayListener* listener;
	struct Remote remotes[mNETPLAY_MAX_PLAYERS];
	enum mNetplayPeerState state;
	char error[160];

	unsigned slot;
	unsigned players;
	unsigned joined;
	unsigned delay;
	int64_t rtcEpochMs;
	uint32_t endFrame;
	uint8_t romSha1[20];
	char buildId[mNETPLAY_BUILD_ID_LENGTH];
	uint64_t lastJoinAt;

	struct mNetplaySession* session;
	struct mNetplayInputQueue input;
	uint16_t lastLocalKeys;

	bool stalled;
	uint64_t stallStart;
	struct mNetplayPeerStats stats;
	/* Client: the host finished and sent its last keys; play out the rest. */
	bool hostFinished;
	/* Host: keys arrived that the other clients have not been sent yet. */
	bool relayPending;

	uint32_t hashFrames[HASH_HISTORY];
	uint32_t hashes[HASH_HISTORY][mNETPLAY_MAX_PLAYERS];
};

static uint64_t _now(const struct mNetplayPeer* peer) {
	return peer->clock->now(peer->clock);
}

static void _fail(struct mNetplayPeer* peer, const char* error) {
	if (peer->state == mNETPLAY_PEER_FAILED || peer->state == mNETPLAY_PEER_ENDED) {
		return;
	}
	strncpy(peer->error, error, sizeof(peer->error) - 1);
	peer->state = mNETPLAY_PEER_FAILED;
	mLOG(NETPLAY, ERROR, "%s", error);
}

static void _end(struct mNetplayPeer* peer) {
	if (peer->state != mNETPLAY_PEER_FAILED) {
		peer->state = mNETPLAY_PEER_ENDED;
	}
}

static bool _sendMessage(struct Remote* remote, const struct mNetplayWriter* writer) {
	if (!remote->link || writer->failed) {
		return false;
	}
	return remote->link->sendReliable(remote->link, writer->data, writer->size);
}

static void _sendBye(struct Remote* remote, enum mNetplayBye reason) {
	struct mNetplayWriter writer;
	mNetplayWriterInit(&writer);
	mNetplayWriteU8(&writer, mNETPLAY_MSG_BYE);
	mNetplayWriteU8(&writer, reason);
	_sendMessage(remote, &writer);
	mNetplayWriterDeinit(&writer);
}

static void _writeRuns(const struct mNetplayPeer* peer, struct mNetplayWriter* writer, const unsigned* slots, unsigned nSlots, const struct Remote* remote, uint32_t maxFrames);
static unsigned _slotsFor(const struct mNetplayPeer* peer, const struct Remote* remote, unsigned* slots);

/* Host: the game is over; hand the client every key it still lacks so it
 * can play out its last frames without us. */
static void _sendFinished(struct mNetplayPeer* peer, struct Remote* remote) {
	unsigned slots[mNETPLAY_MAX_PLAYERS];
	unsigned nSlots = _slotsFor(peer, remote, slots);
	struct mNetplayWriter writer;
	mNetplayWriterInit(&writer);
	mNetplayWriteU8(&writer, mNETPLAY_MSG_BYE);
	mNetplayWriteU8(&writer, mNETPLAY_BYE_FINISHED);
	_writeRuns(peer, &writer, slots, nSlots, remote, UINT32_MAX);
	_sendMessage(remote, &writer);
	mNetplayWriterDeinit(&writer);
}

static void _dropRemote(struct Remote* remote) {
	if (remote->link) {
		remote->link->destroy(remote->link);
	}
	free(remote->save);
	memset(remote, 0, sizeof(*remote));
}

static struct Remote* _remoteAt(struct mNetplayPeer* peer, unsigned index) {
	if (index >= mNETPLAY_MAX_PLAYERS || !peer->remotes[index].link) {
		return NULL;
	}
	return &peer->remotes[index];
}

/* Ends the game for everyone: a linked GBA cannot simply be unplugged. */
static void _hostAbort(struct mNetplayPeer* peer, enum mNetplayBye reason, const char* error) {
	unsigned i;
	for (i = 1; i < mNETPLAY_MAX_PLAYERS; ++i) {
		struct Remote* remote = _remoteAt(peer, i);
		if (remote) {
			_sendBye(remote, reason);
		}
	}
	_fail(peer, error);
}

/* --- Datagrams ------------------------------------------------------------ */

/* The slots whose keys go to remote: the host forwards every slot but the
 * recipient's own, a client sends only its own. */
static unsigned _slotsFor(const struct mNetplayPeer* peer, const struct Remote* remote, unsigned* slots) {
	unsigned n = 0;
	unsigned slot;
	if (!peer->session) {
		return 0;
	}
	for (slot = 0; slot < peer->players; ++slot) {
		if (peer->host ? slot != remote->slot : slot == peer->slot) {
			slots[n++] = slot;
		}
	}
	return n;
}

/* Key runs for the given slots, from what remote has acknowledged up to
 * what we know, at most maxFrames per slot. */
static void _writeRuns(const struct mNetplayPeer* peer, struct mNetplayWriter* writer, const unsigned* slots, unsigned nSlots, const struct Remote* remote, uint32_t maxFrames) {
	struct {
		unsigned slot;
		uint32_t start;
		unsigned count;
	} runs[255];
	unsigned nRuns = 0;
	unsigned i;
	for (i = 0; i < nSlots; ++i) {
		unsigned slot = slots[i];
		uint32_t start = remote->acked[slot];
		uint32_t end = peer->input.known[slot];
		if (start < peer->input.floor) {
			start = peer->input.floor;
		}
		if (end - start > maxFrames && start < end) {
			end = start + maxFrames;
		}
		while (start < end && nRuns < 255) {
			unsigned count = end - start > 255 ? 255 : end - start;
			runs[nRuns].slot = slot;
			runs[nRuns].start = start;
			runs[nRuns].count = count;
			++nRuns;
			start += count;
		}
	}
	mNetplayWriteU8(writer, nRuns);
	for (i = 0; i < nRuns; ++i) {
		mNetplayWriteU8(writer, runs[i].slot);
		mNetplayWriteU32(writer, runs[i].start);
		mNetplayWriteU8(writer, runs[i].count);
		uint32_t f;
		for (f = runs[i].start; f < runs[i].start + runs[i].count; ++f) {
			mNetplayWriteU16(writer, mNetplayInputGet(&peer->input, runs[i].slot, f));
		}
	}
}

/* Applies key runs from remote; false on a protocol violation. */
static bool _readRuns(struct mNetplayPeer* peer, struct mNetplayReader* reader, const struct Remote* remote) {
	unsigned nRuns = mNetplayReadU8(reader);
	unsigned i;
	for (i = 0; i < nRuns && !reader->failed; ++i) {
		unsigned slot = mNetplayReadU8(reader);
		uint32_t start = mNetplayReadU32(reader);
		unsigned count = mNetplayReadU8(reader);
		uint16_t keys[255];
		unsigned k;
		for (k = 0; k < count; ++k) {
			keys[k] = mNetplayReadU16(reader);
		}
		if (reader->failed || !peer->session || slot >= peer->players) {
			return !reader->failed;
		}
		/* A client only speaks for itself; the host speaks for everyone
		 * but the recipient. */
		if (peer->host ? slot != remote->slot : slot == peer->slot) {
			continue;
		}
		uint32_t known = peer->input.known[slot];
		if (!mNetplayInputAdd(&peer->input, slot, start, keys, count)) {
			return false;
		}
		if (peer->input.known[slot] != known) {
			peer->relayPending = true;
		}
	}
	return true;
}

static void _sendDatagram(struct mNetplayPeer* peer, struct Remote* remote) {
	struct mNetplayWriter writer;
	mNetplayWriterInit(&writer);
	uint64_t now = _now(peer);
	mNetplayWriteU8(&writer, mNETPLAY_DGRAM_KEYS);
	mNetplayWriteU8(&writer, peer->slot);
	mNetplayWriteU32(&writer, (uint32_t) now);
	mNetplayWriteU32(&writer, remote->haveEcho ? remote->echoTime : 0);
	mNetplayWriteU32(&writer, remote->haveEcho ? (uint32_t) (now - remote->echoReceivedAt) : 0);
	mNetplayWriteU32(&writer, peer->session ? mNetplaySessionFrame(peer->session) : 0);

	unsigned slots[mNETPLAY_MAX_PLAYERS];
	unsigned nSlots = _slotsFor(peer, remote, slots);
	unsigned slot;

	/* Acks: what we have of the keys the other side sends us. */
	if (peer->session) {
		unsigned nAcks = 0;
		unsigned ackSlots[mNETPLAY_MAX_PLAYERS];
		for (slot = 0; slot < peer->players; ++slot) {
			if (peer->host ? slot == remote->slot : slot != peer->slot) {
				ackSlots[nAcks++] = slot;
			}
		}
		mNetplayWriteU8(&writer, nAcks);
		for (slot = 0; slot < nAcks; ++slot) {
			mNetplayWriteU8(&writer, ackSlots[slot]);
			mNetplayWriteU32(&writer, peer->input.known[ackSlots[slot]]);
		}
	} else {
		mNetplayWriteU8(&writer, 0);
	}

	_writeRuns(peer, &writer, slots, nSlots, remote, mNETPLAY_MAX_RUN);

	if (!writer.failed && writer.size <= mNETPLAY_MAX_DATAGRAM) {
		remote->link->sendUnreliable(remote->link, writer.data, writer.size);
	}
	remote->lastSent = now;
	mNetplayWriterDeinit(&writer);
}

static void _sendDatagrams(struct mNetplayPeer* peer) {
	unsigned i;
	for (i = 0; i < mNETPLAY_MAX_PLAYERS; ++i) {
		struct Remote* remote = _remoteAt(peer, i);
		if (remote && (!peer->host || remote->welcomed)) {
			_sendDatagram(peer, remote);
		}
	}
}

static void _handleDatagram(struct mNetplayPeer* peer, struct Remote* remote, const uint8_t* data, size_t size) {
	struct mNetplayReader reader;
	mNetplayReaderInit(&reader, data, size);
	if (mNetplayReadU8(&reader) != mNETPLAY_DGRAM_KEYS) {
		return;
	}
	unsigned sender = mNetplayReadU8(&reader);
	uint32_t sendTime = mNetplayReadU32(&reader);
	uint32_t echoTime = mNetplayReadU32(&reader);
	uint32_t echoDelay = mNetplayReadU32(&reader);
	uint32_t frame = mNetplayReadU32(&reader);
	if (reader.failed) {
		return;
	}
	if (peer->host ? sender != remote->slot : sender != 0) {
		return;
	}
	uint64_t now = _now(peer);
	remote->lastHeard = now;
	remote->echoTime = sendTime;
	remote->echoReceivedAt = now;
	remote->haveEcho = true;
	if (echoTime) {
		uint32_t rtt = (uint32_t) now - echoTime - echoDelay;
		if (rtt < 60000) {
			remote->rttMs = remote->rttSamples ? (remote->rttMs * 7 + rtt) / 8 : rtt;
			if (rtt > remote->rttMax) {
				remote->rttMax = rtt;
			}
			++remote->rttSamples;
		}
	}
	if (frame >= remote->frame) {
		remote->frame = frame;
		remote->frameAt = now;
	}

	unsigned nAcks = mNetplayReadU8(&reader);
	unsigned i;
	for (i = 0; i < nAcks && !reader.failed; ++i) {
		unsigned slot = mNetplayReadU8(&reader);
		uint32_t known = mNetplayReadU32(&reader);
		if (!reader.failed && slot < mNETPLAY_MAX_PLAYERS && known > remote->acked[slot]) {
			remote->acked[slot] = known;
		}
	}

	if (!_readRuns(peer, &reader, remote)) {
		if (peer->host) {
			_hostAbort(peer, mNETPLAY_BYE_ERROR, "Received conflicting keys");
		} else {
			_fail(peer, "Received conflicting keys");
		}
	}
}

static void _pollDatagrams(struct mNetplayPeer* peer, struct Remote* remote) {
	uint8_t buffer[mNETPLAY_MAX_DATAGRAM];
	ssize_t size;
	while (remote->link && (size = remote->link->recvUnreliable(remote->link, buffer, sizeof(buffer))) > 0) {
		_handleDatagram(peer, remote, buffer, size);
	}
}

/* --- Sessions --------------------------------------------------------------- */

static bool _startSession(struct mNetplayPeer* peer, const void* const* saves, const size_t* saveSizes) {
	struct mNetplaySessionSettings settings = {
		.players = peer->players,
		.rtcEpochMs = peer->rtcEpochMs,
	};
	peer->session = mNetplaySessionCreate(&settings);
	if (!peer->session || !mNetplaySessionLoadROM(peer->session, peer->config.rom, peer->config.romSize)) {
		return false;
	}
	unsigned slot;
	for (slot = 0; slot < peer->players; ++slot) {
		if (!mNetplaySessionLoadSave(peer->session, slot, saves[slot], saveSizes[slot])) {
			return false;
		}
	}
	if (!mNetplaySessionStart(peer->session)) {
		return false;
	}
	mNetplayInputInit(&peer->input, peer->players, peer->delay);
	unsigned i;
	for (i = 0; i < mNETPLAY_MAX_PLAYERS; ++i) {
		for (slot = 0; slot < mNETPLAY_MAX_PLAYERS; ++slot) {
			peer->remotes[i].acked[slot] = peer->delay;
		}
	}
	peer->state = mNETPLAY_PEER_RUNNING;
	return true;
}

static void _hashFrame(struct mNetplayPeer* peer, uint32_t frame, uint32_t* hashes) {
	unsigned slot;
	for (slot = 0; slot < peer->players; ++slot) {
		hashes[slot] = mNetplaySessionHashCore(peer->session, slot);
	}
	UNUSED(frame);
}

static void _sendSnapshot(struct mNetplayPeer* peer, struct Remote* remote) {
	struct VFile* vf = VFileMemChunk(NULL, 0);
	if (!vf) {
		return;
	}
	if (mNetplaySessionSaveSnapshot(peer->session, vf)) {
		size_t size = vf->size(vf);
		void* data = vf->map(vf, size, MAP_READ);
		struct mNetplayWriter writer;
		mNetplayWriterInit(&writer);
		mNetplayWriteU8(&writer, mNETPLAY_MSG_SNAPSHOT);
		mNetplayWriteU32(&writer, mNetplaySessionFrame(peer->session));
		mNetplayWriteBlob(&writer, data, size);
		if (_sendMessage(remote, &writer)) {
			remote->resyncInFlight = true;
			remote->resyncFrame = mNetplaySessionFrame(peer->session);
			++peer->stats.resyncs;
			mLOG(NETPLAY, INFO, "Sent player %u a snapshot of frame %u", remote->slot + 1, remote->resyncFrame);
		}
		mNetplayWriterDeinit(&writer);
		vf->unmap(vf, data, size);
	}
	vf->close(vf);
}

/* Host: a client's hashes for a frame the host has also hashed. */
static void _checkReport(struct mNetplayPeer* peer, struct Remote* remote, uint32_t frame, const uint32_t* hashes) {
	unsigned index = (frame / HASH_INTERVAL) % HASH_HISTORY;
	if (peer->hashFrames[index] != frame) {
		return;
	}
	bool match = !memcmp(peer->hashes[index], hashes, sizeof(*hashes) * peer->players);
	if (match) {
		if (remote->resyncInFlight && frame > remote->resyncFrame) {
			remote->resyncInFlight = false;
		}
		return;
	}
	if (remote->resyncInFlight && frame <= remote->resyncFrame + 2 * HASH_INTERVAL) {
		/* Computed before the snapshot arrived; give it time. */
		return;
	}
	++peer->stats.desyncs;
	mLOG(NETPLAY, WARN, "Player %u desynchronized at frame %u", remote->slot + 1, frame);
	_sendSnapshot(peer, remote);
}

static void _afterFrame(struct mNetplayPeer* peer) {
	uint32_t frame = mNetplaySessionFrame(peer->session);
	if (frame % HASH_INTERVAL) {
		return;
	}
	uint32_t hashes[mNETPLAY_MAX_PLAYERS] = {0};
	_hashFrame(peer, frame, hashes);
	if (peer->host) {
		unsigned index = (frame / HASH_INTERVAL) % HASH_HISTORY;
		peer->hashFrames[index] = frame;
		memcpy(peer->hashes[index], hashes, sizeof(hashes));
		unsigned i;
		for (i = 1; i < mNETPLAY_MAX_PLAYERS; ++i) {
			struct Remote* remote = _remoteAt(peer, i);
			if (remote && remote->reportPending && remote->reportFrame == frame) {
				remote->reportPending = false;
				_checkReport(peer, remote, frame, remote->reportHashes);
			}
		}
	} else {
		struct mNetplayWriter writer;
		mNetplayWriterInit(&writer);
		mNetplayWriteU8(&writer, mNETPLAY_MSG_HASHES);
		mNetplayWriteU32(&writer, frame);
		mNetplayWriteU8(&writer, peer->players);
		unsigned slot;
		for (slot = 0; slot < peer->players; ++slot) {
			mNetplayWriteU32(&writer, hashes[slot]);
		}
		_sendMessage(&peer->remotes[0], &writer);
		mNetplayWriterDeinit(&writer);
	}
}

/* --- Host ------------------------------------------------------------------- */

static void _reject(struct Remote* remote, enum mNetplayReject reason, const char* text) {
	struct mNetplayWriter writer;
	mNetplayWriterInit(&writer);
	mNetplayWriteU8(&writer, mNETPLAY_MSG_REJECT);
	mNetplayWriteU8(&writer, reason);
	mNetplayWriteBlob(&writer, text, strlen(text));
	_sendMessage(remote, &writer);
	mNetplayWriterDeinit(&writer);
	mLOG(NETPLAY, INFO, "Turned away a player: %s", text);
}

static void _broadcastLobby(struct mNetplayPeer* peer) {
	struct mNetplayWriter writer;
	mNetplayWriterInit(&writer);
	mNetplayWriteU8(&writer, mNETPLAY_MSG_LOBBY);
	mNetplayWriteU8(&writer, peer->joined);
	mNetplayWriteU8(&writer, peer->players);
	unsigned i;
	for (i = 1; i < mNETPLAY_MAX_PLAYERS; ++i) {
		struct Remote* remote = _remoteAt(peer, i);
		if (remote && remote->welcomed) {
			_sendMessage(remote, &writer);
		}
	}
	mNetplayWriterDeinit(&writer);
}

static void _hostHello(struct mNetplayPeer* peer, struct Remote* remote, struct mNetplayReader* reader) {
	uint32_t magic = mNetplayReadU32(reader);
	uint32_t protocol = mNetplayReadU32(reader);
	uint32_t stepper = mNetplayReadU32(reader);
	char build[mNETPLAY_BUILD_ID_LENGTH];
	uint8_t romSha1[20];
	mNetplayReadBytes(reader, build, sizeof(build));
	mNetplayReadBytes(reader, romSha1, sizeof(romSha1));
	size_t saveSize;
	const void* save = mNetplayReadBlob(reader, &saveSize);
	if (reader->failed || magic != mNETPLAY_MAGIC) {
		_reject(remote, mNETPLAY_REJECT_MALFORMED, "Not a netplay client");
	} else if (protocol != mNETPLAY_PROTOCOL_VERSION || stepper != mNETPLAY_STEPPER_VERSION) {
		_reject(remote, mNETPLAY_REJECT_PROTOCOL, "Different netplay protocol version");
	} else if (memcmp(build, peer->buildId, sizeof(build))) {
		_reject(remote, mNETPLAY_REJECT_BUILD, "Different emulator build; everyone must run the same one");
	} else if (memcmp(romSha1, peer->romSha1, sizeof(romSha1))) {
		_reject(remote, mNETPLAY_REJECT_ROM, "Different ROM; everyone must load the same one");
	} else if (saveSize > MAX_SAVE_SIZE) {
		_reject(remote, mNETPLAY_REJECT_MALFORMED, "Battery save too large");
	} else {
		remote->save = malloc(saveSize ? saveSize : 1);
		if (!remote->save) {
			_reject(remote, mNETPLAY_REJECT_MALFORMED, "Out of memory");
		} else {
			memcpy(remote->save, save, saveSize);
			remote->saveSize = saveSize;
			remote->welcomed = true;
			++peer->joined;
			peer->lastJoinAt = _now(peer);
			struct mNetplayWriter writer;
			mNetplayWriterInit(&writer);
			mNetplayWriteU8(&writer, mNETPLAY_MSG_WELCOME);
			mNetplayWriteU8(&writer, remote->slot);
			mNetplayWriteU8(&writer, peer->players);
			_sendMessage(remote, &writer);
			mNetplayWriterDeinit(&writer);
			mLOG(NETPLAY, INFO, "Player %u joined", remote->slot + 1);
			_broadcastLobby(peer);
			return;
		}
	}
	/* Rejected: let the REJECT go out before forgetting the link. */
	remote->link->destroy(remote->link);
	remote->link = NULL;
	_dropRemote(remote);
}

static unsigned _chooseDelay(const struct mNetplayPeer* peer) {
	if (peer->config.inputDelay) {
		return peer->config.inputDelay;
	}
	/* Keys travel client -> host -> client, so the worst path is about the
	 * worst round trip; leave a frame for timing slop and jitter. */
	unsigned worst = 0;
	unsigned i;
	for (i = 1; i < peer->players; ++i) {
		const struct Remote* remote = &peer->remotes[i];
		unsigned rtt = remote->rttMax > remote->rttMs ? remote->rttMax : remote->rttMs;
		if (rtt > worst) {
			worst = rtt;
		}
	}
	unsigned delay = ((worst + 4) * 1000 + FRAME_US - 1) / FRAME_US + 1;
	if (delay < MIN_DELAY) {
		delay = MIN_DELAY;
	}
	if (delay > MAX_DELAY) {
		delay = MAX_DELAY;
	}
	return delay;
}

static void _hostTryStart(struct mNetplayPeer* peer) {
	if (peer->joined + 1 < peer->players || _now(peer) - peer->lastJoinAt < START_SETTLE_MS) {
		return;
	}
	unsigned i;
	for (i = 1; i < peer->players; ++i) {
		if (peer->remotes[i].rttSamples < MIN_RTT_SAMPLES) {
			return;
		}
	}
	peer->delay = _chooseDelay(peer);
	const void* saves[mNETPLAY_MAX_PLAYERS] = { peer->config.save };
	size_t saveSizes[mNETPLAY_MAX_PLAYERS] = { peer->config.saveSize };
	for (i = 1; i < peer->players; ++i) {
		saves[i] = peer->remotes[i].save;
		saveSizes[i] = peer->remotes[i].saveSize;
	}

	struct mNetplayWriter writer;
	mNetplayWriterInit(&writer);
	mNetplayWriteU8(&writer, mNETPLAY_MSG_START);
	mNetplayWriteU8(&writer, peer->players);
	mNetplayWriteU8(&writer, peer->delay);
	mNetplayWriteU64(&writer, (uint64_t) peer->rtcEpochMs);
	mNetplayWriteU32(&writer, peer->endFrame);
	for (i = 0; i < peer->players; ++i) {
		mNetplayWriteBlob(&writer, saves[i], saveSizes[i]);
	}
	for (i = 1; i < peer->players; ++i) {
		if (!_sendMessage(&peer->remotes[i], &writer)) {
			mNetplayWriterDeinit(&writer);
			_hostAbort(peer, mNETPLAY_BYE_PLAYER_LOST, "Lost a player while starting");
			return;
		}
	}
	mNetplayWriterDeinit(&writer);
	mLOG(NETPLAY, INFO, "Starting with %u players and an input delay of %u frames", peer->players, peer->delay);
	if (!_startSession(peer, saves, saveSizes)) {
		_hostAbort(peer, mNETPLAY_BYE_ERROR, "Could not start the game");
	}
}

static void _hostMessage(struct mNetplayPeer* peer, struct Remote* remote, const void* data, size_t size) {
	struct mNetplayReader reader;
	mNetplayReaderInit(&reader, data, size);
	uint8_t type = mNetplayReadU8(&reader);
	remote->lastHeard = _now(peer);
	if (!remote->welcomed) {
		if (type == mNETPLAY_MSG_HELLO && peer->state == mNETPLAY_PEER_LOBBY) {
			_hostHello(peer, remote, &reader);
		}
		return;
	}
	switch (type) {
	case mNETPLAY_MSG_HASHES: {
		uint32_t frame = mNetplayReadU32(&reader);
		unsigned n = mNetplayReadU8(&reader);
		uint32_t hashes[mNETPLAY_MAX_PLAYERS] = {0};
		unsigned i;
		for (i = 0; i < n && i < mNETPLAY_MAX_PLAYERS; ++i) {
			hashes[i] = mNetplayReadU32(&reader);
		}
		if (reader.failed || n != peer->players || !peer->session) {
			return;
		}
		if (frame <= mNetplaySessionFrame(peer->session)) {
			_checkReport(peer, remote, frame, hashes);
		} else {
			remote->reportPending = true;
			remote->reportFrame = frame;
			memcpy(remote->reportHashes, hashes, sizeof(hashes));
		}
		break;
	}
	case mNETPLAY_MSG_BYE:
		if (peer->state == mNETPLAY_PEER_RUNNING) {
			char error[64];
			snprintf(error, sizeof(error), "Player %u left", remote->slot + 1);
			_hostAbort(peer, mNETPLAY_BYE_PLAYER_LOST, error);
		} else if (peer->state == mNETPLAY_PEER_LOBBY) {
			_dropRemote(remote);
			--peer->joined;
			_broadcastLobby(peer);
		}
		break;
	default:
		break;
	}
}

static void _hostPoll(struct mNetplayPeer* peer) {
	uint64_t now = _now(peer);
	struct mNetplayLink* link;
	while (peer->listener && (link = peer->listener->accept(peer->listener))) {
		unsigned slot;
		struct Remote* remote = NULL;
		if (peer->state == mNETPLAY_PEER_LOBBY) {
			for (slot = 1; slot < peer->players; ++slot) {
				if (!peer->remotes[slot].link) {
					remote = &peer->remotes[slot];
					remote->slot = slot;
					break;
				}
			}
		}
		if (!remote) {
			struct Remote stranger = { .link = link };
			_reject(&stranger, mNETPLAY_REJECT_FULL, "The game is full or already running");
			link->destroy(link);
			continue;
		}
		remote->link = link;
		remote->lastHeard = now;
	}

	unsigned i;
	for (i = 1; i < mNETPLAY_MAX_PLAYERS; ++i) {
		struct Remote* remote = _remoteAt(peer, i);
		if (!remote) {
			continue;
		}
		void* message;
		ssize_t size;
		while (remote->link && (size = remote->link->recvReliable(remote->link, &message)) > 0) {
			_hostMessage(peer, remote, message, size);
			free(message);
			if (peer->state == mNETPLAY_PEER_FAILED) {
				return;
			}
		}
		if (!remote->link) {
			continue;
		}
		bool lost = size < 0 || !remote->link->isOpen(remote->link) || now - remote->lastHeard > TIMEOUT_MS;
		if (lost) {
			if (peer->state == mNETPLAY_PEER_RUNNING) {
				char error[64];
				snprintf(error, sizeof(error), "Lost the connection to player %u", remote->slot + 1);
				remote->link->destroy(remote->link);
				remote->link = NULL;
				_hostAbort(peer, mNETPLAY_BYE_PLAYER_LOST, error);
				return;
			}
			if (remote->welcomed) {
				--peer->joined;
			}
			_dropRemote(remote);
			_broadcastLobby(peer);
			continue;
		}
		_pollDatagrams(peer, remote);
		if (peer->state == mNETPLAY_PEER_FAILED) {
			return;
		}
	}

	if (peer->state == mNETPLAY_PEER_LOBBY) {
		_hostTryStart(peer);
	} else if (peer->relayPending && peer->state == mNETPLAY_PEER_RUNNING) {
		/* Forward fresh keys now rather than with our next frame: a key
		 * going from one client to another would otherwise sit here for up
		 * to a frame, eating most of the input delay's slack. */
		peer->relayPending = false;
		_sendDatagrams(peer);
	}
}

/* --- Client ----------------------------------------------------------------- */

static void _clientStart(struct mNetplayPeer* peer, struct mNetplayReader* reader) {
	unsigned players = mNetplayReadU8(reader);
	unsigned delay = mNetplayReadU8(reader);
	int64_t epoch = (int64_t) mNetplayReadU64(reader);
	uint32_t endFrame = mNetplayReadU32(reader);
	const void* saves[mNETPLAY_MAX_PLAYERS] = {0};
	size_t saveSizes[mNETPLAY_MAX_PLAYERS] = {0};
	unsigned slot;
	for (slot = 0; slot < players && slot < mNETPLAY_MAX_PLAYERS; ++slot) {
		saves[slot] = mNetplayReadBlob(reader, &saveSizes[slot]);
	}
	if (reader->failed || players != peer->players || delay < 1 || delay > MAX_DELAY) {
		_fail(peer, "The host sent a malformed start message");
		return;
	}
	peer->delay = delay;
	peer->rtcEpochMs = epoch;
	peer->endFrame = endFrame;
	if (!_startSession(peer, saves, saveSizes)) {
		_fail(peer, "Could not start the game");
	}
}

static void _clientSnapshot(struct mNetplayPeer* peer, struct mNetplayReader* reader) {
	uint32_t frame = mNetplayReadU32(reader);
	size_t size;
	const void* data = mNetplayReadBlob(reader, &size);
	if (reader->failed || !peer->session) {
		return;
	}
	struct VFile* vf = VFileFromConstMemory(data, size);
	bool ok = vf && mNetplaySessionLoadSnapshot(peer->session, vf);
	if (vf) {
		vf->close(vf);
	}
	if (!ok || mNetplaySessionFrame(peer->session) != frame) {
		_fail(peer, "Could not load the host's snapshot");
		return;
	}
	++peer->stats.resyncs;
	/* Frames before the snapshot will never run here again. */
	mNetplayInputRebase(&peer->input, frame);
	/* Keys we never got to send for frames up to its frame plus the delay
	 * must still exist, or everyone waits for them forever. Repeat the
	 * last keys. */
	uint32_t needed = frame + peer->delay;
	while (peer->input.known[peer->slot] < needed) {
		uint16_t keys = peer->lastLocalKeys;
		mNetplayInputAdd(&peer->input, peer->slot, peer->input.known[peer->slot], &keys, 1);
	}
	mLOG(NETPLAY, INFO, "Resynchronized to the host at frame %u", frame);
}

static void _clientMessage(struct mNetplayPeer* peer, const void* data, size_t size) {
	struct mNetplayReader reader;
	mNetplayReaderInit(&reader, data, size);
	uint8_t type = mNetplayReadU8(&reader);
	switch (type) {
	case mNETPLAY_MSG_WELCOME:
		peer->slot = mNetplayReadU8(&reader);
		peer->players = mNetplayReadU8(&reader);
		if (reader.failed || !peer->slot || peer->slot >= peer->players || peer->players > mNETPLAY_MAX_PLAYERS) {
			_fail(peer, "The host sent a malformed welcome");
			return;
		}
		peer->state = mNETPLAY_PEER_LOBBY;
		break;
	case mNETPLAY_MSG_REJECT: {
		mNetplayReadU8(&reader);
		size_t length;
		const char* text = mNetplayReadBlob(&reader, &length);
		char error[160] = "The host turned us away: ";
		size_t prefix = strlen(error);
		if (text && length > sizeof(error) - prefix - 1) {
			length = sizeof(error) - prefix - 1;
		}
		if (text) {
			memcpy(&error[prefix], text, length);
			error[prefix + length] = '\0';
		}
		_fail(peer, error);
		break;
	}
	case mNETPLAY_MSG_LOBBY:
		peer->joined = mNetplayReadU8(&reader);
		break;
	case mNETPLAY_MSG_START:
		if (peer->state == mNETPLAY_PEER_LOBBY) {
			_clientStart(peer, &reader);
		}
		break;
	case mNETPLAY_MSG_SNAPSHOT:
		if (peer->state == mNETPLAY_PEER_RUNNING) {
			_clientSnapshot(peer, &reader);
		}
		break;
	case mNETPLAY_MSG_BYE: {
		uint8_t reason = mNetplayReadU8(&reader);
		if (reason == mNETPLAY_BYE_FINISHED) {
			if (peer->session && _readRuns(peer, &reader, &peer->remotes[0]) && mNetplaySessionFrame(peer->session) < peer->endFrame) {
				peer->hostFinished = true;
			} else {
				_end(peer);
			}
		} else if (reason == mNETPLAY_BYE_PLAYER_LOST) {
			_fail(peer, "Another player dropped out");
		} else {
			_fail(peer, "The host ended the game");
		}
		break;
	}
	default:
		break;
	}
}

static void _clientPoll(struct mNetplayPeer* peer) {
	struct Remote* host = &peer->remotes[0];
	if (!host->link) {
		return;
	}
	uint64_t now = _now(peer);
	void* message;
	ssize_t size;
	while ((size = host->link->recvReliable(host->link, &message)) > 0) {
		host->lastHeard = now;
		_clientMessage(peer, message, size);
		free(message);
		if (peer->state == mNETPLAY_PEER_FAILED || peer->state == mNETPLAY_PEER_ENDED) {
			return;
		}
	}
	if (peer->hostFinished) {
		return;
	}
	if (size < 0 || !host->link->isOpen(host->link)) {
		_fail(peer, "Lost the connection to the host");
		return;
	}
	if (now - host->lastHeard > TIMEOUT_MS) {
		_fail(peer, "The host stopped responding");
		return;
	}
	_pollDatagrams(peer, host);
}

static void _sendHello(struct mNetplayPeer* peer) {
	struct mNetplayWriter writer;
	mNetplayWriterInit(&writer);
	mNetplayWriteU8(&writer, mNETPLAY_MSG_HELLO);
	mNetplayWriteU32(&writer, mNETPLAY_MAGIC);
	mNetplayWriteU32(&writer, mNETPLAY_PROTOCOL_VERSION);
	mNetplayWriteU32(&writer, mNETPLAY_STEPPER_VERSION);
	mNetplayWriteBytes(&writer, peer->buildId, sizeof(peer->buildId));
	mNetplayWriteBytes(&writer, peer->romSha1, sizeof(peer->romSha1));
	mNetplayWriteBlob(&writer, peer->config.save, peer->config.saveSize);
	if (!_sendMessage(&peer->remotes[0], &writer)) {
		_fail(peer, "Could not reach the host");
	}
	mNetplayWriterDeinit(&writer);
}

/* --- Public ------------------------------------------------------------------ */

static struct mNetplayPeer* _create(const struct mNetplayPeerConfig* config, struct mNetplayClock* clock) {
	if (!config || !config->rom || !config->romSize || !clock) {
		return NULL;
	}
	struct mNetplayPeer* peer = calloc(1, sizeof(*peer));
	if (!peer) {
		return NULL;
	}
	peer->config = *config;
	peer->clock = clock;
	peer->endFrame = config->endFrame;
	sha1Buffer(config->rom, config->romSize, peer->romSha1);
	strncpy(peer->buildId, config->buildId ? config->buildId : gitCommit, sizeof(peer->buildId) - 1);
	return peer;
}

struct mNetplayPeer* mNetplayPeerCreateHost(const struct mNetplayPeerConfig* config, struct mNetplayListener* listener, struct mNetplayClock* clock) {
	if (!listener || !config || config->players < 2 || config->players > mNETPLAY_MAX_PLAYERS) {
		return NULL;
	}
	struct mNetplayPeer* peer = _create(config, clock);
	if (!peer) {
		return NULL;
	}
	peer->host = true;
	peer->listener = listener;
	peer->players = config->players;
	peer->slot = 0;
	peer->rtcEpochMs = config->rtcEpochMs ? config->rtcEpochMs : (int64_t) time(NULL) * 1000;
	peer->lastJoinAt = _now(peer);
	peer->state = mNETPLAY_PEER_LOBBY;
	return peer;
}

struct mNetplayPeer* mNetplayPeerCreateClient(const struct mNetplayPeerConfig* config, struct mNetplayLink* link, struct mNetplayClock* clock) {
	if (!link) {
		return NULL;
	}
	struct mNetplayPeer* peer = _create(config, clock);
	if (!peer) {
		return NULL;
	}
	peer->remotes[0].link = link;
	peer->remotes[0].lastHeard = _now(peer);
	peer->state = mNETPLAY_PEER_CONNECTING;
	_sendHello(peer);
	return peer;
}

void mNetplayPeerDestroy(struct mNetplayPeer* peer) {
	if (!peer) {
		return;
	}
	unsigned i;
	for (i = 0; i < mNETPLAY_MAX_PLAYERS; ++i) {
		struct Remote* remote = _remoteAt(peer, i);
		if (remote && peer->state != mNETPLAY_PEER_ENDED && peer->state != mNETPLAY_PEER_FAILED) {
			_sendBye(remote, mNETPLAY_BYE_QUIT);
		}
		_dropRemote(&peer->remotes[i]);
	}
	if (peer->listener) {
		peer->listener->destroy(peer->listener);
	}
	mNetplaySessionDestroy(peer->session);
	free(peer);
}

void mNetplayPeerPoll(struct mNetplayPeer* peer) {
	if (peer->state == mNETPLAY_PEER_FAILED || peer->state == mNETPLAY_PEER_ENDED) {
		return;
	}
	if (peer->host) {
		_hostPoll(peer);
	} else {
		_clientPoll(peer);
	}
	if (peer->state == mNETPLAY_PEER_FAILED || peer->state == mNETPLAY_PEER_ENDED) {
		return;
	}
	/* Pings in the lobby, keepalives while running, and quick resends
	 * while stalled so lost keys are replaced fast. */
	uint64_t now = _now(peer);
	unsigned interval = peer->state != mNETPLAY_PEER_RUNNING ? LOBBY_PING_MS : peer->stalled ? RESEND_MS : KEEPALIVE_MS;
	unsigned i;
	for (i = 0; i < mNETPLAY_MAX_PLAYERS; ++i) {
		struct Remote* remote = _remoteAt(peer, i);
		if (remote && (!peer->host || remote->welcomed) && !peer->hostFinished && now - remote->lastSent >= interval) {
			_sendDatagram(peer, remote);
		}
	}
}

enum mNetplayPeerState mNetplayPeerGetState(const struct mNetplayPeer* peer) {
	return peer->state;
}

const char* mNetplayPeerError(const struct mNetplayPeer* peer) {
	return peer->error;
}

enum mNetplayPeerStep mNetplayPeerRunFrame(struct mNetplayPeer* peer, uint16_t localKeys) {
	switch (peer->state) {
	case mNETPLAY_PEER_RUNNING:
		break;
	case mNETPLAY_PEER_ENDED:
		return mNETPLAY_PEER_STEP_ENDED;
	case mNETPLAY_PEER_FAILED:
		return mNETPLAY_PEER_STEP_FAILED;
	default:
		return mNETPLAY_PEER_STEP_NOT_RUNNING;
	}
	uint32_t frame = mNetplaySessionFrame(peer->session);
	if (peer->endFrame && frame >= peer->endFrame) {
		_end(peer);
		return mNETPLAY_PEER_STEP_ENDED;
	}
	uint64_t now = _now(peer);
	if (mNetplayInputMissing(&peer->input, frame)) {
		if (!peer->stalled) {
			peer->stalled = true;
			peer->stallStart = now;
			++peer->stats.stalls;
		}
		return mNETPLAY_PEER_STEP_STALLED;
	}
	if (peer->stalled) {
		peer->stats.stalledMs += now - peer->stallStart;
		peer->stalled = false;
	}

	/* Local keys for frame + delay; after a snapshot from behind those may
	 * already be decided, and what was sent stands. */
	localKeys &= mNETPLAY_KEY_MASK;
	uint32_t target = frame + peer->delay;
	if (peer->input.known[peer->slot] == target) {
		mNetplayInputAdd(&peer->input, peer->slot, target, &localKeys, 1);
		peer->lastLocalKeys = localKeys;
	}

	uint16_t keys[mNETPLAY_MAX_PLAYERS] = {0};
	unsigned slot;
	for (slot = 0; slot < peer->players; ++slot) {
		keys[slot] = mNetplayInputGet(&peer->input, slot, frame);
	}
	if (mNetplaySessionRunFrame(peer->session, keys) != mNETPLAY_STEP_OK) {
		if (peer->host) {
			_hostAbort(peer, mNETPLAY_BYE_ERROR, "The emulated link locked up");
		} else {
			_fail(peer, "The emulated link locked up");
		}
		return mNETPLAY_PEER_STEP_FAILED;
	}
	++frame;
	mNetplayInputSetFloor(&peer->input, frame > mNETPLAY_INPUT_HISTORY ? frame - mNETPLAY_INPUT_HISTORY : 0);
	_afterFrame(peer);
	_sendDatagrams(peer);

	if (peer->endFrame && frame >= peer->endFrame) {
		if (peer->host) {
			unsigned i;
			for (i = 1; i < mNETPLAY_MAX_PLAYERS; ++i) {
				struct Remote* remote = _remoteAt(peer, i);
				if (remote) {
					_sendFinished(peer, remote);
				}
			}
		}
		_end(peer);
	}
	return mNETPLAY_PEER_STEP_OK;
}

unsigned mNetplayPeerLocalSlot(const struct mNetplayPeer* peer) {
	return peer->slot;
}

unsigned mNetplayPeerPlayers(const struct mNetplayPeer* peer) {
	return peer->players;
}

unsigned mNetplayPeerInputDelay(const struct mNetplayPeer* peer) {
	return peer->delay;
}

uint32_t mNetplayPeerFrame(const struct mNetplayPeer* peer) {
	return peer->session ? mNetplaySessionFrame(peer->session) : 0;
}

uint32_t mNetplayPeerLocalInputFrame(const struct mNetplayPeer* peer) {
	return mNetplayPeerFrame(peer) + peer->delay;
}

uint32_t mNetplayPeerWaitingMask(const struct mNetplayPeer* peer) {
	if (!peer->session) {
		return 0;
	}
	return mNetplayInputMissing(&peer->input, mNetplaySessionFrame(peer->session));
}

int32_t mNetplayPeerFrameAdvantage(const struct mNetplayPeer* peer) {
	if (!peer->session) {
		return 0;
	}
	uint64_t now = _now(peer);
	int64_t local = mNetplaySessionFrame(peer->session);
	int64_t slowest = INT64_MAX;
	unsigned i;
	for (i = 0; i < mNETPLAY_MAX_PLAYERS; ++i) {
		const struct Remote* remote = &peer->remotes[i];
		if (!remote->link || (peer->host && !remote->welcomed)) {
			continue;
		}
		/* Where the other site probably is now: its last report, plus the
		 * frames it ran since it sent it. */
		uint64_t elapsedUs = (now - remote->frameAt + remote->rttMs / 2) * 1000;
		int64_t estimate = remote->frame + (int64_t) (elapsedUs / FRAME_US);
		if (estimate < slowest) {
			slowest = estimate;
		}
	}
	return slowest == INT64_MAX ? 0 : (int32_t) (local - slowest);
}

void mNetplayPeerGetStats(const struct mNetplayPeer* peer, struct mNetplayPeerStats* stats) {
	*stats = peer->stats;
	if (peer->stalled) {
		stats->stalledMs += _now(peer) - peer->stallStart;
	}
	stats->rttMs = 0;
	unsigned i;
	for (i = 0; i < mNETPLAY_MAX_PLAYERS; ++i) {
		if (peer->remotes[i].link && peer->remotes[i].rttMs > stats->rttMs) {
			stats->rttMs = peer->remotes[i].rttMs;
		}
	}
}

struct mNetplaySession* mNetplayPeerSession(struct mNetplayPeer* peer) {
	return peer->session;
}

#endif
