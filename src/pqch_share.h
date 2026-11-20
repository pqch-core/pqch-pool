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

// PQCH: a share as it travels between pool nodes, and the check of a share that comes from another node.
//
// A share carries its header, its fields, its coinbase (without witness) and the merkle branch that links the
// coinbase to the header's merkle root, so a node can check it without the block's other transactions:
//   - the share id is the hash of its fields, and the coinbase scriptSig holds BIP34 height | id | extranonce;
//   - the coinbase is in the block: merkle root from its txid and the branch == the header's;
//   - the coinbase pays the PPLNS split of this node's share tree (the share's branch), whatever its total:
//     a node cannot pay itself more than its share of the work. (The total, subsidy + fees, is pqchd's to
//     check when the share is also a block; paying less only hurts whoever built the block.)
//   - the proof of work (RandomX v2 of the header, keyed by the main chain) is checked by the caller.

#include "pqch_sidechain.h"

namespace p2pool {
namespace pqch {

struct ShareMessage
{
	Share share;                          // pow_hash is not sent: the receiver computes it
	std::vector<uint8_t> coinbase;        // serialization without witness
	std::vector<hash> merkle_branch;      // siblings from the coinbase (index 0) up to the root
};

static constexpr size_t MAX_SHARE_MESSAGE_SIZE = 64 * 1024;
static constexpr size_t MAX_MERKLE_BRANCH = 24;           // blocks of up to 16 million transactions
static constexpr size_t MAX_MINER_SCRIPT = 64;

std::vector<uint8_t> serialize(const ShareMessage& m);

// Parse a share message; fills the share's id (from its fields) and main_prev (from the header).
bool deserialize(const uint8_t* data, size_t size, ShareMessage& m, std::string& error);

// Merkle branch of the first transaction (the coinbase) in a list of txids.
std::vector<hash> coinbase_merkle_branch(const std::vector<hash>& txids);
hash merkle_root_from_branch(const hash& coinbase_txid, const std::vector<hash>& branch);

// The share message for a share mined on `w` (own shares, before sending them to the other nodes).
ShareMessage make_share_message(const Work& w, const Share& s);

// Check a share message's coinbase against `chain` (see above). The share's parent must be in the chain.
bool verify_share_coinbase(const SideChain& chain, const ShareMessage& m, std::string& error);

} // namespace pqch
} // namespace p2pool
