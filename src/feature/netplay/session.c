/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/feature/netplay.h>

#ifdef M_CORE_GBA

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/lockstep.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/serialize.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba-util/hash.h>
#include <mgba-util/memory.h>
#include <mgba-util/vfs.h>

#include <time.h>

mLOG_DECLARE_CATEGORY(NETPLAY);
mLOG_DEFINE_CATEGORY(NETPLAY, "Netplay", "netplay");

#define SNAPSHOT_MAGIC 0x53504E4D /* "MNPS" */
#define SNAPSHOT_VERSION 1
#define SNAPSHOT_FLAGS (SAVESTATE_SAVEDATA | SAVESTATE_RTC)

/* A secondary sleeps on reaching the clock owner's published cycle, at most
 * one lockstep interval away; running a whole frame past that without
 * sleeping means the link is broken, not merely busy. */
#define MAX_SECONDARY_FRAMES_PER_PASS 2

struct mNetplayPlayer {
	struct mLockstepUser user;
	struct mNetplaySession* session;
	unsigned slot;
	bool asleep;
	bool saveLoaded;
	struct mCore* core;
	/* The core's own runLoop, kept here because a frontend may replace the
	 * pointer on its core to drive the whole session from its thread. */
	void (*runLoop)(struct mCore*);
	struct GBASIOLockstepDriver driver;
};

struct mNetplaySession {
	struct mNetplaySessionSettings settings;
	struct GBASIOLockstepCoordinator coordinator;
	struct mNetplayPlayer players[mNETPLAY_MAX_PLAYERS];
	uint32_t frame;
	bool romLoaded;
	bool started;
	bool tearingDown;
	bool idMismatch;
};

struct mNetplaySnapshotHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t players;
	uint32_t frame;
	int64_t rtcEpochMs;
};

/* What a GBA savestate leaves out but emulation depends on. */
struct mNetplaySnapshotPlayer {
	uint32_t keysActive;
	uint32_t stateSize;
	int64_t rtcOffset;
	int64_t rtcLastLatch;
	/* SIOMLT_SEND/SIODATA8 is marked invalid in io.c's _isValidRegister,
	 * so savestates neither save nor restore it: a restored link player
	 * would transmit whatever word it held before the load. */
	uint16_t siomltSend;
	uint16_t reserved[3];
};

/* The GBA RTC turns its Unix time into calendar fields with localtime_r(),
 * and a save's RTC block back with mktime(), so the process time zone
 * reaches emulated state. Pin it to UTC while any session exists. */
static unsigned _tzPins;
static char* _savedTz;
static bool _hadTz;

static void _setTz(const char* value) {
#ifdef _WIN32
	/* _putenv copies its argument; "TZ=" removes the variable. */
	size_t length = strlen("TZ=") + (value ? strlen(value) : 0) + 1;
	char* assignment = malloc(length);
	if (assignment) {
		snprintf(assignment, length, "TZ=%s", value ? value : "");
		_putenv(assignment);
		free(assignment);
	}
	_tzset();
#else
	if (value) {
		setenv("TZ", value, 1);
	} else {
		unsetenv("TZ");
	}
	tzset();
#endif
}

static void _pinTimeZone(void) {
	if (_tzPins++) {
		return;
	}
	const char* tz = getenv("TZ");
	_hadTz = tz != NULL;
	_savedTz = tz ? strdup(tz) : NULL;
	_setTz("UTC0");
}

static void _unpinTimeZone(void) {
	if (!_tzPins || --_tzPins) {
		return;
	}
	_setTz(_hadTz ? _savedTz : NULL);
	free(_savedTz);
	_savedTz = NULL;
}

static void _playerSleep(struct mLockstepUser* user) {
	struct mNetplayPlayer* player = (struct mNetplayPlayer*) user;
	player->asleep = true;
}

static void _playerWake(struct mLockstepUser* user) {
	struct mNetplayPlayer* player = (struct mNetplayPlayer*) user;
	player->asleep = false;
}

