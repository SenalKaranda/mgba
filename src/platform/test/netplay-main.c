/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Runs a game through a netplay session with no network and no GUI.
 *
 * Every player's GBA is emulated in this one process exactly as a netplay
 * site would, so this answers three questions about a game before anyone
 * plays it online:
 *
 *  - Does its link work under the deterministic stepper? Lockstep
 *    fallbacks and other emulator warnings are counted and printed.
 *  - Is it deterministic across machines? Run the same command on two
 *    PCs; the "hash" lines must be identical.
 *  - Is this PC fast enough? The summary reports frames per second for
 *    all players together; netplay needs at least 60.
 *
 * --check-snapshots additionally restores a snapshot into a fresh session
 * every N frames and verifies it keeps up with the original, which is
 * what desync recovery relies on.
 */
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/feature/netplay.h>
#include <mgba-util/image.h>
#ifdef USE_PNG
#include <mgba-util/image/png-io.h>
#endif
#include <mgba-util/vfs.h>

#include <errno.h>
#include <stdio.h>
#include <sys/time.h>

#define GBA_WIDTH 240
#define GBA_HEIGHT 160
#define MAX_INPUTS 4096
#define SNAPSHOT_CHECK_FRAMES 60
#define MAX_PRINTED_WARNINGS 20

static const char* const _keyNames[] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L" };

struct Input {
	uint32_t frame;
	uint32_t duration;
	unsigned slot;
	uint16_t keys;
};

struct Options {
	const char* rom;
	const char* saves[mNETPLAY_MAX_PLAYERS];
	const char* inputScript;
	const char* dumpDir;
	const char* saveDir;
	unsigned players;
	uint32_t frames;
	uint32_t hashEvery;
	uint32_t dumpEvery;
	uint32_t checkSnapshotsEvery;
	uint32_t randomSeed;
	bool randomKeys;
	int64_t rtcEpochMs;
};

static unsigned _warnings;

static void _log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	if (!(level & (mLOG_FATAL | mLOG_ERROR | mLOG_WARN))) {
		return;
	}
	if (_warnings++ < MAX_PRINTED_WARNINGS) {
		fprintf(stderr, "warning: [%s] ", mLogCategoryName(category));
		vfprintf(stderr, format, args);
		fprintf(stderr, "\n");
	} else if (_warnings == MAX_PRINTED_WARNINGS + 1) {
		fprintf(stderr, "warning: further warnings are counted but not shown\n");
	}
}

static struct mLogger _logger = { .log = _log };

static void _usage(const char* name) {
	fprintf(stderr,
		"usage: %s [options] ROM\n"
		"  --players N             linked GBAs, 2-4 (default 2)\n"
		"  --frames N              frames to run (default 3600)\n"
		"  --sav0..--sav3 FILE     battery save for a player\n"
		"  --input FILE            key script, lines of: FRAME SLOT KEYS [DURATION]\n"
		"                          KEYS is A+B+SELECT+START+RIGHT+LEFT+UP+DOWN+R+L or NONE\n"
		"  --random-keys SEED      press pseudo-random keys instead (stress test)\n"
		"  --hash-every N          print every player's state hash every N frames (default 600)\n"
		"  --check-snapshots N     every N frames, restore a snapshot into a fresh session\n"
		"                          and check it stays identical for %d frames\n"
		"  --dump DIR              write PNG frames for every player into DIR\n"
		"  --dump-every N          frames between dumps (default 600)\n"
		"  --write-saves DIR       write every player's final battery save into DIR\n"
		"  --rtc-epoch MS          RTC start, milliseconds since 1970 (default 2026-01-01)\n",
		name, SNAPSHOT_CHECK_FRAMES);
}

static bool _parseUInt(const char* arg, uint32_t* out) {
	char* end;
	errno = 0;
	unsigned long value = strtoul(arg, &end, 10);
	if (errno || end == arg || *end) {
		return false;
	}
	*out = value;
	return true;
}

