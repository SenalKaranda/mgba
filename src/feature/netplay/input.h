/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_NETPLAY_INPUT_H
#define M_NETPLAY_INPUT_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/feature/netplay.h>

/* How many frames of keys each slot keeps. Keys must stay around for the
 * frames a site may still run (its own frame plus the input delay) and for
 * the frames it may re-run after loading a snapshot from behind. */
#define mNETPLAY_INPUT_WINDOW 512
#define mNETPLAY_INPUT_HISTORY 192

/* Every slot's keys by frame. Each slot's keys are only ever appended in
 * frame order, so "known" is a single frame count per slot. */
struct mNetplayInputQueue {
	unsigned players;
	uint32_t known[mNETPLAY_MAX_PLAYERS];
	/* Oldest frame still needed; frames before it may be overwritten. */
	uint32_t floor;
	uint16_t keys[mNETPLAY_MAX_PLAYERS][mNETPLAY_INPUT_WINDOW];
};

/* Frames below delay are neutral for every slot. */
void mNetplayInputInit(struct mNetplayInputQueue*, unsigned players, unsigned delay);

/* Records keys for a run of frames starting at start. Frames already known
 * must match (returns false on a conflict); new frames must follow on
 * without a gap, so anything past the first gap is ignored and the sender's
 * redundancy fills it in later. Returns false on conflicts or overflow. */
bool mNetplayInputAdd(struct mNetplayInputQueue*, unsigned slot, uint32_t start, const uint16_t* keys, size_t count);

static inline bool mNetplayInputHas(const struct mNetplayInputQueue* queue, unsigned slot, uint32_t frame) {
	return frame < queue->known[slot];
}

static inline uint16_t mNetplayInputGet(const struct mNetplayInputQueue* queue, unsigned slot, uint32_t frame) {
	return queue->keys[slot][frame % mNETPLAY_INPUT_WINDOW];
}

/* Bit per slot whose keys for frame are not known yet. */
uint32_t mNetplayInputMissing(const struct mNetplayInputQueue*, uint32_t frame);

/* Tells the queue which frames may be forgotten. */
void mNetplayInputSetFloor(struct mNetplayInputQueue*, uint32_t floor);

/* After loading a snapshot of frame: nothing before it will run again, so
 * every slot counts as known up to there and older keys are forgotten. */
void mNetplayInputRebase(struct mNetplayInputQueue*, uint32_t frame);

CXX_GUARD_END

#endif
