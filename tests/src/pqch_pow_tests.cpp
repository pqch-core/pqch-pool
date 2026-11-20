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
#include "pqch_pow.h"
#include "gtest/gtest.h"

namespace p2pool {

namespace {

std::string rx_hex(pqch::PowHasher& h, const char* key, const char* input)
{
	EXPECT_TRUE(h.set_key(key, strlen(key)));
	const hash r = h.calculate(input, strlen(input));
	static constexpr char d[] = "0123456789abcdef";
	std::string s;
	for (uint8_t b : r.h) { s += d[b >> 4]; s += d[b & 15]; }
	return s;
}

} // namespace

// Official RandomX v2 test vectors (tevador/RandomX v2.0.1, src/tests/tests.cpp), the same ones pqchd checks
// in src/test/pqch_randomx_tests.cpp: the pool and the node compute the same proof of work.
TEST(pqch_pow, randomx_v2_official_vectors)
{
	pqch::PowHasher h;
	EXPECT_EQ(rx_hex(h, "test key 000", "This is a test"), "22ec6b861b3eb23686b2efbad69513c967ecfce80983df66c9c5b4fbfb4cdb6f");
	EXPECT_EQ(rx_hex(h, "test key 000", "Lorem ipsum dolor sit amet"), "9e2c772c12fd48f93c14c97fdc89d556264d9100597023f44d9163e279012ecf");
	EXPECT_EQ(rx_hex(h, "test key 000", "sed do eiusmod tempor incididunt ut labore et dolore magna aliqua"), "4d6b063a1a603751d525f18a171336a4002f2f06df6c17e4b25fe17e17796e42");
	EXPECT_EQ(rx_hex(h, "test key 001", "sed do eiusmod tempor incididunt ut labore et dolore magna aliqua"), "97024134686ce27d362ea8d86d8ef16483ac272abdabd46ef13359400777fe5e");
	EXPECT_EQ(rx_hex(h, "test key 000", "This is a test"), "22ec6b861b3eb23686b2efbad69513c967ecfce80983df66c9c5b4fbfb4cdb6f");
}

} // namespace p2pool