static bool _parseArgs(int argc, char** argv, struct Options* opts) {
	int i;
	for (i = 1; i < argc; ++i) {
		const char* arg = argv[i];
		const char* value = i + 1 < argc ? argv[i + 1] : NULL;
		uint32_t number = 0;
		if (arg[0] != '-') {
			if (opts->rom) {
				return false;
			}
			opts->rom = arg;
			continue;
		}
		if (!value) {
			return false;
		}
		++i;
		if (strncmp(arg, "--sav", 5) == 0 && arg[5] >= '0' && arg[5] < '0' + mNETPLAY_MAX_PLAYERS && !arg[6]) {
			opts->saves[arg[5] - '0'] = value;
		} else if (strcmp(arg, "--input") == 0) {
			opts->inputScript = value;
		} else if (strcmp(arg, "--dump") == 0) {
			opts->dumpDir = value;
		} else if (strcmp(arg, "--write-saves") == 0) {
			opts->saveDir = value;
		} else if (strcmp(arg, "--rtc-epoch") == 0) {
			char* end;
			opts->rtcEpochMs = strtoll(value, &end, 10);
			if (*end) {
				return false;
			}
		} else if (_parseUInt(value, &number)) {
			if (strcmp(arg, "--players") == 0) {
				opts->players = number;
			} else if (strcmp(arg, "--frames") == 0) {
				opts->frames = number;
			} else if (strcmp(arg, "--hash-every") == 0) {
				opts->hashEvery = number;
			} else if (strcmp(arg, "--dump-every") == 0) {
				opts->dumpEvery = number;
			} else if (strcmp(arg, "--check-snapshots") == 0) {
				opts->checkSnapshotsEvery = number;
			} else if (strcmp(arg, "--random-keys") == 0) {
				opts->randomKeys = true;
				opts->randomSeed = number;
			} else {
				return false;
			}
		} else {
			return false;
		}
	}
	return opts->rom && opts->players >= 2 && opts->players <= mNETPLAY_MAX_PLAYERS;
}

static bool _parseKeys(char* text, uint16_t* keys) {
	*keys = 0;
	if (strcasecmp(text, "NONE") == 0) {
		return true;
	}
	char* name = text;
	while (name) {
		char* next = strchr(name, '+');
		if (next) {
			*next++ = '\0';
		}
		size_t i;
		for (i = 0; i < sizeof(_keyNames) / sizeof(*_keyNames); ++i) {
			if (strcasecmp(name, _keyNames[i]) == 0) {
				*keys |= 1 << i;
				break;
			}
		}
		if (i == sizeof(_keyNames) / sizeof(*_keyNames)) {
			return false;
		}
		name = next;
	}
	return true;
}

static size_t _loadInputs(const char* path, struct Input* inputs, unsigned players) {
	FILE* file = fopen(path, "r");
	if (!file) {
		fprintf(stderr, "Could not open input script %s\n", path);
		return SIZE_MAX;
	}
	char line[256];
	size_t n = 0;
	unsigned lineNo = 0;
	while (fgets(line, sizeof(line), file)) {
		++lineNo;
		char* hash = strchr(line, '#');
		if (hash) {
			*hash = '\0';
		}
		unsigned long frame, slot, duration = 1;
		char keys[128];
		int fields = sscanf(line, "%lu %lu %127s %lu", &frame, &slot, keys, &duration);
		if (fields <= 0) {
			continue;
		}
		struct Input input = { .frame = frame, .slot = slot, .duration = duration };
		if (fields < 3 || slot >= players || !duration || !_parseKeys(keys, &input.keys) || n == MAX_INPUTS) {
			fprintf(stderr, "%s:%u: bad input line\n", path, lineNo);
			fclose(file);
			return SIZE_MAX;
		}
		inputs[n++] = input;
	}
	fclose(file);
	return n;
}

static uint16_t _randomKeys(uint32_t seed, unsigned slot, uint32_t frame) {
	uint32_t x = seed ^ (slot * 0x9E3779B9u) ^ ((frame >> 3) * 0x85EBCA6Bu);
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	return x & mNETPLAY_KEY_MASK;
}

static void _keysForFrame(const struct Options* opts, const struct Input* inputs, size_t nInputs, uint32_t frame, uint16_t* keys) {
	memset(keys, 0, sizeof(*keys) * mNETPLAY_MAX_PLAYERS);
	unsigned slot;
	if (opts->randomKeys) {
		for (slot = 0; slot < opts->players; ++slot) {
			keys[slot] = _randomKeys(opts->randomSeed, slot, frame);
		}
	}
	size_t i;
	for (i = 0; i < nInputs; ++i) {
		if (frame >= inputs[i].frame && frame - inputs[i].frame < inputs[i].duration) {
			keys[inputs[i].slot] |= inputs[i].keys;
		}
	}
}

static void* _readFile(const char* path, size_t* size) {
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) {
		fprintf(stderr, "Could not open %s\n", path);
		return NULL;
	}
	ssize_t length = vf->size(vf);
	void* data = length > 0 ? malloc(length) : NULL;
	if (!data || vf->read(vf, data, length) != length) {
		fprintf(stderr, "Could not read %s\n", path);
		free(data);
		vf->close(vf);
		return NULL;
	}
	vf->close(vf);
	*size = length;
	return data;
}

