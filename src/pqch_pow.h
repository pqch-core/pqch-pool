/*
 * This file is part of pqch-pool, a fork of the p2pool project for PQCH (PostQuantum Cash).
 * Copyright (c) 2026 The PQCH developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

// PQCH proof of work: RandomX v2 (RANDOMX_FLAG_V2) of the 80-byte header, keyed with the template's
// randomx_seed_hash (internal byte order), light mode (256 MiB cache), exactly as pqchd computes it.

#include "common.h"

struct randomx_cache;
class randomx_vm;
struct randomx_dataset;

namespace p2pool {
namespace pqch {

// Fast mode: the full RandomX dataset (about 2 GiB), built once per key and shared by every miner thread.
// Several times faster than light mode; rebuilt only when the key changes (every 4096 main-chain blocks).
class PowDataset : public nocopy_nomove
{
public:
	PowDataset() = default;
	~PowDataset();

	// Build the dataset for `key` with `threads` threads (seconds to a minute). Cheap if the key did not change.
	// False if the memory cannot be allocated. Not thread-safe: the caller serializes calls.
	bool prepare(const hash& key, unsigned threads);
	randomx_dataset* dataset() const { return m_dataset; }
	const hash& key() const { return m_key; }
	uint64_t generation() const { return m_generation; } // changes with every rebuild

private:
	randomx_cache* m_cache = nullptr;
	randomx_dataset* m_dataset = nullptr;
	hash m_key;
	bool m_ready = false;
	uint64_t m_generation = 0;
};

class PowHasher : public nocopy_nomove
{
public:
	PowHasher() = default;
	~PowHasher();

	// (Re)build the cache for a key; cheap if the key did not change. False if RandomX cannot allocate.
	bool set_key(const hash& key) { return set_key(key.h, HASH_SIZE); }
	bool set_key(const void* key, size_t size);

	// Fast mode: hash with a prepared dataset instead of a light cache.
	bool use_dataset(const PowDataset& d);

	// RandomX v2 hash of `data` (the 80-byte header). Not thread-safe: one PowHasher per thread.
	hash calculate(const void* data, size_t size);

private:
	void release();

	std::vector<uint8_t> m_key;
	bool m_haveKey = false;
	randomx_cache* m_cache = nullptr;
	randomx_vm* m_vm = nullptr;
	uint64_t m_datasetGeneration = 0;  // 0: light mode
};

} // namespace pqch
} // namespace p2pool
