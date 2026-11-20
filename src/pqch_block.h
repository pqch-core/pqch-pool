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

// PQCH: the main-chain block the pool mines, built from a pqchd block template.
//
//   header (80 bytes): version | previous block | merkle root | time | bits | nonce (byte 76)
//   coinbase: scriptSig = BIP34 height | 32-byte share id | 8-byte extranonce
//             outputs  = one P2MR output per miner of the PPLNS window | SegWit commitment
//             witness  = 32 zero bytes (the nonce of the template's default_witness_commitment)
//   then the template's transactions, unchanged.
//
// The coinbase wtxid counts as zero in the witness merkle root, so the template's commitment stays
// valid whatever the coinbase pays. Proof of work: RandomX v2 over the 80-byte header with the
// template's key (randomx_seed_hash), at most the target.

#include "pqchd.h"

namespace p2pool {
namespace pqch {

enum class Network { MAIN, TEST, REGTEST };

// Bech32m human-readable part of P2MR addresses: pqc1p..., pqct1p..., pqcrt1p...
const char* address_hrp(Network net);

// P2MR output script (OP_1 <32 bytes>, 34 bytes) from a P2MR address of `net`. False for anything else
// (wrong network, bad checksum, Bech32 instead of Bech32m, another witness version or length).
bool p2mr_script_from_address(const std::string& address, Network net, std::vector<uint8_t>& script);

// The address of a P2MR output script (inverse of the above).
bool address_from_p2mr_script(const std::vector<uint8_t>& script, Network net, std::string& address);

// A miner's share of the PPLNS window.
struct PayoutWeight
{
	std::vector<uint8_t> script; // P2MR output script
	uint64_t weight = 0;         // sum of the miner's share difficulties in the window (the sum of all weights must fit in 64 bits)
};

struct Payout
{
	std::vector<uint8_t> script;
	uint64_t value = 0;          // quanta
};

static constexpr size_t MAX_PAYOUTS = 500; // coinbase outputs per block (whitepaper / POOL.md)

// Split `total` quanta among the miners in proportion to their weight. Deterministic, so that every pool
// node builds the same coinbase: the same script appearing twice is merged; only the MAX_PAYOUTS heaviest
// miners are paid in this block (ties broken by script); each gets floor(total * weight / sum of the paid
// weights) and the rounding remainder goes to `finder` if it is paid, otherwise to the heaviest miner.
// Outputs are sorted by script. Empty if there is nothing to pay.
std::vector<Payout> split_reward(uint64_t total, const std::vector<PayoutWeight>& weights, const std::vector<uint8_t>& finder);

struct Coinbase
{
	std::vector<uint8_t> with_witness;     // serialization in the block
	std::vector<uint8_t> without_witness;  // serialization hashed for the txid
	hash txid;                             // internal byte order
};

// Build the coinbase. False if the inputs cannot make a valid coinbase (no payouts, outputs not summing to
// at most the template's coinbase value, scriptSig over 100 bytes).
bool make_coinbase(const pqchd::Template& t, const std::vector<Payout>& payouts, const hash& share_id, uint64_t extra_nonce, Coinbase& out);

// Bitcoin merkle root of txids (internal byte order), the coinbase first.
hash merkle_root(const std::vector<hash>& txids);

static constexpr size_t HEADER_SIZE = 80;
static constexpr size_t NONCE_OFFSET = 76;

void make_header(const pqchd::Template& t, const hash& merkle, uint32_t time, uint32_t nonce, uint8_t (&header)[HEADER_SIZE]);

// Header followed by the coinbase and the template's transactions.
std::vector<uint8_t> serialize_block(const uint8_t (&header)[HEADER_SIZE], const Coinbase& coinbase, const pqchd::Template& t);

// SHA256d of the header: the block id (internal byte order).
hash block_hash(const uint8_t (&header)[HEADER_SIZE]);

// pow_hash <= target, both little-endian 256-bit numbers (internal byte order).
bool check_pow(const hash& pow_hash, const hash& target);

} // namespace pqch
} // namespace p2pool
