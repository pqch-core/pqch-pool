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

// PQCH share chain (sidechain), with P2Pool's rules:
//
// - Miners find "shares": PQCH block headers that meet the share difficulty, much lower than the main
//   chain's. A share whose header also meets the main target is a real PQCH block.
// - Each share names its parent share, so the shares form a chain. The share id is committed in the
//   coinbase scriptSig of its block, and the coinbase pays the PPLNS window that ends with the share
//   itself: the last PPLNS_WINDOW shares (this one included), each miner in proportion to the sum of its
//   share difficulties. So every PQCH block found by the pool pays all recent miners directly, and any
//   pool node can check a share by recomputing its coinbase from the chain.
// - The share difficulty follows the recent shares so that one is found every SHARE_TIME seconds.
//
// Phase A runs one pool node; the gossip between nodes (and uncles) comes with phase B.

#include "pqch_block.h"
#include <deque>
#include <map>

namespace p2pool {
namespace pqch {

struct SideChainParams
{
	uint64_t share_time = 10;             // seconds per share
	uint64_t pplns_window = 2160;         // shares (6 hours at 10 s)
	uint64_t difficulty_window = 60;      // shares used to retarget
	uint64_t min_difficulty = 100000;     // never below this (regtest/tests: lower)
};

struct Share
{
	hash id;                              // SHA256d of the fields below (share_id_preimage)
	hash parent;                          // previous share id (zero for the first share)
	uint64_t height = 0;                  // share chain height (0 for the first share)
	uint64_t difficulty = 0;              // share difficulty this share had to meet
	uint64_t timestamp = 0;               // block header time
	std::vector<uint8_t> miner_script;    // P2MR output script of the miner
	hash main_prev;                       // main-chain block it builds on
	uint64_t main_height = 0;             // main-chain height of its block
	uint8_t header[HEADER_SIZE] = {};     // the mined header
	hash pow_hash;                        // RandomX v2 of the header
};

// What the id of a share commits to (everything except the header, which commits to the id itself).
std::vector<uint8_t> share_id_preimage(const hash& parent, uint64_t height, uint64_t difficulty, uint64_t timestamp,
	const std::vector<uint8_t>& miner_script, const hash& main_prev, uint64_t main_height);

class SideChain
{
public:
	explicit SideChain(const SideChainParams& params) : m_params(params) {}

	const SideChainParams& params() const { return m_params; }

	// Tip of the chain, or nullptr if there are no shares yet.
	const Share* tip() const { return m_chain.empty() ? nullptr : &m_chain.back(); }
	uint64_t size() const { return m_chain.size(); }

	// Difficulty the next share (on top of the tip) must meet.
	uint64_t next_difficulty() const;

	// PPLNS weights of a new share by `miner_script` with difficulty `difficulty` on top of the tip:
	// the last pplns_window - 1 shares of the chain plus the new share.
	std::vector<PayoutWeight> pplns_weights(const std::vector<uint8_t>& miner_script, uint64_t difficulty) const;

	// Check a share and append it: parent = tip, height, difficulty = next_difficulty(), id matches its
	// fields, the header's PoW meets the difficulty. (The coinbase is checked by whoever built the block.)
	bool add(const Share& share, std::string& error);

	// The shares from the oldest kept to the tip (at most pplns_window + difficulty_window).
	const std::deque<Share>& shares() const { return m_chain; }

private:
	SideChainParams m_params;
	std::deque<Share> m_chain;
};

// One unit of mining work: everything a miner thread needs to hash and to turn a winning header into a share
// (and, if it also meets the main target, into a PQCH block).
struct Work
{
	uint64_t id = 0;                      // changes when the work changes
	pqchd::Template tmpl;
	std::vector<uint8_t> miner_script;
	hash parent;
	uint64_t share_height = 0;
	uint64_t share_difficulty = 0;
	uint64_t timestamp = 0;
	hash share_id;
	std::vector<Payout> payouts;
	Coinbase coinbase;
	hash merkle_root;
};

// Build the work for a miner on top of the chain and a main-chain template. False if the coinbase cannot
// be built (e.g. more than the template's coinbase value).
// `now` is the current time: the share's (and header's) time is now, but never before the template's mintime
// nor before the parent share.
bool make_work(const SideChain& chain, const pqchd::Template& t, const std::vector<uint8_t>& miner_script,
	uint64_t extra_nonce, uint64_t id, uint64_t now, Work& out);

// The share for a header (with nonce) mined on `w`.
Share make_share(const Work& w, const uint8_t (&header)[HEADER_SIZE], const hash& pow_hash);

} // namespace pqch
} // namespace p2pool
