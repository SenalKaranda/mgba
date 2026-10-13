/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/feature/netplay-net.h>
#include <mgba-util/vfs.h>

#include "linktest-rom.h"

#define RTC_EPOCH_MS 1767225600000LL
#define FRAME_US 16743
/* Generous: a test that has not finished by then is stuck. */
#define MAX_VIRTUAL_MS (10 * 60 * 1000)

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

M_TEST_SUITE_SETUP(NetplayPeer) {
	mLogSetDefaultLogger(&_logger);
	return 0;
}

static uint16_t _keysFor(uint32_t seed, unsigned slot, uint32_t frame) {
	uint32_t x = seed ^ (slot * 0x9E3779B9u) ^ ((frame >> 3) * 0x85EBCA6Bu);
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	return x & mNETPLAY_KEY_MASK;
}

/* A real clock can tick between two reads in the same poll, while the
 * virtual network's stands still; this one runs a millisecond ahead on every
 * third read, never going backwards. */
struct TickingClock {
	struct mNetplayClock d;
	struct mNetplayClock* source;
	uint64_t last;
	unsigned reads;
};

static uint64_t _tickingNow(struct mNetplayClock* base) {
	struct TickingClock* clock = (struct TickingClock*) base;
	uint64_t now = clock->source->now(clock->source);
	if (clock->last < now) {
		clock->last = now;
	}
	if (++clock->reads % 3 == 0 && clock->last == now) {
		clock->last = now + 1;
	}
	return clock->last;
}

static struct mNetplayClock* _tickingClock(struct TickingClock* clock, struct mNetplaySimNetwork* net) {
	clock->d.now = _tickingNow;
	clock->source = mNetplaySimNetworkClock(net);
	clock->last = 0;
	clock->reads = 0;
	return &clock->d;
}

struct Site {
	struct mNetplayPeer* peer;
	bool started;
	uint64_t nextFrameUs;
	/* Neither polls nor runs, like a PC that hiccups. */
	bool frozen;
};

struct Game {
	struct mNetplaySimNetwork* net;
	struct TickingClock clock;
	struct Site sites[mNETPLAY_MAX_PLAYERS];
	struct mNetplayLink* clientLinks[mNETPLAY_MAX_PLAYERS];
	unsigned players;
	uint32_t seed;
	uint32_t endFrame;
	/* Called once per virtual millisecond; may disturb the game. */
	void (*hook)(struct Game*, uint64_t ms);
	void* hookData;
	unsigned prepared;
	unsigned preparedLocal;
};

/* Like a GUI: every site draws its own player's screen, which must not
 * change the game (the sites are compared with a headless session). */
static void _prepareCore(void* context, struct mCore* core, unsigned slot, bool local) {
	static mColor screens[mNETPLAY_MAX_PLAYERS][240 * 160];
	struct Game* game = context;
	++game->prepared;
	if (local) {
		++game->preparedLocal;
		core->setVideoBuffer(core, screens[slot], 240);
	}
}

static struct mNetplayPeerConfig _config(unsigned players, uint32_t endFrame) {
	struct mNetplayPeerConfig config = {
		.rom = linktestRom,
		.romSize = sizeof(linktestRom),
		.players = players,
		.rtcEpochMs = RTC_EPOCH_MS,
		.endFrame = endFrame,
	};
	return config;
}

static void _setup(struct Game* game, unsigned players, uint32_t endFrame, const struct mNetplaySimLinkParams* params, uint32_t seed) {
	memset(game, 0, sizeof(*game));
	_warnings = 0;
	game->net = mNetplaySimNetworkCreate(seed);
	game->players = players;
	game->seed = seed;
	game->endFrame = endFrame;
	struct mNetplayClock* clock = _tickingClock(&game->clock, game->net);
	struct mNetplayListener* listener = mNetplaySimListen(game->net);
	struct mNetplayPeerConfig config = _config(players, endFrame);
	config.prepareCore = _prepareCore;
	config.context = game;
	game->sites[0].peer = mNetplayPeerCreateHost(&config, listener, clock);
	assert_non_null(game->sites[0].peer);
	unsigned i;
	for (i = 1; i < players; ++i) {
		game->clientLinks[i] = mNetplaySimConnect(game->net, listener, params);
		assert_non_null(game->clientLinks[i]);
		game->sites[i].peer = mNetplayPeerCreateClient(&config, game->clientLinks[i], clock);
		assert_non_null(game->sites[i].peer);
	}
}