static int _playerRequestedId(struct mLockstepUser* user) {
	struct mNetplayPlayer* player = (struct mNetplayPlayer*) user;
	return player->slot;
}

static void _playerIdChanged(struct mLockstepUser* user, int id) {
	struct mNetplayPlayer* player = (struct mNetplayPlayer*) user;
	/* Detaching at teardown renumbers whoever is left; that is expected. */
	if (id != (int) player->slot && !player->session->tearingDown) {
		mLOG(NETPLAY, ERROR, "Slot %u was assigned link ID %i", player->slot, id);
		player->session->idMismatch = true;
	}
}

static bool _initCore(struct mNetplaySession* session, struct mNetplayPlayer* player) {
	struct mCore* core = GBACoreCreate();
	if (!core) {
		return false;
	}
	if (!core->init(core)) {
		core->deinit(core);
		return false;
	}
	player->core = core;
	player->runLoop = core->runLoop;

	/* A fresh configuration, never the user's: nothing on disk may change
	 * how this core emulates. */
	mCoreInitConfig(core, NULL);
	struct mCoreConfig* config = &core->config;
	mCoreConfigSetOverrideIntValue(config, "useBios", 0);
	mCoreConfigSetOverrideIntValue(config, "skipBios", 1);
	mCoreConfigSetOverrideIntValue(config, "audioSync", 0);
	mCoreConfigSetOverrideIntValue(config, "videoSync", 0);
	mCoreConfigSetOverrideIntValue(config, "threadedVideo", 0);
	mCoreConfigSetOverrideIntValue(config, "hwaccelVideo", 0);
	mCoreConfigSetOverrideIntValue(config, "allowOpposingDirections", 0);
	mCoreConfigSetOverrideIntValue(config, "vbaBugCompat", 1);
	mCoreConfigSetOverrideIntValue(config, "gba.forceGbp", 0);
	mCoreConfigSetOverrideValue(config, "idleOptimization", "ignore");
	/* Audible, though only a frontend listens. Not pinned: volume only
	 * scales output samples, which the hash leaves out, so a frontend may
	 * set its player's own. */
	mCoreConfigSetIntValue(config, "volume", 0x100);
	mCoreConfigSetIntValue(config, "mute", 0);
	mCoreLoadForeignConfig(core, config);

	core->rtc.override = RTC_FAKE_EPOCH;
	core->rtc.value = session->settings.rtcEpochMs;

	player->user.sleep = _playerSleep;
	player->user.wake = _playerWake;
	player->user.requestedId = _playerRequestedId;
	player->user.playerIdChanged = _playerIdChanged;
	GBASIOLockstepDriverCreate(&player->driver, &player->user);
	return true;
}

struct mNetplaySession* mNetplaySessionCreate(const struct mNetplaySessionSettings* settings) {
	if (!settings || settings->players < 2 || settings->players > mNETPLAY_MAX_PLAYERS) {
		return NULL;
	}
	struct mNetplaySession* session = calloc(1, sizeof(*session));
	if (!session) {
		return NULL;
	}
	session->settings = *settings;
	_pinTimeZone();
	GBASIOLockstepCoordinatorInit(&session->coordinator);

	unsigned slot;
	for (slot = 0; slot < settings->players; ++slot) {
		struct mNetplayPlayer* player = &session->players[slot];
		player->session = session;
		player->slot = slot;
		if (!_initCore(session, player)) {
			mLOG(NETPLAY, ERROR, "Could not create core for slot %u", slot);
			mNetplaySessionDestroy(session);
			return NULL;
		}
	}
	return session;
}

static void _silentLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	UNUSED(category);
	UNUSED(level);
	UNUSED(format);
	UNUSED(args);
}

