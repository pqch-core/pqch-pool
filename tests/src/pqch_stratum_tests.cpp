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
#include "pqch_stratum.h"
#include "gtest/gtest.h"

namespace p2pool {

using namespace pqch;

TEST(pqch_stratum, job_round_trip)
{
	StratumJob j;
	j.job_id = "1f.3";
	for (size_t i = 0; i < HEADER_SIZE; ++i) j.blob[i] = static_cast<uint8_t>(i * 7);
	j.difficulty = 123456789012ULL;
	for (size_t i = 0; i < HASH_SIZE; ++i) j.seed_hash.h[i] = static_cast<uint8_t>(255 - i);
	j.height = 21782;

	const std::string json = stratum_job_json(j);
	StratumJob k;
	std::string error;
	ASSERT_TRUE(parse_stratum_job(json.data(), json.size(), k, error)) << error;
	EXPECT_EQ(k.job_id, j.job_id);
	EXPECT_EQ(memcmp(k.blob, j.blob, HEADER_SIZE), 0);
	EXPECT_EQ(k.difficulty, j.difficulty);
	EXPECT_EQ(k.seed_hash, j.seed_hash);
	EXPECT_EQ(k.height, j.height);

	const std::string notify = R"({"jsonrpc":"2.0","method":"job","params":)" + json + "}";
	ASSERT_TRUE(parse_stratum_job_notify(notify.data(), notify.size(), k, error)) << error;
	EXPECT_EQ(k.job_id, j.job_id);
}

TEST(pqch_stratum, bad_jobs)
{
	StratumJob j;
	j.job_id = "x";
	j.difficulty = 1;
	const std::string good = stratum_job_json(j);
	std::string error;
	StratumJob k;
	ASSERT_TRUE(parse_stratum_job(good.data(), good.size(), k, error)) << error;

	auto bad = [&](const std::string& from, const std::string& to) {
		std::string s = good;
		const size_t p = s.find(from);
		EXPECT_NE(p, std::string::npos) << from;
		s.replace(p, from.size(), to);
		return !parse_stratum_job(s.data(), s.size(), k, error);
	};
	EXPECT_TRUE(bad("rx/pqch", "rx/0"));                          // another algorithm (Monero's)
	EXPECT_TRUE(bad("\"difficulty\":1", "\"difficulty\":0"));      // zero difficulty
	EXPECT_TRUE(bad("\"difficulty\":1", "\"difficulty\":-1"));
	EXPECT_TRUE(bad("\"blob\":\"00", "\"blob\":\"0"));             // 159 hex digits
	EXPECT_TRUE(bad("\"blob\":\"00", "\"blob\":\"zz"));            // not hex
	EXPECT_TRUE(bad("\"seed_hash\":\"00", "\"seed_hash\":\"0000")); // 33 bytes
	EXPECT_TRUE(bad("\"job_id\":\"x\"", "\"job_id\":\"\""));       // empty id
	EXPECT_TRUE(bad("\"height\":0", "\"height\":\"0\""));          // wrong type
	const std::string junk = "{not json";
	EXPECT_FALSE(parse_stratum_job(junk.data(), junk.size(), k, error));
}

TEST(pqch_stratum, nonce_is_little_endian_at_76)
{
	uint8_t h[HEADER_SIZE] = {};
	set_header_nonce(h, 0x11223344);
	EXPECT_EQ(h[76], 0x44);
	EXPECT_EQ(h[77], 0x33);
	EXPECT_EQ(h[78], 0x22);
	EXPECT_EQ(h[79], 0x11);
	for (size_t i = 0; i < 76; ++i) EXPECT_EQ(h[i], 0);
}

} // namespace p2pool
