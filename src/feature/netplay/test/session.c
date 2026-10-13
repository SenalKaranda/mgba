/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba/feature/netplay.h>
#include <mgba/internal/gba/audio.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/image.h>
#include <mgba-util/vfs.h>

#include "linktest-rom.h"

/* Mirrors struct LinkTestResult in rom/linktest.c. */
#define RESULT_BASE 0x02000000
#define RESULT_MAGIC_VALUE 0x544C504E
enum {
	RESULT_MAGIC = 0x00,
	RESULT_FRAMES = 0x04,
	RESULT_TRANSFERS = 0x08,
	RESULT_HASH = 0x0C,
	RESULT_LAG_PARENT = 0x10,
	RESULT_LAG_CHILD = 0x14,
	RESULT_BURSTS = 0x18,
	RESULT_ERRORS = 0x1C,
	RESULT_PLAYER_ID = 0x20,
};

#define BURST_LENGTH 9
#define RTC_EPOCH_MS 1767225600000LL /* 2026-01-01T00:00:00Z */

/* Counts warnings and worse; prints them, drops everything chattier. The
 * lockstep driver's "did not receive data" fallback is a warning, so a
 * clean count also proves no transfer fell back to 0xFFFF. */
static unsigned _warnings;

static void _countingLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	if (!(level & (mLOG_FATAL | mLOG_ERROR | mLOG_WARN))) {
		return;
	}
	++_warnings;
	fprintf(stderr, "[%s] ", mLogCategoryName(category));
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
}

static struct mLogger _logger = { .log = _countingLog };

M_TEST_SUITE_SETUP(NetplaySession) {
	mLogSetDefaultLogger(&_logger);
	return 0;
}

struct Run {
	unsigned frames;
	unsigned hashEvery;
	uint32_t seed;
	uint32_t* hashes;
	size_t nHashes;
};

static uint32_t _result(struct mNetplaySession* session, unsigned slot, uint32_t field) {
	struct mCore* core = mNetplaySessionGetCore(session, slot);
	return core->busRead32(core, RESULT_BASE + field);
}

static struct mNetplaySession* _startSession(unsigned players) {
	_warnings = 0;
	struct mNetplaySessionSettings settings = {
		.players = players,
		.rtcEpochMs = RTC_EPOCH_MS,
	};
	struct mNetplaySession* session = mNetplaySessionCreate(&settings);
	assert_non_null(session);
	assert_true(mNetplaySessionLoadROM(session, linktestRom, sizeof(linktestRom)));
	assert_true(mNetplaySessionStart(session));
	return session;
}

static uint16_t _keysFor(uint32_t seed, unsigned slot, uint32_t frame) {
	/* Held for 8 frames at a time, like a person pressing buttons. */
	uint32_t x = seed ^ (slot * 0x9E3779B9u) ^ ((frame >> 3) * 0x85EBCA6Bu);
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	return x & mNETPLAY_KEY_MASK;
}

static void _runFrames(struct mNetplaySession* session, struct Run* run) {
	unsigned players = mNetplaySessionPlayers(session);
	unsigned i;
	for (i = 0; i < run->frames; ++i) {
		uint16_t keys[mNETPLAY_MAX_PLAYERS] = {0};
		uint32_t frame = mNetplaySessionFrame(session);
		unsigned slot;
		for (slot = 0; slot < players; ++slot) {
			keys[slot] = _keysFor(run->seed, slot, frame);
		}
		assert_int_equal(mNetplaySessionRunFrame(session, keys), mNETPLAY_STEP_OK);
		if (run->hashes && run->hashEvery && (mNetplaySessionFrame(session) % run->hashEvery) == 0) {
			for (slot = 0; slot < players; ++slot) {
				run->hashes[run->nHashes++] = mNetplaySessionHashCore(session, slot);
			}
		}
	}
}

