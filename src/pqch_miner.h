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

#include <cstdint>
#include <string>

namespace p2pool {
namespace pqch {

// PQCH (phase C): a miner without a node. It connects to a pool node's stratum (pqch_stratum.h), mines the
// jobs it gets with RandomX v2 and submits the shares; the pool pays its address by PPLNS.
struct MinerOptions
{
	std::string pool;          // "ip:port" of a pool node's stratum
	std::string address;       // P2MR address that gets paid
	unsigned threads = 1;
	bool fast = false;         // RandomX fast mode (about 2 GiB of memory, several times faster)
	uint64_t seconds = 0;      // run time; 0 = until stopped
};

// `pqch-pool --pqch-miner <ip:port> <address> [--threads N] [--fast] [--seconds N]`.
// Exit 0 if no share was rejected (other than stale ones), 1 otherwise or if the pool refused the login.
int run_miner(const MinerOptions& opt);

} // namespace pqch
} // namespace p2pool
