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
#include <openssl/sha2.h>
#include <algorithm>

namespace p2pool {
namespace pqch {

namespace {

// ---------------------------------------------------------------- Bech32m (BIP350)

constexpr char CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
constexpr uint32_t BECH32M_CONST = 0x2bc830a3;

uint32_t polymod(const std::vector<uint8_t>& v)
{
	static constexpr uint32_t GEN[5] = { 0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3 };
	uint32_t chk = 1;
	for (uint8_t x : v) {
		const uint32_t b = chk >> 25;
		chk = ((chk & 0x1ffffff) << 5) ^ x;
		for (int i = 0; i < 5; ++i) {
			if ((b >> i) & 1) {
				chk ^= GEN[i];
			}
		}
	}
	return chk;
}

std::vector<uint8_t> hrp_expand(const std::string& hrp)
{
	std::vector<uint8_t> r;
	for (char c : hrp) r.push_back(static_cast<uint8_t>(c) >> 5);
	r.push_back(0);
	for (char c : hrp) r.push_back(static_cast<uint8_t>(c) & 31);
	return r;
}

bool convert_bits(const std::vector<uint8_t>& in, int from, int to, bool pad, std::vector<uint8_t>& out)
{
	uint32_t acc = 0;
	int bits = 0;
	const uint32_t maxv = (1u << to) - 1;
	for (uint8_t v : in) {
		if (v >> from) return false;
		acc = (acc << from) | v;
		bits += from;
		while (bits >= to) {
			bits -= to;
			out.push_back(static_cast<uint8_t>((acc >> bits) & maxv));
		}
	}
	if (pad) {
		if (bits) out.push_back(static_cast<uint8_t>((acc << (to - bits)) & maxv));
	}
	else if ((bits >= from) || ((acc << (to - bits)) & maxv)) {
		return false;
	}
	return true;
}

// ---------------------------------------------------------------- serialization

void write_u32(std::vector<uint8_t>& out, uint32_t v)
{
	for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (i * 8)));
}

void write_u64(std::vector<uint8_t>& out, uint64_t v)
{
	for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (i * 8)));
}

void write_compact_size(std::vector<uint8_t>& out, uint64_t n)
{
	if (n < 253) {
		out.push_back(static_cast<uint8_t>(n));
	}
	else if (n <= 0xffff) {
		out.push_back(253);
		out.push_back(static_cast<uint8_t>(n));
		out.push_back(static_cast<uint8_t>(n >> 8));
	}
	else if (n <= 0xffffffffULL) {
		out.push_back(254);
		write_u32(out, static_cast<uint32_t>(n));
	}
	else {
		out.push_back(255);
		write_u64(out, n);
	}
}

// Minimal CScriptNum push of a non-negative number (BIP34 height), as Bitcoin Core's `CScript() << nHeight`
void push_script_num(std::vector<uint8_t>& script, uint64_t n)
{
	if (n == 0) {
		script.push_back(0x00); // OP_0
		return;
	}
	if (n <= 16) {
		script.push_back(static_cast<uint8_t>(0x50 + n)); // OP_1..OP_16
		return;
	}
	std::vector<uint8_t> v;
	while (n) {
		v.push_back(static_cast<uint8_t>(n & 0xff));
		n >>= 8;
	}
	if (v.back() & 0x80) {
		v.push_back(0); // keep it positive
	}
	script.push_back(static_cast<uint8_t>(v.size()));
	script.insert(script.end(), v.begin(), v.end());
}

hash sha256d(const uint8_t* data, size_t size)
{
	hash h1, h2;
	SHA256(data, size, h1.h);
	SHA256(h1.h, HASH_SIZE, h2.h);
	return h2;
}

} // namespace

// ---------------------------------------------------------------- addresses

const char* address_hrp(Network net)
{
	switch (net) {
	case Network::MAIN: return "pqc";
	case Network::TEST: return "pqct";
	case Network::REGTEST: return "pqcrt";
	}
	return "";
}