static struct mNetplaySession* _createSession(const struct Options* opts, const void* rom, size_t romSize, void** saves, size_t* saveSizes) {
	struct mNetplaySessionSettings settings = {
		.players = opts->players,
		.rtcEpochMs = opts->rtcEpochMs,
	};
	struct mNetplaySession* session = mNetplaySessionCreate(&settings);
	if (!session || !mNetplaySessionLoadROM(session, rom, romSize)) {
		mNetplaySessionDestroy(session);
		return NULL;
	}
	unsigned slot;
	for (slot = 0; slot < opts->players; ++slot) {
		if (!mNetplaySessionLoadSave(session, slot, saves[slot], saveSizes[slot])) {
			mNetplaySessionDestroy(session);
			return NULL;
		}
	}
	return session;
}

#ifdef USE_PNG
static bool _dumpFrame(const char* dir, unsigned slot, uint32_t frame, const mColor* pixels) {
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s" PATH_SEP "p%u_%06u.png", dir, slot, frame);
	struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_WRONLY);
	if (!vf) {
		return false;
	}
	png_structp png = PNGWriteOpen(vf);
	png_infop info = PNGWriteHeader(png, GBA_WIDTH, GBA_HEIGHT, mCOLOR_NATIVE);
	bool ok = PNGWritePixels(png, GBA_WIDTH, GBA_HEIGHT, GBA_WIDTH, pixels, mCOLOR_NATIVE);
	PNGWriteClose(png, info);
	vf->close(vf);
	return ok;
}
#endif

static bool _writeSaves(const char* dir, struct mNetplaySession* session, unsigned players) {
	unsigned slot;
	for (slot = 0; slot < players; ++slot) {
		void* save;
		size_t size = mNetplaySessionCopySave(session, slot, &save);
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s" PATH_SEP "player%u.sav", dir, slot + 1);
		struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_WRONLY);
		bool ok = vf && vf->write(vf, save, size) == (ssize_t) size;
		if (vf) {
			vf->close(vf);
		}
		free(save);
		if (!ok) {
			fprintf(stderr, "Could not write %s\n", path);
			return false;
		}
	}
	return true;
}