void mNetplaySessionDestroy(struct mNetplaySession* session) {
	if (!session) {
		return;
	}
	unsigned slot;
	session->tearingDown = true;
	/* Unlink every core before tearing any down: detaching wakes peers.
	 * The lockstep coordinator warns when its last player leaves, which
	 * is exactly what teardown means here, so keep that out of the log. */
	struct mLogger silent = { .log = _silentLog };
	struct mLogger* logger = mLogGetThreadLogger();
	mLogSetThreadLogger(&silent);
	for (slot = 0; slot < session->settings.players; ++slot) {
		struct mCore* core = session->players[slot].core;
		if (core && session->started) {
			core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, NULL);
		}
	}
	mLogSetThreadLogger(logger);
	for (slot = 0; slot < session->settings.players; ++slot) {
		struct mCore* core = session->players[slot].core;
		if (core) {
			mCoreConfigDeinit(&core->config);
			core->deinit(core);
		}
	}
	GBASIOLockstepCoordinatorDeinit(&session->coordinator);
	_unpinTimeZone();
	free(session);
}

bool mNetplaySessionLoadROM(struct mNetplaySession* session, const void* rom, size_t size) {
	if (session->started || session->romLoaded || !rom || !size) {
		return false;
	}
	unsigned slot;
	for (slot = 0; slot < session->settings.players; ++slot) {
		/* A private copy each: GPIO writes land in the ROM buffer. */
		struct VFile* vf = VFileMemChunk(rom, size);
		struct mCore* core = session->players[slot].core;
		if (!vf || !core->loadROM(core, vf)) {
			if (vf) {
				vf->close(vf);
			}
			mLOG(NETPLAY, ERROR, "Could not load ROM into slot %u", slot);
			return false;
		}
	}
	session->romLoaded = true;
	return true;
}

bool mNetplaySessionLoadSave(struct mNetplaySession* session, unsigned slot, const void* save, size_t size) {
	if (session->started || !session->romLoaded || slot >= session->settings.players) {
		return false;
	}
	struct mNetplayPlayer* player = &session->players[slot];
	if (player->saveLoaded) {
		return false;
	}
	/* In memory only: a session never writes battery files itself. */
	struct VFile* vf = VFileMemChunk(size ? save : NULL, size);
	if (!vf) {
		return false;
	}
	if (!player->core->loadSave(player->core, vf)) {
		vf->close(vf);
		return false;
	}
	player->saveLoaded = true;
	return true;
}

bool mNetplaySessionStart(struct mNetplaySession* session) {
	if (session->started || !session->romLoaded) {
		return false;
	}
	unsigned slot;
	for (slot = 0; slot < session->settings.players; ++slot) {
		if (!session->players[slot].saveLoaded && !mNetplaySessionLoadSave(session, slot, NULL, 0)) {
			return false;
		}
	}
	for (slot = 0; slot < session->settings.players; ++slot) {
		struct mCore* core = session->players[slot].core;
		core->reset(core);
	}
	/* Attaching in slot order hands out link IDs in slot order. */
	for (slot = 0; slot < session->settings.players; ++slot) {
		struct mNetplayPlayer* player = &session->players[slot];
		GBASIOLockstepCoordinatorAttach(&session->coordinator, &player->driver);
		player->core->setPeripheral(player->core, mPERIPH_GBA_LINK_PORT, &player->driver.d);
	}
	session->started = true;
	if (session->idMismatch) {
		mLOG(NETPLAY, ERROR, "Link IDs do not match slots");
		return false;
	}
	session->frame = 0;
	return true;
}

static int32_t _framesSince(const struct mCore* core, uint32_t since) {
	return (int32_t) (core->frameCounter(core) - since);
}

enum mNetplayStepResult mNetplaySessionRunFrame(struct mNetplaySession* session, const uint16_t* keys) {
	if (!session->started || session->idMismatch) {
		return mNETPLAY_STEP_NOT_STARTED;
	}
	unsigned players = session->settings.players;
	unsigned slot;
	for (slot = 0; slot < players; ++slot) {
		struct mCore* core = session->players[slot].core;
		core->setKeys(core, keys ? keys[slot] & mNETPLAY_KEY_MASK : 0);
	}

