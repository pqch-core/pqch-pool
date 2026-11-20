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
#include <openssl/sha2.h>

namespace p2pool {
namespace pqch {

namespace {

hash sha256d(const uint8_t* data, size_t size)
{
	hash a, b;
	SHA256(data, size, a.h);
	SHA256(a.h, HASH_SIZE, b.h);
	return b;
}

void put(std::vector<uint8_t>& v, uint64_t x, int bytes)
{
	for (int i = 0; i < bytes; ++i) v.push_back(static_cast<uint8_t>(x >> (i * 8)));
}

struct Reader
{
	const uint8_t* p;
	const uint8_t* end;
	bool ok = true;

	bool need(size_t n) { if (static_cast<size_t>(end - p) < n) ok = false; return ok; }
	uint64_t num(int bytes) { uint64_t x = 0; if (!need(bytes)) return 0; for (int i = 0; i < bytes; ++i) x |= static_cast<uint64_t>(*p++) << (i * 8); return x; }
	void raw(uint8_t* out, size_t n) { if (!need(n)) return; memcpy(out, p, n); p += n; }
	uint64_t compact_size()
	{
		const uint64_t f = num(1);
		if (f < 253) return f;
		if (f == 253) return num(2);
		if (f == 254) return num(4);
		return num(8);
	}
};

// Minimal BIP34 push of a height, as make_coinbase writes it
std::vector<uint8_t> height_push(uint64_t n)
{
	std::vector<uint8_t> s;
	if (n == 0) { s.push_back(0x00); return s; }
	if (n <= 16) { s.push_back(static_cast<uint8_t>(0x50 + n)); return s; }
	std::vector<uint8_t> v;
	while (n) { v.push_back(static_cast<uint8_t>(n & 0xff)); n >>= 8; }
	if (v.back() & 0x80) v.push_back(0);
	s.push_back(static_cast<uint8_t>(v.size()));
	s.insert(s.end(), v.begin(), v.end());
	return s;
}

} // namespace

std::vector<uint8_t> serialize(const ShareMessage& m)
{
	const Share& s = m.share;
	std::vector<uint8_t> v;
	v.reserve(200 + m.coinbase.size() + m.merkle_branch.size() * HASH_SIZE);
	v.push_back(1); // version
	v.insert(v.end(), s.header, s.header + HEADER_SIZE);
	v.insert(v.end(), s.parent.h, s.parent.h + HASH_SIZE);
	put(v, s.height, 8);
	put(v, s.difficulty, 8);
	put(v, s.timestamp, 8);
	put(v, s.main_height, 8);
	v.push_back(static_cast<uint8_t>(s.miner_script.size()));
	v.insert(v.end(), s.miner_script.begin(), s.miner_script.end());
	put(v, m.coinbase.size(), 4);
	v.insert(v.end(), m.coinbase.begin(), m.coinbase.end());
	v.push_back(static_cast<uint8_t>(m.merkle_branch.size()));
	for (const hash& h : m.merkle_branch) v.insert(v.end(), h.h, h.h + HASH_SIZE);
	return v;
}

bool deserialize(const uint8_t* data, size_t size, ShareMessage& m, std::string& error)
{
	if (size > MAX_SHARE_MESSAGE_SIZE) { error = "share message too large"; return false; }
	Reader r{ data, data + size };
	ShareMessage x;
	Share& s = x.share;

	if (r.num(1) != 1) { error = "unknown share message version"; return false; }
	r.raw(s.header, HEADER_SIZE);
	r.raw(s.parent.h, HASH_SIZE);
	s.height = r.num(8);
	s.difficulty = r.num(8);
	s.timestamp = r.num(8);
	s.main_height = r.num(8);
	const uint64_t script_size = r.num(1);
	if (!r.ok || (script_size > MAX_MINER_SCRIPT) || !r.need(script_size)) { error = "bad miner script"; return false; }
	s.miner_script.assign(r.p, r.p + script_size);
	r.p += script_size;
	const uint64_t cb_size = r.num(4);
	if (!r.ok || (cb_size > MAX_SHARE_MESSAGE_SIZE) || !r.need(cb_size)) { error = "bad coinbase"; return false; }
	x.coinbase.assign(r.p, r.p + cb_size);
	r.p += cb_size;
	const uint64_t n = r.num(1);
	if (!r.ok || (n > MAX_MERKLE_BRANCH)) { error = "bad merkle branch"; return false; }
	x.merkle_branch.resize(n);
	for (hash& h : x.merkle_branch) r.raw(h.h, HASH_SIZE);
	if (!r.ok) { error = "truncated share message"; return false; }
	if (r.p != r.end) { error = "trailing bytes in share message"; return false; }

	memcpy(s.main_prev.h, s.header + 4, HASH_SIZE);
	const std::vector<uint8_t> pre = share_id_preimage(s.parent, s.height, s.difficulty, s.timestamp, s.miner_script, s.main_prev, s.main_height);
	s.id = sha256d(pre.data(), pre.size());
	m = std::move(x);
	return true;
}

std::vector<hash> coinbase_merkle_branch(const std::vector<hash>& txids)
{
	std::vector<hash> branch;
	std::vector<hash> level = txids;
	while (level.size() > 1) {
		if (level.size() & 1) level.push_back(level.back());
		branch.push_back(level[1]); // the coinbase's path is always the first of each level
		std::vector<hash> next;
		for (size_t i = 0; i < level.size(); i += 2) {
			uint8_t buf[HASH_SIZE * 2];
			memcpy(buf, level[i].h, HASH_SIZE);
			memcpy(buf + HASH_SIZE, level[i + 1].h, HASH_SIZE);
			next.push_back(sha256d(buf, sizeof(buf)));
		}
		level.swap(next);
	}
	return branch;
}

hash merkle_root_from_branch(const hash& coinbase_txid, const std::vector<hash>& branch)
{
	hash h = coinbase_txid;
	for (const hash& sibling : branch) {
		uint8_t buf[HASH_SIZE * 2];
		memcpy(buf, h.h, HASH_SIZE);
		memcpy(buf + HASH_SIZE, sibling.h, HASH_SIZE);
		h = sha256d(buf, sizeof(buf));
	}
	return h;
}

ShareMessage make_share_message(const Work& w, const Share& s)
{
	ShareMessage m;
	m.share = s;
	m.coinbase = w.coinbase.without_witness;
	std::vector<hash> txids{ w.coinbase.txid };
	for (const auto& tx : w.tmpl.transactions) txids.push_back(tx.txid);
	m.merkle_branch = coinbase_merkle_branch(txids);
	return m;
}

bool verify_share_coinbase(const SideChain& chain, const ShareMessage& m, std::string& error)
{
	const Share& s = m.share;
	const Share* parent = (s.height == 0) ? nullptr : chain.get(s.parent);
	if ((s.height != 0) && !parent) { error = "unknown parent"; return false; }

	// the coinbase is in the block
	const hash txid = sha256d(m.coinbase.data(), m.coinbase.size());
	if (memcmp(merkle_root_from_branch(txid, m.merkle_branch).h, s.header + 36, HASH_SIZE) != 0) {
		error = "coinbase not in the block (merkle branch)";
		return false;
	}

	// parse the coinbase (no witness)
	Reader r{ m.coinbase.data(), m.coinbase.data() + m.coinbase.size() };
	r.num(4); // version
	if (r.compact_size() != 1) { error = "coinbase: not one input"; return false; }
	uint8_t prevout[36];
	r.raw(prevout, sizeof(prevout));
	static const uint8_t null_prevout[36] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0xff,0xff,0xff,0xff };
	if (!r.ok || memcmp(prevout, null_prevout, sizeof(prevout))) { error = "coinbase: not a coinbase input"; return false; }

