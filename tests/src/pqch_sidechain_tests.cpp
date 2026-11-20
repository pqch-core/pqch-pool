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
#include "pqch_sidechain.h"
#include "gtest/gtest.h"
#include <openssl/sha2.h>

namespace p2pool {

namespace {

const std::vector<uint8_t> A(34, 0xaa), B(34, 0xbb), C(34, 0xcc);

hash sha256d(const std::vector<uint8_t>& v)
{
	hash a, b;
	SHA256(v.data(), v.size(), a.h);
	SHA256(a.h, HASH_SIZE, b.h);
	return b;
}

// A share on top of the chain that is valid except for what a test breaks. pow_hash = 0 meets any difficulty.
pqch::Share next_share(const pqch::SideChain& c, const std::vector<uint8_t>& miner, uint64_t timestamp)
{
	pqch::Share s;
	const pqch::Share* t = c.tip();
	s.parent = t ? t->id : hash();
	s.height = t ? t->height + 1 : 0;
	s.difficulty = c.next_difficulty();
	s.timestamp = timestamp;
	s.miner_script = miner;
	s.main_prev.h[0] = 0x42;
	s.main_height = 1000;
	memcpy(s.header + 4, s.main_prev.h, HASH_SIZE);
	s.id = sha256d(pqch::share_id_preimage(s.parent, s.height, s.difficulty, s.timestamp, s.miner_script, s.main_prev, s.main_height));
	return s;
}

pqch::SideChainParams small_params()
{
	pqch::SideChainParams p;
	p.share_time = 10;
	p.pplns_window = 4;
	p.difficulty_window = 5;
	p.min_difficulty = 1000;
	return p;
}

} // namespace

TEST(pqch_sidechain, add_and_reject)
{
	pqch::SideChain c(small_params());
	std::string error;
	EXPECT_EQ(c.next_difficulty(), 1000u);

	ASSERT_TRUE(c.add(next_share(c, A, 100), error)) << error;
	ASSERT_TRUE(c.add(next_share(c, B, 110), error)) << error;
	EXPECT_EQ(c.size(), 2u);
	EXPECT_EQ(c.tip()->height, 1u);

	pqch::Share s = next_share(c, A, 120);
	s.parent.h[0] ^= 1;
	EXPECT_FALSE(c.add(s, error)); EXPECT_EQ(error, "parent is not the tip");

	s = next_share(c, A, 120); s.height += 1;
	EXPECT_FALSE(c.add(s, error)); EXPECT_EQ(error, "wrong share height");

	s = next_share(c, A, 120); s.difficulty -= 1;
	EXPECT_FALSE(c.add(s, error)); EXPECT_EQ(error, "wrong share difficulty");

	s = next_share(c, A, 120); s.miner_script = C; // fields changed after the id was computed
	EXPECT_FALSE(c.add(s, error)); EXPECT_EQ(error, "share id does not match its fields");

	s = next_share(c, A, 120); s.header[4] ^= 1;
	EXPECT_FALSE(c.add(s, error)); EXPECT_EQ(error, "header does not build on main_prev");

	s = next_share(c, A, 120);
	memset(s.pow_hash.h, 0xff, HASH_SIZE); // the largest possible hash: no difficulty above 1 accepts it
	EXPECT_FALSE(c.add(s, error)); EXPECT_EQ(error, "not enough work for the share difficulty");

	s = next_share(c, A, 110 - 601);
	EXPECT_FALSE(c.add(s, error)); EXPECT_EQ(error, "timestamp too far in the past");

	EXPECT_EQ(c.size(), 2u); // nothing was added
}

TEST(pqch_sidechain, difficulty_follows_the_share_time)
{
	std::string error;

	// shares every 5 s instead of 10: the difficulty goes up (at most x2 per share)
	pqch::SideChain fast(small_params());
	uint64_t t = 1000;
	for (int i = 0; i < 12; ++i, t += 5) ASSERT_TRUE(fast.add(next_share(fast, A, t), error)) << error;
	EXPECT_GT(fast.next_difficulty(), 1000u);

	// shares every 40 s: it goes down, but never below the minimum
	pqch::SideChain slow(small_params());
	t = 1000;
	for (int i = 0; i < 12; ++i, t += 40) ASSERT_TRUE(slow.add(next_share(slow, A, t), error)) << error;
	EXPECT_EQ(slow.next_difficulty(), 1000u);

	// on time: stays where it is
	pqch::SideChainParams p = small_params();
	pqch::SideChain steady(p);
	t = 1000;
	for (int i = 0; i < 12; ++i, t += 10) ASSERT_TRUE(steady.add(next_share(steady, A, t), error)) << error;
	EXPECT_EQ(steady.next_difficulty(), 1000u);

	// never more than double the last share's difficulty, even with shares at the same second
	pqch::SideChain burst(small_params());
	for (int i = 0; i < 6; ++i) {
		const uint64_t before = burst.next_difficulty();
		ASSERT_TRUE(burst.add(next_share(burst, A, 5000), error)) << error;
		EXPECT_LE(burst.next_difficulty(), before * 2);
	}
}

TEST(pqch_sidechain, pplns_window)
{
	pqch::SideChain c(small_params()); // window of 4 shares
	std::string error;
	uint64_t t = 1000;
	for (const auto* m : { &A, &A, &B, &B, &B, &C }) {
		ASSERT_TRUE(c.add(next_share(c, *m, t += 10), error)) << error;
	}
	// a new share by A: the window is the last 3 shares of the chain (B, B, C) plus the new one (A)
	const std::vector<pqch::PayoutWeight> w = c.pplns_weights(A, 1000);
	ASSERT_EQ(w.size(), 4u);
	EXPECT_EQ(w[0].script, A);
	EXPECT_EQ(w[1].script, B);
	EXPECT_EQ(w[2].script, B);
	EXPECT_EQ(w[3].script, C);

	// split of 4000 among equal difficulties: A 1000, B 2000, C 1000
	const std::vector<pqch::Payout> p = pqch::split_reward(4000, w, A);
	ASSERT_EQ(p.size(), 3u);
	EXPECT_EQ(p[0].script, A); EXPECT_EQ(p[0].value, 1000u);
	EXPECT_EQ(p[1].script, B); EXPECT_EQ(p[1].value, 2000u);
	EXPECT_EQ(p[2].script, C); EXPECT_EQ(p[2].value, 1000u);

	// old shares are dropped, but enough are kept for both windows
	for (int i = 0; i < 20; ++i) ASSERT_TRUE(c.add(next_share(c, A, t += 10), error)) << error;
	EXPECT_EQ(c.size(), 4u + 5u);
}

TEST(pqch_sidechain, work_commits_to_the_share)
{
	pqch::SideChain c(small_params());
	std::string error;
	ASSERT_TRUE(c.add(next_share(c, B, 1000), error)) << error;

	pqchd::Template t;
	t.version = 0x20000000;
	t.prev_hash.h[0] = 0x42;
	t.height = 1000;
	t.bits = 0x207fffff;
	t.curtime = t.mintime = 2000;
	t.coinbase_value = 700000000;
	t.witness_commitment = { 0x6a, 0x24, 0xaa, 0x21, 0xa9, 0xed };
	t.witness_commitment.resize(38, 0);

	pqch::Work w;
	ASSERT_TRUE(pqch::make_work(c, t, A, 7, 1, 2001, w));
	EXPECT_EQ(w.parent, c.tip()->id);
	EXPECT_EQ(w.share_height, 1u);
	EXPECT_EQ(w.timestamp, 2001u);
	// the coinbase carries the share id and pays the window (B's share and A's new one, same difficulty)
	EXPECT_NE(std::search(w.coinbase.without_witness.begin(), w.coinbase.without_witness.end(), w.share_id.h, w.share_id.h + HASH_SIZE), w.coinbase.without_witness.end());
	ASSERT_EQ(w.payouts.size(), 2u);
	EXPECT_EQ(w.payouts[0].value + w.payouts[1].value, 700000000u);
	EXPECT_EQ(w.payouts[0].value, 350000000u);

	// a share made from the work is accepted by the chain
	uint8_t header[pqch::HEADER_SIZE];
	pqch::make_header(t, w.merkle_root, static_cast<uint32_t>(w.timestamp), 0, header);
	ASSERT_TRUE(c.add(pqch::make_share(w, header, hash()), error)) << error;
	EXPECT_EQ(c.tip()->id, w.share_id);

	// time never goes back: a template time before the tip's keeps the tip's time
	pqch::Work w2;
	ASSERT_TRUE(pqch::make_work(c, t, A, 7, 2, 1500, w2));
	EXPECT_EQ(w2.timestamp, 2001u);
}

} // namespace p2pool