static void _teardown(struct Game* game) {
	unsigned i;
	for (i = 0; i < game->players; ++i) {
		mNetplayPeerDestroy(game->sites[i].peer);
	}
	mNetplaySimNetworkDestroy(game->net);
}

static bool _settled(enum mNetplayPeerState state) {
	return state == mNETPLAY_PEER_ENDED || state == mNETPLAY_PEER_FAILED;
}

/* Every site polls each virtual millisecond and runs frames at 60 Hz of
 * virtual time, catching up after stalls, until every site is done. */
static void _play(struct Game* game) {
	uint64_t ms;
	for (ms = 0; ms < MAX_VIRTUAL_MS; ++ms) {
		if (game->hook) {
			game->hook(game, ms);
		}
		bool allDone = true;
		unsigned i;
		for (i = 0; i < game->players; ++i) {
			struct Site* site = &game->sites[i];
			if (!site->peer) {
				continue;
			}
			if (site->frozen) {
				allDone = false;
				continue;
			}
			mNetplayPeerPoll(site->peer);
			enum mNetplayPeerState state = mNetplayPeerGetState(site->peer);
			if (state == mNETPLAY_PEER_RUNNING) {
				if (!site->started) {
					site->started = true;
					site->nextFrameUs = ms * 1000;
				}
				while (site->nextFrameUs <= ms * 1000) {
					unsigned slot = mNetplayPeerLocalSlot(site->peer);
					uint16_t keys = _keysFor(game->seed, slot, mNetplayPeerLocalInputFrame(site->peer));
					if (mNetplayPeerRunFrame(site->peer, keys) != mNETPLAY_PEER_STEP_OK) {
						break;
					}
					site->nextFrameUs += FRAME_US;
				}
			}
			if (!_settled(mNetplayPeerGetState(site->peer))) {
				allDone = false;
			}
		}
		if (allDone) {
			return;
		}
		mNetplaySimNetworkAdvance(game->net, 1);
	}
	fail_msg("The game never finished");
}

/* What every site should have computed: one local session fed the keys
 * each player scheduled, neutral for the first delay frames. */
static void _referenceHashes(unsigned players, uint32_t seed, unsigned delay, uint32_t frames, uint32_t* hashes) {
	struct mNetplaySessionSettings settings = { .players = players, .rtcEpochMs = RTC_EPOCH_MS };
	struct mNetplaySession* session = mNetplaySessionCreate(&settings);
	assert_non_null(session);
	assert_true(mNetplaySessionLoadROM(session, linktestRom, sizeof(linktestRom)));
	assert_true(mNetplaySessionStart(session));
	uint32_t frame;
	for (frame = 0; frame < frames; ++frame) {
		uint16_t keys[mNETPLAY_MAX_PLAYERS] = {0};
		unsigned slot;
		for (slot = 0; slot < players; ++slot) {
			keys[slot] = frame < delay ? 0 : _keysFor(seed, slot, frame);
		}
		assert_int_equal(mNetplaySessionRunFrame(session, keys), mNETPLAY_STEP_OK);
	}
	unsigned slot;
	for (slot = 0; slot < players; ++slot) {
		hashes[slot] = mNetplaySessionHashCore(session, slot);
	}
	mNetplaySessionDestroy(session);
}

