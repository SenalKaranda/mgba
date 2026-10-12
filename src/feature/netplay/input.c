/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "feature/netplay/input.h"

void mNetplayInputInit(struct mNetplayInputQueue* queue, unsigned players, unsigned delay) {
	memset(queue, 0, sizeof(*queue));
	queue->players = players;
	unsigned slot;
	for (slot = 0; slot < players; ++slot) {
		queue->known[slot] = delay;
	}
}

bool mNetplayInputAdd(struct mNetplayInputQueue* queue, unsigned slot, uint32_t start, const uint16_t* keys, size_t count) {
	if (slot >= queue->players) {
		return false;
	}
	size_t i;
	for (i = 0; i < count; ++i) {
		uint32_t frame = start + i;
		uint16_t value = keys[i] & mNETPLAY_KEY_MASK;
		if (frame < queue->known[slot]) {
			/* Too old to check against is fine: it was consumed long ago. */
			if (frame >= queue->floor && mNetplayInputGet(queue, slot, frame) != value) {
				return false;
			}
			continue;
		}
		if (frame > queue->known[slot]) {
			break;
		}
		if (frame - queue->floor >= mNETPLAY_INPUT_WINDOW) {
			return false;
		}
		queue->keys[slot][frame % mNETPLAY_INPUT_WINDOW] = value;
		++queue->known[slot];
	}
	return true;
}

uint32_t mNetplayInputMissing(const struct mNetplayInputQueue* queue, uint32_t frame) {
	uint32_t missing = 0;
	unsigned slot;
	for (slot = 0; slot < queue->players; ++slot) {
		if (!mNetplayInputHas(queue, slot, frame)) {
			missing |= 1 << slot;
		}
	}
	return missing;
}

void mNetplayInputSetFloor(struct mNetplayInputQueue* queue, uint32_t floor) {
	if (floor > queue->floor) {
		queue->floor = floor;
	}
}

void mNetplayInputRebase(struct mNetplayInputQueue* queue, uint32_t frame) {
	unsigned slot;
	for (slot = 0; slot < queue->players; ++slot) {
		if (queue->known[slot] < frame) {
			queue->known[slot] = frame;
		}
	}
	mNetplayInputSetFloor(queue, frame);
}
