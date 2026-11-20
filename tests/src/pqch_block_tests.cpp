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
#include "pqch_block.h"
#include "gtest/gtest.h"
#include <fstream>
#include <sstream>

namespace p2pool {

namespace {

std::vector<uint8_t> bytes(const char* h)
{
	std::vector<uint8_t> v;
	EXPECT_TRUE(from_hex(h, strlen(h), v));
	return v;
}

hash rpc_hash(const char* h) // RPC hex (big-endian) -> internal order
{
	hash x, r;
	EXPECT_TRUE(from_hex(h, 64, x));
	for (size_t i = 0; i < HASH_SIZE; ++i) r.h[i] = x.h[HASH_SIZE - 1 - i];
	return r;
}

pqchd::Template fixture()
{
	std::ifstream f("pqchd_getblocktemplate.json");
	std::stringstream s;
	s << R"({"result":)" << f.rdbuf() << R"(,"error":null})";
	const std::string doc = s.str();
	pqchd::Template t;
	std::string error;
	EXPECT_TRUE(pqchd::parse_getblocktemplate(doc.data(), doc.size(), t, error)) << error;
	return t;
}

} // namespace

// Addresses and scripts produced by pqchd (getnewaddress / getaddressinfo)
TEST(pqch_block, p2mr_addresses)
{
	struct { const char* address; pqch::Network net; const char* script; } v[] = {
		{ "pqcrt1pfyplwrzcd4sdqtgcqatgu73axaxx08llupvjr89lagz0q6wk57uqsp8mde", pqch::Network::REGTEST, "51204903f70c586d60d02d1807568e7a3d374c679fffe059219cbfea04f069d6a7b8" },
		{ "pqcrt1pr07sks6xzdfsq6dzw79qjca72k4cfwgfy9qw07hxw6t8e76gst8sn3xscq", pqch::Network::REGTEST, "51201bfd0b434613530069a2778a0963be55ab84b9092140e7fae676967cfb4882cf" },
		{ "pqcrt1pgg8k8ynd7prk3ugfjh7a03sfzquervanvyc9nqg72cmv4ks8502s7s7xha", pqch::Network::REGTEST, "5120420f63926df04768f10995fdd7c609103991b3b3613059811e5636cada07a3d5" },
		{ "pqct1pv9xfdwwan78dhkkz73766lcmkqg2n25w3rmagz7x0e6qddnuvjuqnvtk8z", pqch::Network::TEST, "5120614c96b9dd9f8edbdac2f47dad7f1bb010a9aa8e88f7d40bc67e7406b67c64b8" },
	};
	for (const auto& x : v) {
		std::vector<uint8_t> script;
		ASSERT_TRUE(pqch::p2mr_script_from_address(x.address, x.net, script)) << x.address;
		EXPECT_EQ(script, bytes(x.script)) << x.address;
		std::string a;
		ASSERT_TRUE(pqch::address_from_p2mr_script(script, x.net, a));
		EXPECT_EQ(a, x.address);
	}

	// round trip on mainnet
	std::vector<uint8_t> s = bytes(v[0].script), s2;
	std::string main_addr;
	ASSERT_TRUE(pqch::address_from_p2mr_script(s, pqch::Network::MAIN, main_addr));
	EXPECT_EQ(main_addr.substr(0, 5), "pqc1p");
	ASSERT_TRUE(pqch::p2mr_script_from_address(main_addr, pqch::Network::MAIN, s2));
	EXPECT_EQ(s, s2);

	std::vector<uint8_t> script;
	// another network's address
	EXPECT_FALSE(pqch::p2mr_script_from_address(v[0].address, pqch::Network::MAIN, script));
	EXPECT_FALSE(pqch::p2mr_script_from_address(v[3].address, pqch::Network::REGTEST, script));
	EXPECT_FALSE(pqch::p2mr_script_from_address(main_addr, pqch::Network::TEST, script));
	// one changed character (checksum)
	std::string bad = v[0].address;
	bad[20] = (bad[20] == 'q') ? 'p' : 'q';
	EXPECT_FALSE(pqch::p2mr_script_from_address(bad, pqch::Network::REGTEST, script));
	// mixed case
	std::string mixed = v[0].address;
	mixed[10] = static_cast<char>(toupper(mixed[10]));
	EXPECT_FALSE(pqch::p2mr_script_from_address(mixed, pqch::Network::REGTEST, script));
	// all upper case is valid
	std::string upper = v[0].address;
	for (char& c : upper) c = static_cast<char>(toupper(c));
	EXPECT_TRUE(pqch::p2mr_script_from_address(upper, pqch::Network::REGTEST, script));
	// not a P2MR script
	std::string a;
	EXPECT_FALSE(pqch::address_from_p2mr_script(bytes("0014751e76e8199196d454941c45d1b3a323f1433bd6"), pqch::Network::MAIN, a));
	EXPECT_FALSE(pqch::p2mr_script_from_address("", pqch::Network::MAIN, script));
	EXPECT_FALSE(pqch::p2mr_script_from_address("random text", pqch::Network::MAIN, script));
}

TEST(pqch_block, split_reward)
{
	const std::vector<uint8_t> a(34, 0xaa), b(34, 0xbb), c(34, 0xcc);

	// 1:2:1 of 1000 -> 250, 500, 250
	std::vector<pqch::Payout> p = pqch::split_reward(1000, { { a, 1 }, { b, 2 }, { c, 1 } }, a);
	ASSERT_EQ(p.size(), 3u);
	EXPECT_EQ(p[0].script, a); EXPECT_EQ(p[0].value, 250u);
	EXPECT_EQ(p[1].script, b); EXPECT_EQ(p[1].value, 500u);
	EXPECT_EQ(p[2].script, c); EXPECT_EQ(p[2].value, 250u);

	// rounding remainder goes to the finder; the sum is always the total
	p = pqch::split_reward(1001, { { a, 1 }, { b, 1 }, { c, 1 } }, c);
	ASSERT_EQ(p.size(), 3u);
	EXPECT_EQ(p[0].value, 333u);
	EXPECT_EQ(p[1].value, 333u);
	EXPECT_EQ(p[2].value, 335u);

	// same script twice is merged; order of the input does not matter
	std::vector<pqch::Payout> p1 = pqch::split_reward(900, { { a, 1 }, { b, 1 }, { a, 1 } }, b);
	std::vector<pqch::Payout> p2 = pqch::split_reward(900, { { b, 1 }, { a, 2 } }, b);
	ASSERT_EQ(p1.size(), 2u);
	EXPECT_EQ(p1[0].value, 600u);
	EXPECT_EQ(p1[1].value, 300u);
	EXPECT_EQ(p1[0].script, p2[0].script); EXPECT_EQ(p1[0].value, p2[0].value);
	EXPECT_EQ(p1[1].script, p2[1].script); EXPECT_EQ(p1[1].value, p2[1].value);

	// large numbers: a full block reward and 64-bit weights do not overflow
	p = pqch::split_reward(700000000ull, { { a, 0xffffffffffull }, { b, 0xffffffffffull * 3 } }, a);
	ASSERT_EQ(p.size(), 2u);
	EXPECT_EQ(p[0].value + p[1].value, 700000000ull);
	EXPECT_EQ(p[1].value, 525000000ull);

	// no weight -> nothing
	EXPECT_TRUE(pqch::split_reward(1000, {}, a).empty());
	EXPECT_TRUE(pqch::split_reward(1000, { { a, 0 } }, a).empty());

	// more than MAX_PAYOUTS miners: only the heaviest are paid, all of the total still goes out
	std::vector<pqch::PayoutWeight> many;
	for (uint32_t i = 0; i < pqch::MAX_PAYOUTS + 50; ++i) {
		std::vector<uint8_t> s(34, 0);
		s[0] = 0x51; s[1] = 0x20; s[2] = static_cast<uint8_t>(i >> 8); s[3] = static_cast<uint8_t>(i);
		many.push_back({ s, 1000 + i });
	}
	p = pqch::split_reward(1250000000ull, many, many[0].script);
	EXPECT_EQ(p.size(), pqch::MAX_PAYOUTS);
	uint64_t sum = 0;
	for (const auto& x : p) {
		sum += x.value;
		EXPECT_NE(x.script, many[0].script); // the 50 lightest are left for later blocks
	}
	EXPECT_EQ(sum, 1250000000ull);
}

// Bitcoin block 100000: 4 transactions, well-known merkle root
TEST(pqch_block, merkle_root)
{
	const std::vector<hash> txids = {
		rpc_hash("8c14f0db3df150123e6f3dbbf30f8b955a8249b62ac1d1ff16284aefa3d06d87"),
		rpc_hash("fff2525b8931402dd09222c50775608f75787bd2b87e56995a7bdd30f79702c4"),
		rpc_hash("6359f0868171b1d194cbee1af2f16ea598ae8fad666d9b012c8ed2b79a236ec4"),
		rpc_hash("e9a66845e05d5abc0ad04ec80f774a7e585c6e8db975962d069a522137b80c1d"),
	};
	EXPECT_EQ(pqch::merkle_root(txids), rpc_hash("f3e94742aca4b5ef85488dc37c06c3282295ffec960994b2c0d5ac2a25a95766"));
	// one transaction: the root is its txid; three: the last is paired with itself
	EXPECT_EQ(pqch::merkle_root({ txids[0] }), txids[0]);
	EXPECT_EQ(pqch::merkle_root({ txids[0], txids[1], txids[2] }), pqch::merkle_root({ txids[0], txids[1], txids[2], txids[2] }));
}

TEST(pqch_block, coinbase_and_block)
{
	const pqchd::Template t = fixture();
	ASSERT_EQ(t.height, 111u);

	const std::vector<uint8_t> a = bytes("51204903f70c586d60d02d1807568e7a3d374c679fffe059219cbfea04f069d6a7b8");
	const std::vector<uint8_t> b = bytes("51201bfd0b434613530069a2778a0963be55ab84b9092140e7fae676967cfb4882cf");
	const std::vector<pqch::Payout> payouts = pqch::split_reward(t.coinbase_value, { { a, 2 }, { b, 1 } }, a);

	hash share_id;
	for (size_t i = 0; i < HASH_SIZE; ++i) share_id.h[i] = static_cast<uint8_t>(i);

	pqch::Coinbase cb;
	ASSERT_TRUE(pqch::make_coinbase(t, payouts, share_id, 0x0102030405060708ull, cb));

	// version 2, then (with witness) marker and flag
	EXPECT_EQ(cb.without_witness[0], 2);
	EXPECT_EQ(cb.with_witness[4], 0x00);
	EXPECT_EQ(cb.with_witness[5], 0x01);
	// scriptSig: 01 6f (height 111) | 20 <share id> | 08 <extranonce LE>  = 44 bytes
	const size_t ss = 4 + 1 + 32 + 4; // version, input count, prevout hash, prevout index
	EXPECT_EQ(cb.without_witness[ss], 44);
	EXPECT_EQ(cb.without_witness[ss + 1], 0x01);
	EXPECT_EQ(cb.without_witness[ss + 2], 111);
	EXPECT_EQ(cb.without_witness[ss + 3], 0x20);
	EXPECT_EQ(memcmp(&cb.without_witness[ss + 4], share_id.h, HASH_SIZE), 0);
	EXPECT_EQ(cb.without_witness[ss + 36], 0x08);
	EXPECT_EQ(cb.without_witness[ss + 37], 0x08); // extranonce, little-endian
	// nLockTime = height - 1
	EXPECT_EQ(cb.without_witness[cb.without_witness.size() - 4], 110);
	// the witness serialization is the other plus marker, flag and a 1-item witness of 32 zero bytes
	EXPECT_EQ(cb.with_witness.size(), cb.without_witness.size() + 2 + 1 + 1 + 32);

	// too much paid, or nothing paid
	std::vector<pqch::Payout> over = payouts;
	over[0].value += 1;
	pqch::Coinbase bad;
	EXPECT_FALSE(pqch::make_coinbase(t, over, share_id, 0, bad));
	EXPECT_FALSE(pqch::make_coinbase(t, {}, share_id, 0, bad));

	// merkle root over the coinbase and the template's transaction
	std::vector<hash> txids{ cb.txid };
	for (const auto& tx : t.transactions) txids.push_back(tx.txid);
	const hash root = pqch::merkle_root(txids);

	uint8_t header[pqch::HEADER_SIZE];
	pqch::make_header(t, root, static_cast<uint32_t>(t.curtime), 0x11223344, header);
	EXPECT_EQ(memcmp(header + 4, t.prev_hash.h, HASH_SIZE), 0);
	EXPECT_EQ(memcmp(header + 36, root.h, HASH_SIZE), 0);
	EXPECT_EQ(header[72], 0xff); // bits 207fffff, little-endian
	EXPECT_EQ(header[75], 0x20);
	EXPECT_EQ(header[pqch::NONCE_OFFSET], 0x44);
	EXPECT_EQ(header[pqch::NONCE_OFFSET + 3], 0x11);

	const std::vector<uint8_t> block = pqch::serialize_block(header, cb, t);
	EXPECT_EQ(block.size(), pqch::HEADER_SIZE + 1 + cb.with_witness.size() + t.transactions[0].data.size());
	EXPECT_EQ(block[pqch::HEADER_SIZE], 2); // transaction count
}

TEST(pqch_block, check_pow)
{
	hash target, h;
	target.h[31] = 0x10; // 0x10 << 248
	EXPECT_TRUE(pqch::check_pow(h, target));       // 0 <= target
	h.h[31] = 0x10;
	EXPECT_TRUE(pqch::check_pow(h, target));       // equal
	h.h[0] = 1;
	EXPECT_FALSE(pqch::check_pow(h, target));      // target + 1
	h = hash();
	h.h[30] = 0xff;
	EXPECT_TRUE(pqch::check_pow(h, target));       // lower high byte wins over the rest
}

} // namespace p2pool