static void _assertAllMatch(struct Game* game) {
	unsigned delay = mNetplayPeerInputDelay(game->sites[0].peer);
	uint32_t expected[mNETPLAY_MAX_PLAYERS];
	_referenceHashes(game->players, game->seed, delay, game->endFrame, expected);
	unsigned i;
	for (i = 0; i < game->players; ++i) {
		struct mNetplayPeer* peer = game->sites[i].peer;
		assert_int_equal(mNetplayPeerGetState(peer), mNETPLAY_PEER_ENDED);
		assert_int_equal(mNetplayPeerFrame(peer), game->endFrame);
		assert_int_equal(mNetplayPeerInputDelay(peer), delay);
		struct mNetplaySession* session = mNetplayPeerSession(peer);
		unsigned slot;
		for (slot = 0; slot < game->players; ++slot) {
			assert_int_equal(mNetplaySessionHashCore(session, slot), expected[slot]);
		}
	}
}

static void _playMatch(unsigned players, uint32_t frames, unsigned latency, unsigned jitter, unsigned loss, unsigned minDelay, unsigned maxDelay) {
	struct mNetplaySimLinkParams params = { .latencyMs = latency, .jitterMs = jitter, .lossPermille = loss };
	struct Game game;
	_setup(&game, players, frames, &params, 0xC0FFEE + players + latency);
	_play(&game);
	_assertAllMatch(&game);
	assert_int_equal(game.prepared, players * players);
	assert_int_equal(game.preparedLocal, players);
	unsigned delay = mNetplayPeerInputDelay(game.sites[0].peer);
	assert_true(delay >= minDelay && delay <= maxDelay);
	struct mNetplayPeerStats stats;
	unsigned i;
	for (i = 0; i < players; ++i) {
		mNetplayPeerGetStats(game.sites[i].peer, &stats);
		assert_int_equal(stats.desyncs, 0);
		if (getenv("NETPLAY_TEST_VERBOSE")) {
			fprintf(stderr, "  site %u: delay %u, rtt %u ms, %u stalls, %u ms stalled over %u frames\n", i,
			        delay, stats.rttMs, stats.stalls, (unsigned) stats.stalledMs, frames);
		}
	}
	assert_int_equal(_warnings, 0);
	_teardown(&game);
}

M_TEST_DEFINE(twoPlayersLan) {
	_playMatch(2, 600, 0, 0, 0, 2, 2);
}

M_TEST_DEFINE(fourPlayersInternet) {
	/* 50 ms round trips with jitter and 2% of datagrams lost. */
	_playMatch(4, 1800, 25, 6, 20, 4, 7);
}

M_TEST_DEFINE(fourPlayersFarAway) {
	/* 150 ms round trips. */
	_playMatch(4, 1200, 75, 10, 20, 10, 12);
}

struct Corruption {
	unsigned site;
	uint64_t atMs;
	bool done;
};

static void _corrupt(struct Game* game, uint64_t ms) {
	struct Corruption* corruption = game->hookData;
	if (corruption->done || ms < corruption->atMs) {
		return;
	}
	struct mNetplaySession* session = mNetplayPeerSession(game->sites[corruption->site].peer);
	if (!session) {
		return;
	}
	/* Flip the link hash the test ROM keeps in EWRAM on one site only. */
	struct mCore* core = mNetplaySessionGetCore(session, 0);
	core->rawWrite32(core, 0x0200000C, -1, core->rawRead32(core, 0x0200000C, -1) ^ 0x5A5A5A5A);
	corruption->done = true;
}

M_TEST_DEFINE(desyncIsRepaired) {
	struct mNetplaySimLinkParams params = { .latencyMs = 10, .jitterMs = 2 };
	struct Game game;
	_setup(&game, 3, 1200, &params, 77);
	struct Corruption corruption = { .site = 2, .atMs = 6000 };
	game.hook = _corrupt;
	game.hookData = &corruption;
	_play(&game);
	assert_true(corruption.done);
	struct mNetplayPeerStats host, client;
	mNetplayPeerGetStats(game.sites[0].peer, &host);
	mNetplayPeerGetStats(game.sites[2].peer, &client);
	assert_true(host.desyncs >= 1);
	assert_true(client.resyncs >= 1);
	/* After the snapshot, the corrupted site is back in line. */
	_assertAllMatch(&game);
	_teardown(&game);
}

