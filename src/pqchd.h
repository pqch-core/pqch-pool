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

// PQCH: the pool's connection to pqchd (the PQCH node, Bitcoin Core RPC style).
//
// pqchd hands out work with getblocktemplate and takes found blocks with submitblock. Hashes are kept
// here in internal byte order (the order of the 80-byte block header and of RandomX's key), which is the
// reverse of the hex shown by the RPC.

#include "common.h"

namespace p2pool {
namespace pqchd {

struct TemplateTx
{
	std::vector<uint8_t> data;   // serialized transaction, with witness
	hash txid;                   // internal byte order
	hash wtxid;                  // internal byte order
	uint64_t fee = 0;            // in quanta (1 PQCH = 100,000,000 quanta)
	uint64_t weight = 0;
	uint64_t sigops = 0;
	std::vector<uint32_t> depends; // 1-based indices into Template::transactions
};

struct Template
{
	int32_t version = 0;
	hash prev_hash;              // internal byte order
	uint64_t height = 0;
	uint32_t bits = 0;
	hash target;                 // little-endian 256-bit number (internal byte order)
	uint64_t curtime = 0;
	uint64_t mintime = 0;
	uint64_t coinbase_value = 0; // subsidy + fees, in quanta
	hash seed_hash;              // RandomX key for this block, internal byte order
	uint64_t seed_height = 0;
	std::vector<uint8_t> witness_commitment; // scriptPubKey of the coinbase's SegWit commitment output
	uint64_t weight_limit = 0;
	uint64_t sigop_limit = 0;
	std::string longpollid;
	std::vector<TemplateTx> transactions;
};

// The JSON-RPC request bodies.
std::string getblocktemplate_request(const std::string& longpollid = std::string());
std::string submitblock_request(const uint8_t* block, size_t size);

// Parse a getblocktemplate response. Returns false (and a reason in `error`) on an RPC error or a
// malformed / incomplete template; `out` is only valid when it returns true.
bool parse_getblocktemplate(const char* data, size_t size, Template& out, std::string& error);

// Parse a submitblock response. Returns true if the block was accepted ("result": null); otherwise
// `reason` holds the node's reject reason (e.g. "high-hash", "bad-txns-output-not-pq", "duplicate").
bool parse_submitblock(const char* data, size_t size, std::string& reason);

// Authentication string for JSONRPCRequest from pqchd's cookie file (<datadir>/<network>/.cookie):
// "basic:__cookie__:<password>". Returns false if the file cannot be read or is malformed.
bool read_cookie(const std::string& path, std::string& auth);

// Authentication string from an explicit user and password (-rpcuser / -rpcpassword style).
std::string make_auth(const std::string& user, const std::string& password);

} // namespace pqchd
} // namespace p2pool