	struct mNetplayPlayer* primary = &session->players[0];
	struct mCore* clock = primary->core;
	uint32_t start = clock->frameCounter(clock);
	unsigned secondaryFrames[mNETPLAY_MAX_PLAYERS] = {0};
	while (_framesSince(clock, start) < 1) {
		bool progressed = false;
		/* One runLoop apiece, round-robin. Letting the clock owner run a
		 * whole frame before the others drain events fills lockstep's
		 * 8-slot queue (Pokémon polls SIO on the title screen; mashing
		 * buttons overflows it and crashes). Threaded multiplayer does
		 * not hit this because the cores actually run together. */
		if (!primary->asleep && _framesSince(clock, start) < 1) {
			primary->runLoop(clock);
			progressed = true;
		}
		for (slot = 1; slot < players; ++slot) {
			struct mNetplayPlayer* player = &session->players[slot];
			if (player->asleep) {
				continue;
			}
			struct mCore* core = player->core;
			uint32_t passStart = core->frameCounter(core);
			player->runLoop(core);
			progressed = true;
			secondaryFrames[slot] += (unsigned) _framesSince(core, passStart);
			if (secondaryFrames[slot] > MAX_SECONDARY_FRAMES_PER_PASS) {
				mLOG(NETPLAY, ERROR, "Slot %u never reached the link clock", slot);
				return mNETPLAY_STEP_DEADLOCK;
			}
		}
		if (!progressed) {
			mLOG(NETPLAY, ERROR, "Every core is waiting on another at frame %u", session->frame);
			return mNETPLAY_STEP_DEADLOCK;
		}
	}
	++session->frame;
	return mNETPLAY_STEP_OK;
}

unsigned mNetplaySessionPlayers(const struct mNetplaySession* session) {
	return session->settings.players;
}

uint32_t mNetplaySessionFrame(const struct mNetplaySession* session) {
	return session->frame;
}

struct mCore* mNetplaySessionGetCore(struct mNetplaySession* session, unsigned slot) {
	if (slot >= session->settings.players) {
		return NULL;
	}
	return session->players[slot].core;
}

uint32_t mNetplaySessionHashCore(struct mNetplaySession* session, unsigned slot) {
	if (slot >= session->settings.players) {
		return 0;
	}
	struct mCore* core = session->players[slot].core;
	size_t size = core->stateSize(core);
	/* Zeroed so the reserved fields the serializer skips hash the same. */
	void* state = calloc(1, size);
	if (!state) {
		return 0;
	}
	core->saveState(core, state);
	/* The last mixed output samples follow the frontend's volume and
	 * channel mutes, which are each player's own business. */
	struct GBASerializedState* serialized = state;
	memset(serialized->currentSamples, 0, sizeof(serialized->currentSamples));
	uint32_t hash = hash32(state, size, 0);
	free(state);

	/* Emulation state the savestate leaves out (see mNetplaySnapshotPlayer).
	 * Spelled out byte by byte: storing through a wider pointer into a
	 * byte array is an aliasing violation, and with LTO the stores were
	 * dropped, hashing whatever the stack held instead. */
	struct GBA* gba = core->board;
	uint16_t send = gba->memory.io[GBA_REG(SIOMLT_SEND)];
	uint16_t keys = gba->keysActive;
	const uint8_t extra[4] = { send & 0xFF, send >> 8, keys & 0xFF, keys >> 8 };
	hash = hash32(extra, sizeof(extra), hash);

	void* save = NULL;
	size_t saveSize = core->savedataClone(core, &save);
	if (save) {
		hash = hash32(save, saveSize, hash);
		free(save);
	}
	return hash;
}

size_t mNetplaySessionCopySave(struct mNetplaySession* session, unsigned slot, void** save) {
	*save = NULL;
	if (slot >= session->settings.players) {
		return 0;
	}
	struct mCore* core = session->players[slot].core;
	return core->savedataClone(core, save);
}