static struct mNetplayPeer* _lonelyClient(struct Game* game, struct mNetplayListener** listener, struct mNetplayPeerConfig* clientConfig) {
	_warnings = 0;
	memset(game, 0, sizeof(*game));
	game->net = mNetplaySimNetworkCreate(5);
	game->players = 2;
	struct mNetplayClock* clock = _tickingClock(&game->clock, game->net);
	*listener = mNetplaySimListen(game->net);
	struct mNetplayPeerConfig hostConfig = _config(2, 120);
	game->sites[0].peer = mNetplayPeerCreateHost(&hostConfig, *listener, clock);
	struct mNetplayLink* link = mNetplaySimConnect(game->net, *listener, NULL);
	return mNetplayPeerCreateClient(clientConfig, link, clock);
}

static void _pollUntilSettled(struct Game* game, struct mNetplayPeer* client) {
	unsigned ms;
	for (ms = 0; ms < 2000 && !_settled(mNetplayPeerGetState(client)); ++ms) {
		mNetplayPeerPoll(game->sites[0].peer);
		mNetplayPeerPoll(client);
		mNetplaySimNetworkAdvance(game->net, 1);
	}
}

M_TEST_DEFINE(rejectsDifferentRom) {
	static uint8_t otherRom[sizeof(linktestRom)];
	memcpy(otherRom, linktestRom, sizeof(otherRom));
	otherRom[0x100] ^= 1;
	struct mNetplayPeerConfig config = _config(2, 120);
	config.rom = otherRom;
	struct Game game;
	struct mNetplayListener* listener;
	struct mNetplayPeer* client = _lonelyClient(&game, &listener, &config);
	_pollUntilSettled(&game, client);
	assert_int_equal(mNetplayPeerGetState(client), mNETPLAY_PEER_FAILED);
	assert_non_null(strstr(mNetplayPeerError(client), "Different ROM"));
	assert_int_equal(mNetplayPeerGetState(game.sites[0].peer), mNETPLAY_PEER_LOBBY);
	mNetplayPeerDestroy(client);
	_teardown(&game);
}

M_TEST_DEFINE(rejectsDifferentBuild) {
	struct mNetplayPeerConfig config = _config(2, 120);
	config.buildId = "some-other-build";
	struct Game game;
	struct mNetplayListener* listener;
	struct mNetplayPeer* client = _lonelyClient(&game, &listener, &config);
	_pollUntilSettled(&game, client);
	assert_int_equal(mNetplayPeerGetState(client), mNETPLAY_PEER_FAILED);
	assert_non_null(strstr(mNetplayPeerError(client), "Different emulator build"));
	mNetplayPeerDestroy(client);
	_teardown(&game);
}

struct Sever {
	unsigned site;
	uint64_t atMs;
	bool done;
};

static void _sever(struct Game* game, uint64_t ms) {
	struct Sever* sever = game->hookData;
	if (!sever->done && ms >= sever->atMs) {
		mNetplaySimSever(game->clientLinks[sever->site]);
		sever->done = true;
	}
}

M_TEST_DEFINE(lostPlayerEndsGame) {
	struct Game game;
	_setup(&game, 3, 6000, NULL, 9);
	struct Sever sever = { .site = 1, .atMs = 4000 };
	game.hook = _sever;
	game.hookData = &sever;
	_play(&game);
	unsigned i;
	for (i = 0; i < 3; ++i) {
		assert_int_equal(mNetplayPeerGetState(game.sites[i].peer), mNETPLAY_PEER_FAILED);
	}
	assert_non_null(strstr(mNetplayPeerError(game.sites[0].peer), "player 2"));
	assert_non_null(strstr(mNetplayPeerError(game.sites[2].peer), "dropped out"));
	_teardown(&game);
}

