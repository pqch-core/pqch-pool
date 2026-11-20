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
#include "pqch_share.h"
#include "gtest/gtest.h"
#include <openssl/sha2.h>

namespace p2pool {

namespace {

const std::vector<uint8_t> A = [] { std::vector<uint8_t> s(34, 0xaa); s[0] = 0x51; s[1] = 0x20; return s; }();
const std::vector<uint8_t> B = [] { std::vector<uint8_t> s(34, 0xbb); s[0] = 0x51; s[1] = 0x20; return s; }();

hash h256(uint8_t b) { hash h; memset(h.h, b, HASH_SIZE); return h; }

pqchd::Template make_template(size_t txs)
{
	pqchd::Template t;
	t.version = 0x20000000;
	t.prev_hash = h256(0x42);
	t.height = 5000;
	t.bits = 0x207fffff;
	t.curtime = t.mintime = 2000;
	t.coinbase_value = 700000123;
	t.witness_commitment = { 0x6a, 0x24, 0xaa, 0x21, 0xa9, 0xed };
	t.witness_commitment.resize(38, 0x11);
	for (size_t i = 0; i < txs; ++i) {
		pqchd::TemplateTx x;
		x.txid = h256(static_cast<uint8_t>(0x60 + i));
		x.data = { 1, 2, 3 };
		t.transactions.push_back(x);
	}
	return t;
}

// A chain where B has a share, then a share by A on top of it, as node "A" would send it.
struct Fixture
{
	pqch::SideChain chain{ [] { pqch::SideChainParams p; p.min_difficulty = 1000; p.pplns_window = 10; return p; }() };
	pqch::Work work;
	pqch::ShareMessage msg;