bool p2mr_script_from_address(const std::string& address, Network net, std::vector<uint8_t>& script)
{
	const std::string hrp = address_hrp(net);
	if ((address.size() < hrp.size() + 8) || (address.size() > 90)) return false;

	// one case only; we take lower case
	std::string a = address;
	bool lower = false, upper = false;
	for (char& c : a) {
		if ((c >= 'a') && (c <= 'z')) lower = true;
		if ((c >= 'A') && (c <= 'Z')) { upper = true; c = static_cast<char>(c - 'A' + 'a'); }
	}
	if (lower && upper) return false;

	const size_t sep = a.rfind('1');
	if ((sep == std::string::npos) || (a.compare(0, sep, hrp) != 0) || (sep != hrp.size())) return false;

	std::vector<uint8_t> data;
	for (size_t i = sep + 1; i < a.size(); ++i) {
		const char* p = strchr(CHARSET, a[i]);
		if (!p || !*p) return false;
		data.push_back(static_cast<uint8_t>(p - CHARSET));
	}
	if (data.size() < 7) return false;

	std::vector<uint8_t> check = hrp_expand(hrp);
	check.insert(check.end(), data.begin(), data.end());
	if (polymod(check) != BECH32M_CONST) return false; // Bech32 (v0) checksums are refused too

	data.resize(data.size() - 6);
	if (data.empty() || (data[0] != 1)) return false; // P2MR is witness version 1

	std::vector<uint8_t> program;
	if (!convert_bits(std::vector<uint8_t>(data.begin() + 1, data.end()), 5, 8, false, program)) return false;
	if (program.size() != 32) return false;

	script.clear();
	script.push_back(0x51); // OP_1
	script.push_back(0x20); // push 32
	script.insert(script.end(), program.begin(), program.end());
	return true;
}

bool address_from_p2mr_script(const std::vector<uint8_t>& script, Network net, std::string& address)
{
	if ((script.size() != 34) || (script[0] != 0x51) || (script[1] != 0x20)) return false;
	const std::string hrp = address_hrp(net);

	std::vector<uint8_t> data{ 1 };
	if (!convert_bits(std::vector<uint8_t>(script.begin() + 2, script.end()), 8, 5, true, data)) return false;

	std::vector<uint8_t> check = hrp_expand(hrp);
	check.insert(check.end(), data.begin(), data.end());
	check.insert(check.end(), 6, 0);
	const uint32_t mod = polymod(check) ^ BECH32M_CONST;

	address = hrp + '1';
	for (uint8_t d : data) address += CHARSET[d];
	for (int i = 0; i < 6; ++i) address += CHARSET[(mod >> (5 * (5 - i))) & 31];
	return true;
}

// ---------------------------------------------------------------- PPLNS split

std::vector<Payout> split_reward(uint64_t total, const std::vector<PayoutWeight>& weights, const std::vector<uint8_t>& finder)
{
	// merge by script
	std::vector<PayoutWeight> merged;
	for (const PayoutWeight& w : weights) {
		if (w.weight == 0) continue;
		auto it = std::find_if(merged.begin(), merged.end(), [&](const PayoutWeight& m) { return m.script == w.script; });
		if (it == merged.end()) merged.push_back(w);
		else it->weight += w.weight;
	}
	if (merged.empty() || (total == 0)) return {};

	// the MAX_PAYOUTS heaviest (ties: smaller script first)
	std::sort(merged.begin(), merged.end(), [](const PayoutWeight& a, const PayoutWeight& b) {
		return (a.weight != b.weight) ? (a.weight > b.weight) : (a.script < b.script);
	});
	if (merged.size() > MAX_PAYOUTS) merged.resize(MAX_PAYOUTS);

	uint64_t sum = 0;
	for (const PayoutWeight& w : merged) sum += w.weight;

	std::vector<Payout> out;
	out.reserve(merged.size());
	uint64_t paid = 0;
	for (const PayoutWeight& w : merged) {
		uint64_t hi;
		const uint64_t lo = umul128(total, w.weight, &hi);
		uint64_t rem;
		const uint64_t v = udiv128(hi, lo, sum, &rem); // <= total because w.weight <= sum
		out.push_back(Payout{ w.script, v });
		paid += v;
	}

	// rounding remainder: to the finder if paid in this block, otherwise to the heaviest miner (first)
	const uint64_t remainder = total - paid;
	auto f = std::find_if(out.begin(), out.end(), [&](const Payout& p) { return p.script == finder; });
	(f != out.end() ? *f : out.front()).value += remainder;

	out.erase(std::remove_if(out.begin(), out.end(), [](const Payout& p) { return p.value == 0; }), out.end());
	std::sort(out.begin(), out.end(), [](const Payout& a, const Payout& b) { return a.script < b.script; });
	return out;
}

// ---------------------------------------------------------------- coinbase, merkle, header, block