static void _assertLinked(struct mNetplaySession* session, unsigned frames) {
	unsigned players = mNetplaySessionPlayers(session);
	assert_int_equal(_warnings, 0);
	uint32_t hash = _result(session, 0, RESULT_HASH);
	unsigned slot;
	for (slot = 0; slot < players; ++slot) {
		assert_int_equal(_result(session, slot, RESULT_MAGIC), RESULT_MAGIC_VALUE);
		/* Every console folds the same SIOMULTI words, so equal hashes
		 * mean every transfer delivered the same data everywhere. */
		assert_int_equal(_result(session, slot, RESULT_HASH), hash);
		assert_int_equal(_result(session, slot, RESULT_PLAYER_ID), slot);
		assert_int_equal(_result(session, slot, RESULT_LAG_PARENT), 0);
		assert_int_equal(_result(session, slot, RESULT_LAG_CHILD), 0);
		assert_int_equal(_result(session, slot, RESULT_ERRORS), 0);
		/* A full burst every frame, allowing a few frames to link up. */
		assert_true(_result(session, slot, RESULT_TRANSFERS) >= BURST_LENGTH * (frames - 8));
	}
}

M_TEST_DEFINE(rejectsBadPlayerCounts) {
	struct mNetplaySessionSettings settings = { .rtcEpochMs = RTC_EPOCH_MS };
	settings.players = 1;
	assert_null(mNetplaySessionCreate(&settings));
	settings.players = mNETPLAY_MAX_PLAYERS + 1;
	assert_null(mNetplaySessionCreate(&settings));
}

M_TEST_DEFINE(pinsHostState) {
	struct mNetplaySession* session = _startSession(2);
	assert_string_equal(getenv("TZ"), "UTC0");
	unsigned slot;
	for (slot = 0; slot < 2; ++slot) {
		struct mCore* core = mNetplaySessionGetCore(session, slot);
		assert_int_equal(core->rtc.override, RTC_FAKE_EPOCH);
		assert_true(core->rtc.value == RTC_EPOCH_MS);
	}
	assert_int_equal(mNetplaySessionRunFrame(session, NULL), mNETPLAY_STEP_OK);
	mNetplaySessionDestroy(session);
}

M_TEST_DEFINE(linkTwoPlayers) {
	struct mNetplaySession* session = _startSession(2);
	struct Run run = { .frames = 600, .seed = 1 };
	_runFrames(session, &run);
	assert_int_equal(mNetplaySessionFrame(session), 600);
	_assertLinked(session, 600);
	mNetplaySessionDestroy(session);
}

M_TEST_DEFINE(linkFourPlayers) {
	struct mNetplaySession* session = _startSession(4);
	struct Run run = { .frames = 600, .seed = 2 };
	_runFrames(session, &run);
	_assertLinked(session, 600);
	mNetplaySessionDestroy(session);
}

static void _hashRun(unsigned players, uint32_t seed, unsigned frames, uint32_t* hashes, size_t* nHashes) {
	struct mNetplaySession* session = _startSession(players);
	struct Run run = { .frames = frames, .hashEvery = 30, .seed = seed, .hashes = hashes };
	_runFrames(session, &run);
	*nHashes = run.nHashes;
	mNetplaySessionDestroy(session);
}

M_TEST_DEFINE(repeatRunsMatch) {
	enum { FRAMES = 900, PLAYERS = 4, MAX = FRAMES / 30 * PLAYERS };
	uint32_t a[MAX], b[MAX];
	size_t nA, nB;
	_hashRun(PLAYERS, 7, FRAMES, a, &nA);
	_hashRun(PLAYERS, 7, FRAMES, b, &nB);
	assert_int_equal(nA, MAX);
	assert_int_equal(nB, MAX);
	assert_memory_equal(a, b, sizeof(a));
}

M_TEST_DEFINE(inputsReachTheLink) {
	enum { FRAMES = 300, PLAYERS = 2, MAX = FRAMES / 30 * PLAYERS };
	uint32_t a[MAX], b[MAX];
	size_t nA, nB;
	_hashRun(PLAYERS, 11, FRAMES, a, &nA);
	_hashRun(PLAYERS, 12, FRAMES, b, &nB);
	assert_memory_not_equal(a, b, sizeof(a));

	struct mNetplaySession* first = _startSession(PLAYERS);
	struct mNetplaySession* second = _startSession(PLAYERS);
	struct Run run = { .frames = FRAMES, .seed = 11 };
	_runFrames(first, &run);
	run.seed = 12;
	_runFrames(second, &run);
	/* Different keys changed the words on the wire, not just KEYINPUT. */
	assert_int_not_equal(_result(first, 0, RESULT_HASH), _result(second, 0, RESULT_HASH));
	mNetplaySessionDestroy(first);
	mNetplaySessionDestroy(second);
}