	const uint64_t ss_size = r.compact_size();
	if (!r.ok || (ss_size > 100) || !r.need(ss_size)) { error = "coinbase: bad scriptSig"; return false; }
	std::vector<uint8_t> expected = height_push(s.main_height);
	expected.push_back(HASH_SIZE);
	expected.insert(expected.end(), s.id.h, s.id.h + HASH_SIZE);
	expected.push_back(8);
	if ((ss_size != expected.size() + 8) || memcmp(r.p, expected.data(), expected.size())) {
		error = "coinbase: scriptSig does not commit to this share";
		return false;
	}
	r.p += ss_size;
	r.num(4); // sequence

	const uint64_t n_out = r.compact_size();
	if (!r.ok || (n_out < 2) || (n_out > MAX_PAYOUTS + 1)) { error = "coinbase: bad output count"; return false; }
	std::vector<Payout> paid;
	uint64_t total = 0;
	for (uint64_t i = 0; i < n_out; ++i) {
		Payout p;
		p.value = r.num(8);
		const uint64_t len = r.compact_size();
		if (!r.ok || (len > 10000) || !r.need(len)) { error = "coinbase: bad output"; return false; }
		p.script.assign(r.p, r.p + len);
		r.p += len;
		if (i + 1 == n_out) {
			// the last output is the SegWit commitment
			static const uint8_t head[6] = { 0x6a, 0x24, 0xaa, 0x21, 0xa9, 0xed };
			if ((p.value != 0) || (p.script.size() != 38) || memcmp(p.script.data(), head, sizeof(head))) {
				error = "coinbase: last output is not the SegWit commitment";
				return false;
			}
			break;
		}
		if (total + p.value < total) { error = "coinbase: value overflow"; return false; }
		total += p.value;
		paid.push_back(std::move(p));
	}
	r.num(4); // lock time
	if (!r.ok || (r.p != r.end)) { error = "coinbase: malformed"; return false; }

	// it pays exactly the PPLNS split of this share's branch
	const std::vector<Payout> due = split_reward(total, chain.pplns_weights(parent, s.miner_script, s.difficulty), s.miner_script);
	if (paid.size() != due.size()) { error = "coinbase does not pay the PPLNS split"; return false; }
	for (size_t i = 0; i < due.size(); ++i) {
		if ((paid[i].script != due[i].script) || (paid[i].value != due[i].value)) {
			error = "coinbase does not pay the PPLNS split";
			return false;
		}
	}
	return true;
}

} // namespace pqch
} // namespace p2pool
