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

const Share* SideChain::tip() const
{
	return m_haveTip ? get(m_tip) : nullptr;
}

const Share* SideChain::get(const hash& id) const
{
	auto it = m_shares.find(id);
	return (it == m_shares.end()) ? nullptr : &it->second.share;
}

std::vector<const Share*> SideChain::branch(const hash& id, uint64_t n) const
{
	std::vector<const Share*> r;
	const Share* s = get(id);
	while (s && (r.size() < n)) {
		r.push_back(s);
		if (s->height == 0) break;
		s = get(s->parent);
	}
	return r;
}

uint64_t SideChain::next_difficulty(const Share* parent) const
{
	if (!parent) {
		return m_params.min_difficulty;
	}
	const std::vector<const Share*> b = branch(parent->id, m_params.difficulty_window); // newest first
	if (b.size() < 2) {
		return m_params.min_difficulty;
	}

	// Work done over these shares divided by the time they took, times the target time per share.
	const Share& newest = *b.front();
	const Share& oldest = *b.back();
	uint64_t work = 0;
	for (size_t i = 0; i + 1 < b.size(); ++i) {
		work += b[i]->difficulty;
	}
	const uint64_t span = (newest.timestamp > oldest.timestamp) ? (newest.timestamp - oldest.timestamp) : 1;

	// Do not move by more than a factor of 2 per share, so that a burst or a stall cannot swing it wildly.
	uint64_t hi;
	const uint64_t lo = umul128(work, m_params.share_time, &hi);
	uint64_t rem;
	uint64_t d = (hi >= span) ? UINT64_MAX : udiv128(hi, lo, span, &rem);
	const uint64_t prev = newest.difficulty;
	d = std::min(d, (prev > UINT64_MAX / 2) ? UINT64_MAX : prev * 2);
	d = std::max(d, prev / 2);
	return std::max(d, m_params.min_difficulty);
}

std::vector<PayoutWeight> SideChain::pplns_weights(const Share* parent, const std::vector<uint8_t>& miner_script, uint64_t difficulty) const
{
	std::vector<PayoutWeight> w;
	w.push_back({ miner_script, difficulty });
	if (parent) {
		for (const Share* s : branch(parent->id, m_params.pplns_window - 1)) {
			w.push_back({ s->miner_script, s->difficulty });
		}
	}
	return w;
}

bool SideChain::add(const Share& s, std::string& error, bool* became_tip, bool history)
{
	if (became_tip) *became_tip = false;
	if (contains(s.id)) { error = "duplicate share"; return false; }

	const Share* parent = nullptr;
	if (s.height == 0) {
		if (s.parent != hash()) { error = "a first share has no parent"; return false; }
	}
	else {
		parent = get(s.parent);
		if (!parent && !history) { error = "unknown parent"; return false; }
		if (parent && (s.height != parent->height + 1)) { error = "wrong share height"; return false; }
		if (parent && (s.timestamp + 600 < parent->timestamp)) { error = "timestamp too far in the past"; return false; }
	}
	if (!history && (s.difficulty != next_difficulty(parent))) { error = "wrong share difficulty"; return false; }
	if (s.difficulty == 0) { error = "zero difficulty"; return false; }

	const hash id = sha256d(share_id_preimage(s.parent, s.height, s.difficulty, s.timestamp, s.miner_script, s.main_prev, s.main_height));
	if (id != s.id) { error = "share id does not match its fields"; return false; }
	if (memcmp(s.header + 4, s.main_prev.h, HASH_SIZE) != 0) { error = "header does not build on main_prev"; return false; }
	if (!difficulty_type(s.difficulty).check_pow(s.pow_hash)) { error = "not enough work for the share difficulty"; return false; }

	Node n;
	n.share = s;
	n.cumulative = parent ? m_shares.find(parent->id)->second.cumulative : difficulty_type();
	n.cumulative += s.difficulty;
	const difficulty_type cumulative = n.cumulative;
	m_shares.emplace(s.id, std::move(n));

	const Share* t = tip();
	if (!t || (m_shares.find(t->id)->second.cumulative < cumulative)) {
		m_tip = s.id;
		m_haveTip = true;
		if (became_tip) *became_tip = true;
	}
	prune();
	return true;
}

void SideChain::prune()
{
	const Share* t = tip();
	const uint64_t keep = m_params.pplns_window + m_params.difficulty_window;
	if (!t || (t->height < keep)) {
		return;
	}
	const uint64_t min_height = t->height - keep + 1;
	for (auto it = m_shares.begin(); it != m_shares.end();) {
		if (it->second.share.height < min_height) {
			it = m_shares.erase(it);
		}
		else {
			++it;
		}
	}
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