M_TEST_DEFINE(renderingDoesNotChangeState) {
	/* In the GUI only the local player's core renders, and which one
	 * that is differs per site; drawing must not touch emulated state. */
	enum { FRAMES = 300, PLAYERS = 3, MAX = FRAMES / 30 * PLAYERS };
	uint32_t headless[MAX], drawn[MAX];
	size_t n;
	_hashRun(PLAYERS, 31, FRAMES, headless, &n);

	struct mNetplaySessionSettings settings = { .players = PLAYERS, .rtcEpochMs = RTC_EPOCH_MS };
	struct mNetplaySession* session = mNetplaySessionCreate(&settings);
	assert_non_null(session);
	assert_true(mNetplaySessionLoadROM(session, linktestRom, sizeof(linktestRom)));
	static mColor buffer[240 * 160];
	struct mCore* core = mNetplaySessionGetCore(session, 1);
	core->setVideoBuffer(core, buffer, 240);
	_warnings = 0;
	assert_true(mNetplaySessionStart(session));
	struct Run run = { .frames = FRAMES, .hashEvery = 30, .seed = 31, .hashes = drawn };
	_runFrames(session, &run);
	assert_memory_equal(headless, drawn, sizeof(headless));
	mNetplaySessionDestroy(session);
}

/* The test ROM is silent; start a square wave on every console the same
 * way, so there are samples for the volume to scale. */
static void _playTone(struct mNetplaySession* session) {
	unsigned slot;
	for (slot = 0; slot < mNetplaySessionPlayers(session); ++slot) {
		struct mCore* core = mNetplaySessionGetCore(session, slot);
		core->busWrite16(core, 0x04000084, 0x0080); /* SOUNDCNT_X: sound on */
		core->busWrite16(core, 0x04000080, 0x1177); /* SOUNDCNT_L: tone 1, both sides */
		core->busWrite16(core, 0x04000082, 0x0002); /* SOUNDCNT_H: PSG at 100% */
		core->busWrite16(core, 0x04000062, 0xF080); /* SOUND1CNT_H: loud, 50% duty */
		core->busWrite16(core, 0x04000064, 0x8600); /* SOUND1CNT_X: start */
	}
}

M_TEST_DEFINE(outputSettingsDoNotChangeState) {
	/* Each player sets their own volume and may mute channels; that
	 * changes the mixed samples but must never look like a desync. */
	enum { FRAMES = 300, PLAYERS = 3, MAX = FRAMES / 30 * PLAYERS };
	uint32_t plain[MAX], adjusted[MAX];
	struct mNetplaySession* reference = _startSession(PLAYERS);
	_playTone(reference);
	struct Run plainRun = { .frames = FRAMES, .hashEvery = 30, .seed = 41, .hashes = plain };
	_runFrames(reference, &plainRun);
	mNetplaySessionDestroy(reference);

	struct mNetplaySession* session = _startSession(PLAYERS);
	_playTone(session);
	struct GBA* gba = mNetplaySessionGetCore(session, 0)->board;
	/* Audible, so a frontend's local player hears the game. */
	assert_int_equal(gba->audio.masterVolume, GBA_AUDIO_VOLUME_MAX);
	struct Run run = { .frames = FRAMES / 2, .hashEvery = 30, .seed = 41, .hashes = adjusted };
	_runFrames(session, &run);

	struct mCore* core = mNetplaySessionGetCore(session, 0);
	core->opts.volume = 0x40;
	core->reloadConfigOption(core, NULL, NULL);
	core = mNetplaySessionGetCore(session, 1);
	core->opts.mute = true;
	core->reloadConfigOption(core, NULL, NULL);
	core = mNetplaySessionGetCore(session, 2);
	core->enableAudioChannel(core, 0, false);

	_runFrames(session, &run);
	assert_int_equal(run.nHashes, MAX);
	assert_memory_equal(plain, adjusted, sizeof(plain));
	mNetplaySessionDestroy(session);
}