/* Loads one core's state from a snapshot so that the result depends only
 * on the snapshot, never on what the core was doing before.
 *
 * mCoreLoadStateNamed() is not enough for that. GBAIODeserialize() ends in
 * GBASIOWriteRCNT(), which, when the SIO mode changes (a fresh session has
 * not entered Multiplayer mode yet), calls into the lockstep driver before
 * the driver's own state is restored: it enqueues a mode-change event to
 * the other players, recomputes the ready bits from stale peer modes, and
 * can put player 0 to sleep. It also keeps RCNT's low bits from the old
 * state, and an idle SIO completion event keeps its old timestamp. So load
 * the core with the link driver unhooked, put SIOCNT/RCNT back exactly as
 * saved (both are serialized through GBAIORead(), i.e. live values), and
 * then let the driver restore itself from the extdata. */
static bool _loadCoreState(struct mCore* core, const void* data, size_t size) {
	struct VFile* vf = VFileFromConstMemory(data, size);
	if (!vf) {
		return false;
	}
	struct mStateExtdata extdata;
	mStateExtdataInit(&extdata);
	void* state = mCoreExtractState(core, vf, &extdata);
	vf->close(vf);
	if (!state) {
		mStateExtdataDeinit(&extdata);
		return false;
	}

	struct GBA* gba = core->board;
	const struct GBASerializedState* serialized = state;
	struct GBASIODriver* driver = gba->sio.driver;
	gba->sio.driver = NULL;
	bool ok = core->loadState(core, state);
	gba->sio.driver = driver;
	if (ok) {
		uint16_t siocnt, rcnt;
		int32_t when;
		LOAD_16LE(siocnt, GBA_REG_SIOCNT, serialized->io);
		LOAD_16LE(rcnt, GBA_REG_RCNT, serialized->io);
		gba->sio.siocnt = siocnt;
		gba->sio.rcnt = rcnt;
		gba->memory.io[GBA_REG(SIOCNT)] = siocnt;
		gba->memory.io[GBA_REG(RCNT)] = rcnt;
		LOAD_32LE(when, 0, &serialized->hw.sioNextEvent);
		if (!mTimingIsScheduled(&gba->timing, &gba->sio.completeEvent)) {
			gba->sio.completeEvent.when = when + mTimingCurrentTime(&gba->timing);
		}
	}
	mappedMemoryFree(state, core->stateSize(core));

	if (ok) {
		ok = core->loadExtraState(core, &extdata);
	}
	struct mStateExtdataItem item;
	if (ok && mStateExtdataGet(&extdata, EXTDATA_SAVEDATA, &item) && item.data) {
		ok = core->savedataRestore(core, item.data, item.size, true);
	}
	if (ok && mStateExtdataGet(&extdata, EXTDATA_RTC, &item) && core->rtc.d.deserialize) {
		ok = core->rtc.d.deserialize(&core->rtc.d, &item);
	}
	mStateExtdataDeinit(&extdata);
	return ok;
}

static bool _writeAll(struct VFile* vf, const void* data, size_t size) {
	return vf->write(vf, data, size) == (ssize_t) size;
}

static bool _readAll(struct VFile* vf, void* data, size_t size) {
	return vf->read(vf, data, size) == (ssize_t) size;
}

