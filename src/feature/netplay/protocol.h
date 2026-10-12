/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_NETPLAY_PROTOCOL_H
#define M_NETPLAY_PROTOCOL_H

#include <mgba-util/common.h>

CXX_GUARD_START

/* Wire format. Everything is little-endian and written byte by byte.
 *
 * Reliable messages start with a type byte:
 *   HELLO     client -> host: u32 magic, u32 protocol, u32 stepper,
 *             char build[48], u8 romSha1[20], blob save
 *   WELCOME   host -> client: u8 slot, u8 players
 *   REJECT    host -> client: u8 reason, string text
 *   LOBBY     host -> clients: u8 joined, u8 players
 *   START     host -> clients: u8 players, u8 delay, i64 rtcEpochMs,
 *             u32 endFrame, blob save x players
 *   HASHES    client -> host: u32 frame, u8 n, u32 hash x n
 *   SNAPSHOT  host -> client: u32 frame, blob snapshot
 *   BYE       either way: u8 reason; from the host with reason FINISHED,
 *             followed by key runs (as in datagrams) holding every key
 *             the client still lacks, so it can finish the last frames
 * where a blob is u32 size followed by the bytes and a string is a blob.
 *
 * Datagrams carry keys and timing, one per frame or so in each direction:
 *   u8 type (KEYS), u8 sender slot, u32 send time, u32 echoed send time,
 *   u32 echo delay, u32 sender frame,
 *   u8 nAcks, then nAcks x (u8 slot, u32 known),
 *   u8 nRuns, then nRuns x (u8 slot, u32 start, u8 count, u16 key x count)
 * The echo fields measure round trips; acks say which keys the receiver
 * already has, so the sender repeats only the rest (at most
 * mNETPLAY_MAX_RUN frames per slot), which also covers lost datagrams.
 */

#define mNETPLAY_MAGIC 0x504E4D4D /* "MMNP" */
#define mNETPLAY_BUILD_ID_LENGTH 48
#define mNETPLAY_MAX_RUN 32
#define mNETPLAY_MAX_DATAGRAM 1024

enum mNetplayMessage {
	mNETPLAY_MSG_HELLO = 1,
	mNETPLAY_MSG_WELCOME,
	mNETPLAY_MSG_REJECT,
	mNETPLAY_MSG_LOBBY,
	mNETPLAY_MSG_START,
	mNETPLAY_MSG_HASHES,
	mNETPLAY_MSG_SNAPSHOT,
	mNETPLAY_MSG_BYE,
};

enum mNetplayDatagram {
	mNETPLAY_DGRAM_KEYS = 1,
};

enum mNetplayReject {
	mNETPLAY_REJECT_PROTOCOL = 1,
	mNETPLAY_REJECT_BUILD,
	mNETPLAY_REJECT_ROM,
	mNETPLAY_REJECT_FULL,
	mNETPLAY_REJECT_MALFORMED,
};

enum mNetplayBye {
	mNETPLAY_BYE_FINISHED = 1,
	mNETPLAY_BYE_PLAYER_LOST,
	mNETPLAY_BYE_QUIT,
	mNETPLAY_BYE_ERROR,
};

struct mNetplayWriter {
	uint8_t* data;
	size_t size;
	size_t capacity;
	bool failed;
};

void mNetplayWriterInit(struct mNetplayWriter*);
void mNetplayWriterDeinit(struct mNetplayWriter*);
void mNetplayWriteU8(struct mNetplayWriter*, uint8_t);
void mNetplayWriteU16(struct mNetplayWriter*, uint16_t);
void mNetplayWriteU32(struct mNetplayWriter*, uint32_t);
void mNetplayWriteU64(struct mNetplayWriter*, uint64_t);
void mNetplayWriteBytes(struct mNetplayWriter*, const void*, size_t);
void mNetplayWriteBlob(struct mNetplayWriter*, const void*, size_t);

struct mNetplayReader {
	const uint8_t* data;
	size_t size;
	size_t pos;
	bool failed;
};

void mNetplayReaderInit(struct mNetplayReader*, const void* data, size_t size);
uint8_t mNetplayReadU8(struct mNetplayReader*);
uint16_t mNetplayReadU16(struct mNetplayReader*);
uint32_t mNetplayReadU32(struct mNetplayReader*);
uint64_t mNetplayReadU64(struct mNetplayReader*);
bool mNetplayReadBytes(struct mNetplayReader*, void*, size_t);
/* Points into the reader's buffer; returns NULL (and fails) if truncated. */
const void* mNetplayReadBlob(struct mNetplayReader*, size_t* size);

CXX_GUARD_END

#endif