M_TEST_DEFINE(snapshotRoundTrip) {
	enum { BEFORE = 240, AFTER = 300, PLAYERS = 3, MAX = AFTER / 30 * PLAYERS };
	struct mNetplaySession* session = _startSession(PLAYERS);
	struct Run run = { .frames = BEFORE, .seed = 5 };
	_runFrames(session, &run);

	struct VFile* snapshot = VFileMemChunk(NULL, 0);
	assert_non_null(snapshot);
	assert_true(mNetplaySessionSaveSnapshot(session, snapshot));

	uint32_t straight[MAX], restored[MAX], elsewhere[MAX];
	run.frames = AFTER;
	run.hashEvery = 30;
	run.hashes = straight;
	run.nHashes = 0;
	_runFrames(session, &run);
	assert_int_equal(run.nHashes, MAX);

	/* Rewind the same session to the snapshot and replay the same keys. */
	snapshot->seek(snapshot, 0, SEEK_SET);
	assert_true(mNetplaySessionLoadSnapshot(session, snapshot));
	assert_int_equal(mNetplaySessionFrame(session), BEFORE);
	run.hashes = restored;
	run.nHashes = 0;
	_runFrames(session, &run);
	assert_memory_equal(straight, restored, sizeof(straight));
	_assertLinked(session, BEFORE + AFTER);
	mNetplaySessionDestroy(session);

	/* A different session, as a late joiner would load it. */
	struct mNetplaySession* joiner = _startSession(PLAYERS);
	snapshot->seek(snapshot, 0, SEEK_SET);
	assert_true(mNetplaySessionLoadSnapshot(joiner, snapshot));
	run.hashes = elsewhere;
	run.nHashes = 0;
	_runFrames(joiner, &run);
	assert_memory_equal(straight, elsewhere, sizeof(straight));
	mNetplaySessionDestroy(joiner);
	snapshot->close(snapshot);
}

M_TEST_DEFINE(snapshotAnyFrame) {
	/* Resyncs happen whenever a desync is noticed, including while the
	 * link is still coming up, so a joiner loading a snapshot taken at
	 * any frame must continue exactly like the session it came from. */
	static const unsigned at[] = { 1, 2, 3, 17, 64, 199, 333 };
	enum { AFTER = 60, PLAYERS = 4, MAX = AFTER / 10 * PLAYERS };
	size_t i;
	for (i = 0; i < sizeof(at) / sizeof(*at); ++i) {
		struct mNetplaySession* host = _startSession(PLAYERS);
		struct Run run = { .frames = at[i], .seed = 23 };
		_runFrames(host, &run);
		struct VFile* snapshot = VFileMemChunk(NULL, 0);
		assert_true(mNetplaySessionSaveSnapshot(host, snapshot));

		uint32_t expected[MAX], actual[MAX];
		run.frames = AFTER;
		run.hashEvery = 10;
		run.hashes = expected;
		run.nHashes = 0;
		_runFrames(host, &run);

		struct mNetplaySession* joiner = _startSession(PLAYERS);
		snapshot->seek(snapshot, 0, SEEK_SET);
		assert_true(mNetplaySessionLoadSnapshot(joiner, snapshot));
		run.hashes = actual;
		run.nHashes = 0;
		_runFrames(joiner, &run);
		assert_memory_equal(expected, actual, sizeof(expected));

		snapshot->close(snapshot);
		mNetplaySessionDestroy(host);
		mNetplaySessionDestroy(joiner);
	}
}

M_TEST_DEFINE(snapshotRejectsMismatch) {
	struct mNetplaySession* two = _startSession(2);
	struct mNetplaySession* three = _startSession(3);
	struct VFile* snapshot = VFileMemChunk(NULL, 0);
	assert_true(mNetplaySessionSaveSnapshot(two, snapshot));
	snapshot->seek(snapshot, 0, SEEK_SET);
	assert_false(mNetplaySessionLoadSnapshot(three, snapshot));
	snapshot->close(snapshot);
	mNetplaySessionDestroy(two);
	mNetplaySessionDestroy(three);
}

M_TEST_SUITE_DEFINE_SETUP(NetplaySession,
	cmocka_unit_test(rejectsBadPlayerCounts),
	cmocka_unit_test(pinsHostState),
	cmocka_unit_test(linkTwoPlayers),
	cmocka_unit_test(linkFourPlayers),
	cmocka_unit_test(repeatRunsMatch),
	cmocka_unit_test(inputsReachTheLink),
	cmocka_unit_test(renderingDoesNotChangeState),
	cmocka_unit_test(outputSettingsDoNotChangeState),
	cmocka_unit_test(snapshotRoundTrip),
	cmocka_unit_test(snapshotAnyFrame),
	cmocka_unit_test(snapshotRejectsMismatch))