struct EndRace {
	/* Cut the client off while it waits at the end, as a host that closes
	 * without its goodbye arriving would. */
	bool sever;
	bool severed;
	uint64_t frozenAtMs;
	enum mNetplayPeerState clientEnd;
	uint32_t clientFrame;
	enum mNetplayPeerState hostWhenClientEnded;
};

/* The host stalls one frame short of the end, so the client gets there
 * first; like a frontend, the client lets go of the game once it is over. */
static void _hostLagsAtEnd(struct Game* game, uint64_t ms) {
	struct EndRace* race = game->hookData;
	struct Site* host = &game->sites[0];
	if (!race->frozenAtMs && mNetplayPeerSession(host->peer) && mNetplayPeerFrame(host->peer) + 1 >= game->endFrame) {
		race->frozenAtMs = ms;
	}
	host->frozen = race->frozenAtMs && ms < race->frozenAtMs + 200;
	struct Site* client = &game->sites[1];
	if (race->sever && !race->severed && host->frozen && client->peer && mNetplayPeerFrame(client->peer) >= game->endFrame) {
		mNetplaySimSever(game->clientLinks[1]);
		race->severed = true;
	}
	if (client->peer && _settled(mNetplayPeerGetState(client->peer))) {
		race->clientEnd = mNetplayPeerGetState(client->peer);
		race->clientFrame = mNetplayPeerFrame(client->peer);
		race->hostWhenClientEnded = mNetplayPeerGetState(host->peer);
		mNetplayPeerDestroy(client->peer);
		client->peer = NULL;
	}
}

M_TEST_DEFINE(clientWaitsForHostAtEnd) {
	struct Game game;
	_setup(&game, 2, 300, NULL, 13);
	struct EndRace race = {0};
	game.hook = _hostLagsAtEnd;
	game.hookData = &race;
	_play(&game);
	/* The loop may stop on the step the client finished in. */
	_hostLagsAtEnd(&game, UINT64_MAX);
	assert_true(race.frozenAtMs > 0);
	assert_int_equal(race.clientEnd, mNETPLAY_PEER_ENDED);
	assert_int_equal(race.clientFrame, 300);
	/* Ending first would have hung up on a host that still needed us. */
	assert_int_equal(race.hostWhenClientEnded, mNETPLAY_PEER_ENDED);
	assert_int_equal(mNetplayPeerGetState(game.sites[0].peer), mNETPLAY_PEER_ENDED);
	assert_int_equal(mNetplayPeerFrame(game.sites[0].peer), 300);
	_teardown(&game);
}

M_TEST_DEFINE(clientAtEndSurvivesLosingHost) {
	struct Game game;
	_setup(&game, 2, 300, NULL, 17);
	struct EndRace race = { .sever = true };
	game.hook = _hostLagsAtEnd;
	game.hookData = &race;
	_play(&game);
	_hostLagsAtEnd(&game, UINT64_MAX);
	assert_true(race.severed);
	/* The client had played everything, so it finished... */
	assert_int_equal(race.clientEnd, mNETPLAY_PEER_ENDED);
	assert_int_equal(race.clientFrame, 300);
	/* ...while the host, one frame short, lost a player. */
	assert_int_equal(mNetplayPeerGetState(game.sites[0].peer), mNETPLAY_PEER_FAILED);
	_warnings = 0;
	_teardown(&game);
}

M_TEST_SUITE_DEFINE_SETUP(NetplayPeer,
	cmocka_unit_test(twoPlayersLan),
	cmocka_unit_test(fourPlayersInternet),
	cmocka_unit_test(fourPlayersFarAway),
	cmocka_unit_test(desyncIsRepaired),
	cmocka_unit_test(rejectsDifferentRom),
	cmocka_unit_test(rejectsDifferentBuild),
	cmocka_unit_test(lostPlayerEndsGame),
	cmocka_unit_test(clientWaitsForHostAtEnd),
	cmocka_unit_test(clientAtEndSurvivesLosingHost))