bool mNetplaySessionSaveSnapshot(struct mNetplaySession* session, struct VFile* vf) {
	if (!session->started) {
		return false;
	}
	struct mNetplaySnapshotHeader header;
	STORE_32LE(SNAPSHOT_MAGIC, 0, &header.magic);
	STORE_32LE(SNAPSHOT_VERSION, 0, &header.version);
	STORE_32LE(session->settings.players, 0, &header.players);
	STORE_32LE(session->frame, 0, &header.frame);
	STORE_64LE(session->settings.rtcEpochMs, 0, &header.rtcEpochMs);
	if (!_writeAll(vf, &header, sizeof(header))) {
		return false;
	}

	unsigned slot;
	for (slot = 0; slot < session->settings.players; ++slot) {
		struct mCore* core = session->players[slot].core;
		struct GBA* gba = core->board;
		struct VFile* state = VFileMemChunk(NULL, 0);
		if (!state) {
			return false;
		}
		if (!mCoreSaveStateNamed(core, state, SNAPSHOT_FLAGS)) {
			state->close(state);
			return false;
		}
		size_t stateSize = state->size(state);
		struct mNetplaySnapshotPlayer info;
		memset(&info, 0, sizeof(info));
		STORE_32LE(gba->keysActive, 0, &info.keysActive);
		STORE_16LE(gba->memory.io[GBA_REG(SIOMLT_SEND)], 0, &info.siomltSend);
		STORE_32LE(stateSize, 0, &info.stateSize);
		STORE_64LE(gba->memory.hw.rtc.offset, 0, &info.rtcOffset);
		STORE_64LE(gba->memory.hw.rtc.lastLatch, 0, &info.rtcLastLatch);
		void* data = state->map(state, stateSize, MAP_READ);
		bool ok = data && _writeAll(vf, &info, sizeof(info)) && _writeAll(vf, data, stateSize);
		if (data) {
			state->unmap(state, data, stateSize);
		}
		state->close(state);
		if (!ok) {
			return false;
		}
	}
	return true;
}

bool mNetplaySessionLoadSnapshot(struct mNetplaySession* session, struct VFile* vf) {
	if (!session->started) {
		return false;
	}
	struct mNetplaySnapshotHeader header;
	if (!_readAll(vf, &header, sizeof(header))) {
		return false;
	}
	uint32_t magic, version, players, frame;
	int64_t rtcEpochMs;
	LOAD_32LE(magic, 0, &header.magic);
	LOAD_32LE(version, 0, &header.version);
	LOAD_32LE(players, 0, &header.players);
	LOAD_32LE(frame, 0, &header.frame);
	LOAD_64LE(rtcEpochMs, 0, &header.rtcEpochMs);
	if (magic != SNAPSHOT_MAGIC || version != SNAPSHOT_VERSION || players != session->settings.players ||
	    rtcEpochMs != session->settings.rtcEpochMs) {
		mLOG(NETPLAY, ERROR, "Snapshot does not match this session");
		return false;
	}

	/* Slot 0 first: its driver state carries the coordinator's. */
	unsigned slot;
	for (slot = 0; slot < players; ++slot) {
		struct mCore* core = session->players[slot].core;
		struct GBA* gba = core->board;
		struct mNetplaySnapshotPlayer info;
		if (!_readAll(vf, &info, sizeof(info))) {
			return false;
		}
		uint32_t keysActive, stateSize;
		int64_t rtcOffset, rtcLastLatch;
		uint16_t siomltSend;
		LOAD_32LE(keysActive, 0, &info.keysActive);
		LOAD_16LE(siomltSend, 0, &info.siomltSend);
		LOAD_32LE(stateSize, 0, &info.stateSize);
		LOAD_64LE(rtcOffset, 0, &info.rtcOffset);
		LOAD_64LE(rtcLastLatch, 0, &info.rtcLastLatch);

		void* data = malloc(stateSize);
		if (!data || !_readAll(vf, data, stateSize)) {
			free(data);
			return false;
		}
		bool ok = _loadCoreState(core, data, stateSize);
		free(data);
		if (!ok) {
			mLOG(NETPLAY, ERROR, "Could not load snapshot state for slot %u", slot);
			return false;
		}
		gba->keysActive = keysActive;
		gba->memory.io[GBA_REG(SIOMLT_SEND)] = siomltSend;
		gba->memory.hw.rtc.offset = rtcOffset;
		gba->memory.hw.rtc.lastLatch = rtcLastLatch;
	}
	session->frame = frame;
	return true;
}

#endif