	explicit Fixture(size_t txs)
	{
		std::string error;
		const pqchd::Template t = make_template(txs);
		pqch::Work wb;
		EXPECT_TRUE(pqch::make_work(chain, t, B, 0, 1, 2000, wb));
		uint8_t hb[pqch::HEADER_SIZE];
		pqch::make_header(t, wb.merkle_root, static_cast<uint32_t>(wb.timestamp), 0, hb);
		EXPECT_TRUE(chain.add(pqch::make_share(wb, hb, hash()), error)) << error;

		EXPECT_TRUE(pqch::make_work(chain, t, A, 7, 2, 2010, work));
		uint8_t ha[pqch::HEADER_SIZE];
		pqch::make_header(t, work.merkle_root, static_cast<uint32_t>(work.timestamp), 99, ha);
		msg = pqch::make_share_message(work, pqch::make_share(work, ha, hash()));
	}
};

// Replace the coinbase and fix the header's merkle root so that only the coinbase check is tested.
void set_coinbase(pqch::ShareMessage& m, const std::vector<uint8_t>& cb)
{
	m.coinbase = cb;
	hash a, txid;
	SHA256(cb.data(), cb.size(), a.h);
	SHA256(a.h, HASH_SIZE, txid.h);
	const hash root = pqch::merkle_root_from_branch(txid, m.merkle_branch);
	memcpy(m.share.header + 36, root.h, HASH_SIZE);
}

} // namespace

TEST(pqch_share, merkle_branch)
{
	for (size_t n : { 1u, 2u, 3u, 4u, 5u, 7u, 8u, 13u }) {
		std::vector<hash> txids;
		for (size_t i = 0; i < n; ++i) txids.push_back(h256(static_cast<uint8_t>(i + 1)));
		const std::vector<hash> branch = pqch::coinbase_merkle_branch(txids);
		EXPECT_EQ(pqch::merkle_root_from_branch(txids[0], branch), pqch::merkle_root(txids)) << n;
	}
}

TEST(pqch_share, round_trip_and_verify)
{
	for (size_t txs : { 0u, 1u, 6u }) {
		Fixture f(txs);
		std::string error;
		const std::vector<uint8_t> bytes = pqch::serialize(f.msg);

		pqch::ShareMessage m;
		ASSERT_TRUE(pqch::deserialize(bytes.data(), bytes.size(), m, error)) << error;
		EXPECT_EQ(m.share.id, f.msg.share.id);         // recomputed from the fields
		EXPECT_EQ(m.share.main_prev, h256(0x42));       // taken from the header
		EXPECT_EQ(m.coinbase, f.msg.coinbase);
		EXPECT_EQ(m.merkle_branch.size(), f.msg.merkle_branch.size());

		EXPECT_TRUE(pqch::verify_share_coinbase(f.chain, m, error)) << error;
		EXPECT_TRUE(f.chain.add(m.share, error)) << error; // pow_hash 0 meets the difficulty
	}
}

TEST(pqch_share, cheating_is_refused)
{
	Fixture f(3);
	std::string error;
	const std::vector<uint8_t> cb = f.msg.coinbase;

	// the sharer moves 1000 quanta from B to itself
	{
		pqch::ShareMessage m = f.msg;
		std::vector<uint8_t> x = cb;
		// outputs are sorted by script: A (0xaa..) first, then B; values are 8-byte LE before each script
		const size_t first_value = 4 + 1 + 36 + 1 + 45 + 4 + 1; // scriptSig: 3-byte height push, 33-byte id push, 9-byte extranonce push
		uint64_t a = 0, b = 0;
		for (int i = 0; i < 8; ++i) a |= static_cast<uint64_t>(x[first_value + i]) << (8 * i);
		const size_t second_value = first_value + 8 + 1 + 34;
		for (int i = 0; i < 8; ++i) b |= static_cast<uint64_t>(x[second_value + i]) << (8 * i);
		a += 1000; b -= 1000;
		for (int i = 0; i < 8; ++i) { x[first_value + i] = static_cast<uint8_t>(a >> (8 * i)); x[second_value + i] = static_cast<uint8_t>(b >> (8 * i)); }
		set_coinbase(m, x);
		EXPECT_FALSE(pqch::verify_share_coinbase(f.chain, m, error));
		EXPECT_EQ(error, "coinbase does not pay the PPLNS split");
	}

	// the coinbase commits to another share id
	{
		pqch::ShareMessage m = f.msg;
		std::vector<uint8_t> x = cb;
		x[4 + 1 + 36 + 1 + 4] ^= 1; // first byte of the id in the scriptSig (after the 3-byte height push)
		set_coinbase(m, x);
		EXPECT_FALSE(pqch::verify_share_coinbase(f.chain, m, error));
		EXPECT_EQ(error, "coinbase: scriptSig does not commit to this share");
	}

	// a coinbase that is not in the block
	{
		pqch::ShareMessage m = f.msg;
		m.merkle_branch[0].h[0] ^= 1;
		EXPECT_FALSE(pqch::verify_share_coinbase(f.chain, m, error));
		EXPECT_EQ(error, "coinbase not in the block (merkle branch)");
	}

	// the miner field says B but the coinbase was built for A: the id no longer matches the scriptSig
	{
		std::vector<uint8_t> bytes = pqch::serialize(f.msg);
		const size_t script_at = 1 + 80 + 32 + 32 + 1;
		bytes[script_at + 2] = 0xbb;
		pqch::ShareMessage m;
		ASSERT_TRUE(pqch::deserialize(bytes.data(), bytes.size(), m, error));
		EXPECT_FALSE(pqch::verify_share_coinbase(f.chain, m, error));
	}

	// malformed messages
	{
		const std::vector<uint8_t> bytes = pqch::serialize(f.msg);
		pqch::ShareMessage m;
		EXPECT_FALSE(pqch::deserialize(bytes.data(), bytes.size() - 1, m, error));
		std::vector<uint8_t> longer = bytes; longer.push_back(0);
		EXPECT_FALSE(pqch::deserialize(longer.data(), longer.size(), m, error));
		EXPECT_EQ(error, "trailing bytes in share message");
		std::vector<uint8_t> v2 = bytes; v2[0] = 2;
		EXPECT_FALSE(pqch::deserialize(v2.data(), v2.size(), m, error));
		std::vector<uint8_t> huge(pqch::MAX_SHARE_MESSAGE_SIZE + 1, 1);
		EXPECT_FALSE(pqch::deserialize(huge.data(), huge.size(), m, error));
		EXPECT_EQ(error, "share message too large");
	}

	// unknown parent
	{
		pqch::SideChain empty(f.chain.params());
		EXPECT_FALSE(pqch::verify_share_coinbase(empty, f.msg, error));
		EXPECT_EQ(error, "unknown parent");
	}
}

} // namespace p2pool
