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
#include "pqchd.h"
#include "gtest/gtest.h"
#include <cstdio>
#include <fstream>
#include <sstream>

namespace p2pool {

namespace {

// getblocktemplate result captured from pqchd (regtest, height 111, one mempool transaction)
std::string fixture_result()
{
	std::ifstream f("pqchd_getblocktemplate.json");
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

std::string wrap(const std::string& result)
{
	return R"({"result":)" + result + R"(,"error":null,"id":"pqch-pool"})";
}

std::string replace(std::string s, const std::string& from, const std::string& to)
{
	const size_t pos = s.find(from);
	EXPECT_NE(pos, std::string::npos) << from;
	if (pos != std::string::npos) {
		s.replace(pos, from.size(), to);
	}
	return s;
}

std::string hex(const hash& h)
{
	static constexpr char d[] = "0123456789abcdef";
	std::string s;
	for (uint8_t b : h.h) { s += d[b >> 4]; s += d[b & 15]; }
	return s;
}

} // namespace

TEST(pqchd, parse_getblocktemplate)
{
	const std::string result = fixture_result();
	ASSERT_FALSE(result.empty());
	const std::string doc = wrap(result);

	pqchd::Template t;
	std::string error;
	ASSERT_TRUE(pqchd::parse_getblocktemplate(doc.data(), doc.size(), t, error)) << error;

	EXPECT_EQ(t.version, 536870912);
	EXPECT_EQ(t.height, 111u);
	EXPECT_EQ(t.bits, 0x207fffffu);
	EXPECT_EQ(t.coinbase_value, 1250011070u); // 12.5 PQCH regtest subsidy + 11,070 quanta of fees
	EXPECT_EQ(t.seed_height, 0u);
	EXPECT_EQ(t.weight_limit, 2000000u);
	EXPECT_EQ(t.curtime, 1795197218u);
	EXPECT_GE(t.curtime, t.mintime);

	// target 7fffff00..00 (big-endian in the RPC) -> internal little-endian: last byte 0x7f
	EXPECT_EQ(t.target.h[31], 0x7f);
	EXPECT_EQ(t.target.h[30], 0xff);
	EXPECT_EQ(t.target.h[0], 0x00);

	// hashes are reversed into internal byte order
	{
		hash shown;
		const size_t p = result.find(R"("previousblockhash":")") + 21;
		ASSERT_TRUE(from_hex(result.c_str() + p, 64, shown));
		for (size_t i = 0; i < HASH_SIZE; ++i) {
			EXPECT_EQ(t.prev_hash.h[i], shown.h[HASH_SIZE - 1 - i]);
		}
	}

	// SegWit commitment output script: OP_RETURN, push 36, aa21a9ed, 32 bytes
	ASSERT_EQ(t.witness_commitment.size(), 38u);
	EXPECT_EQ(t.witness_commitment[0], 0x6a);
	EXPECT_EQ(t.witness_commitment[1], 0x24);
	EXPECT_EQ(t.witness_commitment[2], 0xaa);
	EXPECT_EQ(t.witness_commitment[5], 0xed);

	ASSERT_EQ(t.transactions.size(), 1u);
	const pqchd::TemplateTx& tx = t.transactions[0];
	EXPECT_EQ(tx.data.size(), 4015u);
	EXPECT_EQ(tx.fee, 11070u);
	EXPECT_EQ(tx.weight, 4426u);
	EXPECT_TRUE(tx.depends.empty());
	EXPECT_NE(hex(tx.txid), hex(tx.wtxid)); // a P2MR spend has witness data
	EXPECT_FALSE(t.longpollid.empty());
}

TEST(pqchd, parse_getblocktemplate_errors)
{
	pqchd::Template t;
	std::string error;

	const std::string rpc_err = R"({"result":null,"error":{"code":-10,"message":"PQCH is in initial sync and waiting for blocks..."},"id":"pqch-pool"})";
	EXPECT_FALSE(pqchd::parse_getblocktemplate(rpc_err.data(), rpc_err.size(), t, error));
	EXPECT_NE(error.find("initial sync"), std::string::npos);
	EXPECT_NE(error.find("(-10)"), std::string::npos);

	const std::string junk = "not json";
	EXPECT_FALSE(pqchd::parse_getblocktemplate(junk.data(), junk.size(), t, error));

	const std::string result = fixture_result();

	// A node without the RandomX key is not a PQCH node: refuse its template.
	const size_t a = result.find(R"("randomx_seed_hash")");
	ASSERT_NE(a, std::string::npos);
	const size_t b = result.find(',', a);
	const std::string no_seed = wrap(result.substr(0, a) + result.substr(b + 1));
	EXPECT_FALSE(pqchd::parse_getblocktemplate(no_seed.data(), no_seed.size(), t, error));
	EXPECT_NE(error.find("randomx_seed_hash"), std::string::npos);

	// A seed height at or above the block height is impossible.
	const std::string bad_seed = wrap(replace(result, R"("randomx_seed_height":0)", R"("randomx_seed_height":111)"));
	EXPECT_FALSE(pqchd::parse_getblocktemplate(bad_seed.data(), bad_seed.size(), t, error));

	// A transaction may only depend on an earlier one.
	const std::string bad_dep = wrap(replace(result, R"("depends":[])", R"("depends":[1])"));
	EXPECT_FALSE(pqchd::parse_getblocktemplate(bad_dep.data(), bad_dep.size(), t, error));
	EXPECT_NE(error.find("depends"), std::string::npos);

	// Wrong-length hash
	const std::string bad_prev = wrap(replace(result, R"("previousblockhash":")", R"("previousblockhash":"00)"));
	EXPECT_FALSE(pqchd::parse_getblocktemplate(bad_prev.data(), bad_prev.size(), t, error));
}

TEST(pqchd, submitblock)
{
	const uint8_t block[] = { 0x00, 0x01, 0xab, 0xff };
	EXPECT_EQ(pqchd::submitblock_request(block, sizeof(block)), R"({"jsonrpc":"1.0","id":"pqch-pool","method":"submitblock","params":["0001abff"]})");

	std::string reason;
	const std::string ok = R"({"result":null,"error":null,"id":"pqch-pool"})";
	EXPECT_TRUE(pqchd::parse_submitblock(ok.data(), ok.size(), reason));
	EXPECT_TRUE(reason.empty());

	const std::string rejected = R"({"result":"high-hash","error":null,"id":"pqch-pool"})";
	EXPECT_FALSE(pqchd::parse_submitblock(rejected.data(), rejected.size(), reason));
	EXPECT_EQ(reason, "high-hash");

	const std::string err = R"({"result":null,"error":{"code":-22,"message":"Block decode failed"},"id":"pqch-pool"})";
	EXPECT_FALSE(pqchd::parse_submitblock(err.data(), err.size(), reason));
	EXPECT_EQ(reason, "Block decode failed (-22)");
}

TEST(pqchd, requests)
{
	EXPECT_EQ(pqchd::getblocktemplate_request(), R"({"jsonrpc":"1.0","id":"pqch-pool","method":"getblocktemplate","params":[{"rules":["segwit"]}]})");
	EXPECT_EQ(pqchd::getblocktemplate_request("abc123"), R"({"jsonrpc":"1.0","id":"pqch-pool","method":"getblocktemplate","params":[{"rules":["segwit"],"longpollid":"abc123"}]})");
	// anything that could break out of the JSON string is refused
	EXPECT_TRUE(pqchd::getblocktemplate_request(R"(a"}],"x":[{")").empty());
}

TEST(pqchd, auth)
{
	EXPECT_EQ(pqchd::make_auth("user", "pass"), "basic:user:pass");

	const char* path = "pqchd_test_cookie";
	{
		std::ofstream f(path);
		f << "__cookie__:0123abcd\n";
	}
	std::string auth;
	EXPECT_TRUE(pqchd::read_cookie(path, auth));
	EXPECT_EQ(auth, "basic:__cookie__:0123abcd");
	{
		std::ofstream f(path);
		f << "nocolon\n";
	}
	EXPECT_FALSE(pqchd::read_cookie(path, auth));
	std::remove(path);
	EXPECT_FALSE(pqchd::read_cookie("does_not_exist", auth));
}

} // namespace p2pool
