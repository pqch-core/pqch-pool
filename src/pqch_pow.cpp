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

#include "common.h"
#include "pqch_pow.h"
#include "randomx.h"
#include <algorithm>

LOG_CATEGORY(PqchPoW)

namespace p2pool {
namespace pqch {

namespace {

// Same flags as pqchd (pow_randomx.cpp): light mode; V2 only affects the virtual machine.
randomx_flags cache_flags(bool jit)
{
	int f = randomx_get_flags();
	f &= ~(RANDOMX_FLAG_FULL_MEM | RANDOMX_FLAG_LARGE_PAGES | RANDOMX_FLAG_V2);
	if (!jit) f &= ~(RANDOMX_FLAG_JIT | RANDOMX_FLAG_SECURE);
	return static_cast<randomx_flags>(f);
}

} // namespace

PowHasher::~PowHasher()
{
	release();
}

void PowHasher::release()
{
	if (m_vm) randomx_destroy_vm(m_vm);
	if (m_cache) randomx_release_cache(m_cache);
	m_vm = nullptr;
	m_cache = nullptr;
	m_haveKey = false;
}

bool PowHasher::set_key(const void* key, size_t size)
{
	const uint8_t* k = static_cast<const uint8_t*>(key);
	if (m_haveKey && (m_key.size() == size) && std::equal(m_key.begin(), m_key.end(), k)) {
		return true;
	}
	release();

	for (bool jit : { true, false }) {
		m_cache = randomx_alloc_cache(cache_flags(jit));
		if (!m_cache) continue;
		randomx_init_cache(m_cache, key, size);
		m_vm = randomx_create_vm(static_cast<randomx_flags>(cache_flags(jit) | RANDOMX_FLAG_V2), m_cache, nullptr);
		if (m_vm) break;
		randomx_release_cache(m_cache);
		m_cache = nullptr;
	}
	if (!m_vm) {
		LOGERR(0, "RandomX: could not allocate a cache and a virtual machine");
		return false;
	}
	m_key.assign(k, k + size);
	m_haveKey = true;
	return true;
}

hash PowHasher::calculate(const void* data, size_t size)
{
	hash h;
	if (m_vm) {
		randomx_calculate_hash(m_vm, data, size, h.h);
	}
	return h;
}

} // namespace pqch
} // namespace p2pool