bool make_coinbase(const pqchd::Template& t, const std::vector<Payout>& payouts, const hash& share_id, uint64_t extra_nonce, Coinbase& out)
{
	if (payouts.empty() || t.witness_commitment.empty()) return false;

	uint64_t sum = 0;
	for (const Payout& p : payouts) {
		if (p.script.empty() || (sum + p.value < sum)) return false;
		sum += p.value;
	}
	if (sum > t.coinbase_value) return false;

	std::vector<uint8_t> script_sig;
	push_script_num(script_sig, t.height);
	script_sig.push_back(HASH_SIZE);
	script_sig.insert(script_sig.end(), share_id.h, share_id.h + HASH_SIZE);
	script_sig.push_back(8);
	for (int i = 0; i < 8; ++i) script_sig.push_back(static_cast<uint8_t>(extra_nonce >> (i * 8)));
	if ((script_sig.size() < 2) || (script_sig.size() > 100)) return false; // consensus: bad-cb-length

	// body = inputs and outputs (common to both serializations)
	std::vector<uint8_t> body;
	write_compact_size(body, 1);
	body.insert(body.end(), HASH_SIZE, 0);       // prevout hash: null
	write_u32(body, 0xffffffffu);                // prevout index: null
	write_compact_size(body, script_sig.size());
	body.insert(body.end(), script_sig.begin(), script_sig.end());
	write_u32(body, 0xfffffffeu);                // MAX_SEQUENCE_NONFINAL: nLockTime is enforced

	write_compact_size(body, payouts.size() + 1);
	for (const Payout& p : payouts) {
		write_u64(body, p.value);
		write_compact_size(body, p.script.size());
		body.insert(body.end(), p.script.begin(), p.script.end());
	}
	write_u64(body, 0);                          // SegWit commitment output
	write_compact_size(body, t.witness_commitment.size());
	body.insert(body.end(), t.witness_commitment.begin(), t.witness_commitment.end());

	const uint32_t version = 2;
	const uint32_t lock_time = static_cast<uint32_t>(t.height - 1); // as Bitcoin Core's miner

	out.without_witness.clear();
	write_u32(out.without_witness, version);
	out.without_witness.insert(out.without_witness.end(), body.begin(), body.end());
	write_u32(out.without_witness, lock_time);

	out.with_witness.clear();
	write_u32(out.with_witness, version);
	out.with_witness.push_back(0x00);            // marker
	out.with_witness.push_back(0x01);            // flag
	out.with_witness.insert(out.with_witness.end(), body.begin(), body.end());
	write_compact_size(out.with_witness, 1);     // one witness item:
	write_compact_size(out.with_witness, HASH_SIZE);
	out.with_witness.insert(out.with_witness.end(), HASH_SIZE, 0); // the witness nonce (0, as in default_witness_commitment)
	write_u32(out.with_witness, lock_time);

	out.txid = sha256d(out.without_witness.data(), out.without_witness.size());
	return true;
}

hash merkle_root(const std::vector<hash>& txids)
{
	if (txids.empty()) return hash();
	std::vector<hash> level = txids;
	while (level.size() > 1) {
		if (level.size() & 1) level.push_back(level.back());
		std::vector<hash> next;
		next.reserve(level.size() / 2);
		for (size_t i = 0; i < level.size(); i += 2) {
			uint8_t buf[HASH_SIZE * 2];
			memcpy(buf, level[i].h, HASH_SIZE);
			memcpy(buf + HASH_SIZE, level[i + 1].h, HASH_SIZE);
			next.push_back(sha256d(buf, sizeof(buf)));
		}
		level.swap(next);
	}
	return level[0];
}

void make_header(const pqchd::Template& t, const hash& merkle, uint32_t time, uint32_t nonce, uint8_t (&header)[HEADER_SIZE])
{
	std::vector<uint8_t> h;
	h.reserve(HEADER_SIZE);
	write_u32(h, static_cast<uint32_t>(t.version));
	h.insert(h.end(), t.prev_hash.h, t.prev_hash.h + HASH_SIZE);
	h.insert(h.end(), merkle.h, merkle.h + HASH_SIZE);
	write_u32(h, time);
	write_u32(h, t.bits);
	write_u32(h, nonce);
	memcpy(header, h.data(), HEADER_SIZE);
}

std::vector<uint8_t> serialize_block(const uint8_t (&header)[HEADER_SIZE], const Coinbase& coinbase, const pqchd::Template& t)
{
	size_t size = HEADER_SIZE + 9 + coinbase.with_witness.size();
	for (const pqchd::TemplateTx& tx : t.transactions) size += tx.data.size();

	std::vector<uint8_t> b;
	b.reserve(size);
	b.insert(b.end(), header, header + HEADER_SIZE);
	write_compact_size(b, t.transactions.size() + 1);
	b.insert(b.end(), coinbase.with_witness.begin(), coinbase.with_witness.end());
	for (const pqchd::TemplateTx& tx : t.transactions) b.insert(b.end(), tx.data.begin(), tx.data.end());
	return b;
}

hash block_hash(const uint8_t (&header)[HEADER_SIZE])
{
	return sha256d(header, HEADER_SIZE);
}

bool check_pow(const hash& pow_hash, const hash& target)
{
	for (size_t i = HASH_SIZE; i-- > 0;) { // most significant byte last
		if (pow_hash.h[i] != target.h[i]) return pow_hash.h[i] < target.h[i];
	}
	return true;
}

} // namespace pqch
} // namespace p2pool
