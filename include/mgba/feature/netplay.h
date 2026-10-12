/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_FEATURE_NETPLAY_H
#define M_FEATURE_NETPLAY_H

#include <mgba-util/common.h>

CXX_GUARD_START

/* Input-synchronized netplay for linked GBAs.
 *
 * A session owns every player's GBA core and links them through the
 * in-process lockstep SIO driver. Unlike the threaded multiplayer window,
 * all cores are stepped from the calling thread in a fixed order, so the
 * outcome of every serial transfer depends only on emulated time and the
 * keys passed to mNetplaySessionRunFrame(). Every site of a netplay game
 * runs the same session with the same inputs and therefore computes the
 * same state; only keys need to cross the network.
 *
 * Host-dependent inputs are pinned: the HLE BIOS is used, the idle-loop
 * optimization is off, cartridge overrides come only from the built-in
 * table, the RTC counts emulated time from a shared epoch, and the process
 * time zone is pinned to UTC while a session exists (the GBA RTC converts
 * through localtime_r()/mktime()).
 */

#define mNETPLAY_MAX_PLAYERS 4
#define mNETPLAY_STEPPER_VERSION 1
#define mNETPLAY_KEY_MASK 0x3FF

struct mCore;
struct VFile;

struct mNetplaySessionSettings {
	/* Number of linked GBAs, 2 to mNETPLAY_MAX_PLAYERS. */
	unsigned players;
	/* RTC epoch in milliseconds since 1970, identical on every site. */
	int64_t rtcEpochMs;
};

enum mNetplayStepResult {
	mNETPLAY_STEP_OK = 0,
	/* Every core is waiting on another; the session cannot continue. */
	mNETPLAY_STEP_DEADLOCK,
	/* mNetplaySessionStart() has not succeeded. */
	mNETPLAY_STEP_NOT_STARTED,
};

struct mNetplaySession;

struct mNetplaySession* mNetplaySessionCreate(const struct mNetplaySessionSettings* settings);
void mNetplaySessionDestroy(struct mNetplaySession* session);

/* Loads a private copy of the ROM into every core. */
bool mNetplaySessionLoadROM(struct mNetplaySession* session, const void* rom, size_t size);
/* Loads a battery save for one player; size 0 starts with an empty save. */
bool mNetplaySessionLoadSave(struct mNetplaySession* session, unsigned slot, const void* save, size_t size);
/* Resets every core and links them in slot order. */
bool mNetplaySessionStart(struct mNetplaySession* session);

/* Emulates one frame of player 0, the link clock owner. The other cores
 * advance to the same emulated time, give or take one lockstep interval.
 * keys[slot] is applied to each core first (masked to mNETPLAY_KEY_MASK). */
enum mNetplayStepResult mNetplaySessionRunFrame(struct mNetplaySession* session, const uint16_t* keys);

unsigned mNetplaySessionPlayers(const struct mNetplaySession* session);
uint32_t mNetplaySessionFrame(const struct mNetplaySession* session);
struct mCore* mNetplaySessionGetCore(struct mNetplaySession* session, unsigned slot);

/* Hash of one core's emulated state and battery save, for desync checks. */
uint32_t mNetplaySessionHashCore(struct mNetplaySession* session, unsigned slot);

/* Whole-session snapshot for resynchronization (and later, rollback). */
bool mNetplaySessionSaveSnapshot(struct mNetplaySession* session, struct VFile* vf);
bool mNetplaySessionLoadSnapshot(struct mNetplaySession* session, struct VFile* vf);

/* Copies one player's battery save; free() the result. Returns its size. */
size_t mNetplaySessionCopySave(struct mNetplaySession* session, unsigned slot, void** save);

CXX_GUARD_END

#endif