static double _now(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(int argc, char** argv) {
	struct Options opts = {
		.players = 2,
		.frames = 3600,
		.hashEvery = 600,
		.dumpEvery = 600,
		.rtcEpochMs = 1767225600000LL, /* 2026-01-01T00:00:00Z */
	};
	if (!_parseArgs(argc, argv, &opts)) {
		_usage(argv[0]);
		return 2;
	}
#ifndef USE_PNG
	if (opts.dumpDir) {
		fprintf(stderr, "This build has no PNG support; --dump is unavailable\n");
		return 2;
	}
#endif
	mLogSetDefaultLogger(&_logger);

	static struct Input inputs[MAX_INPUTS];
	size_t nInputs = 0;
	if (opts.inputScript) {
		nInputs = _loadInputs(opts.inputScript, inputs, opts.players);
		if (nInputs == SIZE_MAX) {
			return 2;
		}
	}

	size_t romSize = 0;
	void* rom = _readFile(opts.rom, &romSize);
	if (!rom) {
		return 1;
	}
	void* saves[mNETPLAY_MAX_PLAYERS] = {0};
	size_t saveSizes[mNETPLAY_MAX_PLAYERS] = {0};
	unsigned slot;
	for (slot = 0; slot < opts.players; ++slot) {
		if (opts.saves[slot] && !(saves[slot] = _readFile(opts.saves[slot], &saveSizes[slot]))) {
			return 1;
		}
	}

	struct mNetplaySession* session = _createSession(&opts, rom, romSize, saves, saveSizes);
	if (!session) {
		fprintf(stderr, "Could not set up the session\n");
		return 1;
	}
	mColor* video[mNETPLAY_MAX_PLAYERS] = {0};
	if (opts.dumpDir) {
		for (slot = 0; slot < opts.players; ++slot) {
			struct mCore* core = mNetplaySessionGetCore(session, slot);
			video[slot] = calloc(GBA_WIDTH * GBA_HEIGHT, sizeof(mColor));
			core->setVideoBuffer(core, video[slot], GBA_WIDTH);
		}
	}
	if (!mNetplaySessionStart(session)) {
		fprintf(stderr, "Could not start the session\n");
		return 1;
	}

	struct mNetplaySession* shadow = NULL;
	uint32_t shadowUntil = 0;
	unsigned snapshotChecks = 0;
	unsigned snapshotFailures = 0;
	int status = 0;
	double start = _now();
	double shadowTime = 0;

	uint32_t frame;
	for (frame = 0; frame < opts.frames; ++frame) {
		uint16_t keys[mNETPLAY_MAX_PLAYERS];
		_keysForFrame(&opts, inputs, nInputs, frame, keys);

		if (opts.checkSnapshotsEvery && !shadow && frame && frame % opts.checkSnapshotsEvery == 0) {
			double t = _now();
			struct VFile* snapshot = VFileMemChunk(NULL, 0);
			shadow = _createSession(&opts, rom, romSize, saves, saveSizes);
			if (!snapshot || !shadow || !mNetplaySessionStart(shadow) ||
			    !mNetplaySessionSaveSnapshot(session, snapshot) ||
			    (snapshot->seek(snapshot, 0, SEEK_SET), !mNetplaySessionLoadSnapshot(shadow, snapshot))) {
				fprintf(stderr, "frame %u: snapshot could not be restored\n", frame);
				++snapshotFailures;
				mNetplaySessionDestroy(shadow);
				shadow = NULL;
			} else {
				shadowUntil = frame + SNAPSHOT_CHECK_FRAMES;
				++snapshotChecks;
			}
			if (snapshot) {
				snapshot->close(snapshot);
			}
			shadowTime += _now() - t;
		}

		enum mNetplayStepResult result = mNetplaySessionRunFrame(session, keys);
		if (result != mNETPLAY_STEP_OK) {
			fprintf(stderr, "frame %u: the session stopped (%s)\n", frame,
			        result == mNETPLAY_STEP_DEADLOCK ? "lockstep deadlock" : "not running");
			status = 1;
			break;
		}
		uint32_t done = mNetplaySessionFrame(session);

		if (shadow) {
			double t = _now();
			bool ok = mNetplaySessionRunFrame(shadow, keys) == mNETPLAY_STEP_OK;
			if (ok && (done == shadowUntil || done % 10 == 0)) {
				for (slot = 0; slot < opts.players && ok; ++slot) {
					ok = mNetplaySessionHashCore(session, slot) == mNetplaySessionHashCore(shadow, slot);
				}
			}
			if (!ok) {
				fprintf(stderr, "frame %u: a restored snapshot diverged from the session\n", done);
				++snapshotFailures;
			}
			if (!ok || done >= shadowUntil) {
				mNetplaySessionDestroy(shadow);
				shadow = NULL;
			}
			shadowTime += _now() - t;
		}

		if (opts.hashEvery && done % opts.hashEvery == 0) {
			printf("hash %u:", done);
			for (slot = 0; slot < opts.players; ++slot) {
				printf(" %08X", mNetplaySessionHashCore(session, slot));
			}
			printf("\n");
			fflush(stdout);
		}
#ifdef USE_PNG
		if (opts.dumpDir && opts.dumpEvery && done % opts.dumpEvery == 0) {
			for (slot = 0; slot < opts.players; ++slot) {
				if (!_dumpFrame(opts.dumpDir, slot, done, video[slot])) {
					fprintf(stderr, "Could not write a frame dump into %s\n", opts.dumpDir);
					status = 1;
				}
			}
		}
#endif
	}
	double elapsed = _now() - start - shadowTime;
	mNetplaySessionDestroy(shadow);

	printf("final %u:", mNetplaySessionFrame(session));
	for (slot = 0; slot < opts.players; ++slot) {
		printf(" %08X", mNetplaySessionHashCore(session, slot));
	}
	printf("\n");

	if (opts.saveDir && !_writeSaves(opts.saveDir, session, opts.players)) {
		status = 1;
	}

	uint32_t ran = mNetplaySessionFrame(session);
	double fps = elapsed > 0 ? ran / elapsed : 0;
	fprintf(stderr, "%u players, %u frames in %.2f s: %.1f fps for all players together (%.2fx of the 59.73 fps netplay needs)\n",
	        opts.players, ran, elapsed, fps, fps / 59.7275);
	if (opts.checkSnapshotsEvery) {
		fprintf(stderr, "snapshot checks: %u run, %u failed\n", snapshotChecks, snapshotFailures);
	}
	fprintf(stderr, "emulator warnings: %u\n", _warnings);
	if (_warnings || snapshotFailures) {
		status = 1;
	}

	mNetplaySessionDestroy(session);
	for (slot = 0; slot < mNETPLAY_MAX_PLAYERS; ++slot) {
		free(video[slot]);
		free(saves[slot]);
	}
	free(rom);
	return status;
}
