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
#include <openssl/sha2.h>

namespace p2pool {
namespace pqch {

namespace {

void put_u64(std::vector<uint8_t>& v, uint64_t x)
{
	for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (i * 8)));
}

hash sha256d(const std::vector<uint8_t>& v)
{
	hash a, b;
	SHA256(v.data(), v.size(), a.h);
	SHA256(a.h, HASH_SIZE, b.h);
	return b;
}

} // namespace

std::vector<uint8_t> share_id_preimage(const hash& parent, uint64_t height, uint64_t difficulty, uint64_t timestamp,
	const std::vector<uint8_t>& miner_script, const hash& main_prev, uint64_t main_height)
{
	static constexpr char TAG[] = "PQCH share v1";
	std::vector<uint8_t> v(TAG, TAG + sizeof(TAG) - 1);
	v.insert(v.end(), parent.h, parent.h + HASH_SIZE);
	put_u64(v, height);
	put_u64(v, difficulty);
	put_u64(v, timestamp);
	put_u64(v, miner_script.size());
	v.insert(v.end(), miner_script.begin(), miner_script.end());
	v.insert(v.end(), main_prev.h, main_prev.h + HASH_SIZE);
	put_u64(v, main_height);
	return v;
}

uint64_t SideChain::next_difficulty() const
{
	const uint64_t n = std::min<uint64_t>(m_chain.size(), m_params.difficulty_window);
	if (n < 2) {
		return m_params.min_difficulty;
	}

	// Work done over the last n shares divided by the time they took, times the target time per share.
	const Share& oldest = m_chain[m_chain.size() - n];
	const Share& newest = m_chain.back();
	uint64_t work = 0;
	for (size_t i = m_chain.size() - n + 1; i < m_chain.size(); ++i) {
		work += m_chain[i].difficulty;
	}
	uint64_t span = (newest.timestamp > oldest.timestamp) ? (newest.timestamp - oldest.timestamp) : 1;

	// Do not move by more than a factor of 2 per share, so that a burst or a stall cannot swing it wildly.
	uint64_t hi;
	const uint64_t lo = umul128(work, m_params.share_time, &hi);
	uint64_t rem;
	uint64_t d = (hi >= span) ? UINT64_MAX : udiv128(hi, lo, span, &rem);
	const uint64_t prev = newest.difficulty;
	d = std::min(d, prev * 2);
	d = std::max(d, prev / 2);
	return std::max(d, m_params.min_difficulty);
}

std::vector<PayoutWeight> SideChain::pplns_weights(const std::vector<uint8_t>& miner_script, uint64_t difficulty) const
{
	std::vector<PayoutWeight> w;
	w.push_back({ miner_script, difficulty });
	const uint64_t from_chain = std::min<uint64_t>(m_chain.size(), m_params.pplns_window - 1);
	for (size_t i = m_chain.size() - from_chain; i < m_chain.size(); ++i) {
		w.push_back({ m_chain[i].miner_script, m_chain[i].difficulty });
	}
	return w;
}

bool SideChain::add(const Share& s, std::string& error)
{
	const Share* t = tip();
	if (t ? (s.parent != t->id) : (s.parent != hash())) { error = "parent is not the tip"; return false; }
	if (s.height != (t ? t->height + 1 : 0)) { error = "wrong share height"; return false; }
	if (s.difficulty != next_difficulty()) { error = "wrong share difficulty"; return false; }
	if (t && (s.timestamp + 600 < t->timestamp)) { error = "timestamp too far in the past"; return false; }

	const hash id = sha256d(share_id_preimage(s.parent, s.height, s.difficulty, s.timestamp, s.miner_script, s.main_prev, s.main_height));
	if (id != s.id) { error = "share id does not match its fields"; return false; }
	if (memcmp(s.header + 4, s.main_prev.h, HASH_SIZE) != 0) { error = "header does not build on main_prev"; return false; }
	if (!difficulty_type(s.difficulty).check_pow(s.pow_hash)) { error = "not enough work for the share difficulty"; return false; }

	m_chain.push_back(s);
	while (m_chain.size() > m_params.pplns_window + m_params.difficulty_window) {
		m_chain.pop_front();
	}
	return true;
}

bool make_work(const SideChain& chain, const pqchd::Template& t, const std::vector<uint8_t>& miner_script,
	uint64_t extra_nonce, uint64_t id, uint64_t now, Work& w)
{
	const Share* tip = chain.tip();
	w.id = id;
	w.tmpl = t;
	w.miner_script = miner_script;
	w.parent = tip ? tip->id : hash();
	w.share_height = tip ? tip->height + 1 : 0;
	w.share_difficulty = chain.next_difficulty();
	w.timestamp = std::max(now, t.mintime);
	if (tip && (w.timestamp < tip->timestamp)) {
		w.timestamp = tip->timestamp; // shares do not go back in time
	}
	w.share_id = sha256d(share_id_preimage(w.parent, w.share_height, w.share_difficulty, w.timestamp, miner_script, t.prev_hash, t.height));

	w.payouts = split_reward(t.coinbase_value, chain.pplns_weights(miner_script, w.share_difficulty), miner_script);
	if (!make_coinbase(t, w.payouts, w.share_id, extra_nonce, w.coinbase)) {
		return false;
	}
	std::vector<hash> txids{ w.coinbase.txid };
	for (const auto& tx : t.transactions) txids.push_back(tx.txid);
	w.merkle_root = merkle_root(txids);
	return true;
}

Share make_share(const Work& w, const uint8_t (&header)[HEADER_SIZE], const hash& pow_hash)
{
	Share s;
	s.id = w.share_id;
	s.parent = w.parent;
	s.height = w.share_height;
	s.difficulty = w.share_difficulty;
	s.timestamp = w.timestamp;
	s.miner_script = w.miner_script;
	s.main_prev = w.tmpl.prev_hash;
	s.main_height = w.tmpl.height;
	memcpy(s.header, header, HEADER_SIZE);
	s.pow_hash = pow_hash;
	return s;
}

} // namespace pqch
} // namespace p2pool
