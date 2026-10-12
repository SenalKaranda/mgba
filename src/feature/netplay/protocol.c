/* Copyright (c) 2026 Senal Karanda
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "feature/netplay/protocol.h"

void mNetplayWriterInit(struct mNetplayWriter* writer) {
	memset(writer, 0, sizeof(*writer));
}

void mNetplayWriterDeinit(struct mNetplayWriter* writer) {
	free(writer->data);
	memset(writer, 0, sizeof(*writer));
}

static bool _reserve(struct mNetplayWriter* writer, size_t more) {
	if (writer->failed) {
		return false;
	}
	if (writer->size + more <= writer->capacity) {
		return true;
	}
	size_t capacity = writer->capacity ? writer->capacity : 64;
	while (capacity < writer->size + more) {
		capacity *= 2;
	}
	uint8_t* data = realloc(writer->data, capacity);
	if (!data) {
		writer->failed = true;
		return false;
	}
	writer->data = data;
	writer->capacity = capacity;
	return true;
}

void mNetplayWriteU8(struct mNetplayWriter* writer, uint8_t value) {
	if (_reserve(writer, 1)) {
		writer->data[writer->size++] = value;
	}
}

void mNetplayWriteU16(struct mNetplayWriter* writer, uint16_t value) {
	mNetplayWriteU8(writer, value);
	mNetplayWriteU8(writer, value >> 8);
}

void mNetplayWriteU32(struct mNetplayWriter* writer, uint32_t value) {
	mNetplayWriteU16(writer, value);
	mNetplayWriteU16(writer, value >> 16);
}

void mNetplayWriteU64(struct mNetplayWriter* writer, uint64_t value) {
	mNetplayWriteU32(writer, value);
	mNetplayWriteU32(writer, value >> 32);
}

void mNetplayWriteBytes(struct mNetplayWriter* writer, const void* data, size_t size) {
	if (size && _reserve(writer, size)) {
		memcpy(&writer->data[writer->size], data, size);
		writer->size += size;
	}
}

void mNetplayWriteBlob(struct mNetplayWriter* writer, const void* data, size_t size) {
	if (size > UINT32_MAX) {
		writer->failed = true;
		return;
	}
	mNetplayWriteU32(writer, size);
	mNetplayWriteBytes(writer, data, size);
}

void mNetplayReaderInit(struct mNetplayReader* reader, const void* data, size_t size) {
	reader->data = data;
	reader->size = size;
	reader->pos = 0;
	reader->failed = false;
}

static bool _available(struct mNetplayReader* reader, size_t size) {
	if (reader->failed || reader->size - reader->pos < size) {
		reader->failed = true;
		return false;
	}
	return true;
}

uint8_t mNetplayReadU8(struct mNetplayReader* reader) {
	if (!_available(reader, 1)) {
		return 0;
	}
	return reader->data[reader->pos++];
}

uint16_t mNetplayReadU16(struct mNetplayReader* reader) {
	uint16_t low = mNetplayReadU8(reader);
	return low | (mNetplayReadU8(reader) << 8);
}

uint32_t mNetplayReadU32(struct mNetplayReader* reader) {
	uint32_t low = mNetplayReadU16(reader);
	return low | ((uint32_t) mNetplayReadU16(reader) << 16);
}

uint64_t mNetplayReadU64(struct mNetplayReader* reader) {
	uint64_t low = mNetplayReadU32(reader);
	return low | ((uint64_t) mNetplayReadU32(reader) << 32);
}

bool mNetplayReadBytes(struct mNetplayReader* reader, void* data, size_t size) {
	if (!_available(reader, size)) {
		return false;
	}
	memcpy(data, &reader->data[reader->pos], size);
	reader->pos += size;
	return true;
}

const void* mNetplayReadBlob(struct mNetplayReader* reader, size_t* size) {
	*size = mNetplayReadU32(reader);
	if (!_available(reader, *size)) {
		*size = 0;
		return NULL;
	}
	const void* data = &reader->data[reader->pos];
	reader->pos += *size;
	return data;
}
