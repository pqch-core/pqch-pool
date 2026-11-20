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

#include "pqchd_client.h"

namespace p2pool {
namespace pqch {

struct PoolOptions
{
	PqchdClient::Config pqchd;
	std::vector<std::string> miners;      // P2MR addresses, one built-in miner thread each
	uint64_t share_time = 10;
	uint64_t min_difficulty = 100000;
	uint64_t seconds = 0;                 // run time; 0 = until stopped
	std::string log_file = "pqch-pool.jsonl";
	uint16_t p2p_port = 0;                // listen for other pool nodes (0 = do not listen)
	std::vector<std::string> peers;       // other pool nodes to connect to, "ip:port"
	std::string network;                  // main | testnet | regtest (needed only without miners)
};

// `pqch-pool --pqch-pool <host:port> <cookie|user:password> <addr,addr,...|-> [--share-time S] [--min-diff D]
//  [--seconds N] [--log FILE] [--p2p-port P] [--peer ip:port]... [--network N]`. Exit 0 if no block was rejected.
int run_pool(const PoolOptions& opt);

} // namespace pqch
} // namespace p2pool
