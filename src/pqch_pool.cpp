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

// A PQCH pool node. It takes work from pqchd, keeps the share tree, runs one built-in miner thread per miner
// address, exchanges shares with the other pool nodes (pqch_p2p), checks every share it receives (PoW,
// coinbase = PPLNS split, chain rules) and submits the shares of its own miners that are PQCH blocks.
// Every share and block is written as a JSON line to the log file, so that tests can check the coinbases
// against the share chain independently.

#include "common.h"
#include "pqch_pool.h"
#include "pqchd_client.h"
#include "pqch_sidechain.h"
#include "pqch_share.h"
#include "pqch_p2p.h"
#include "pqch_pow.h"
#include "pqch_stratum.h"
#include "json_rpc_request.h"
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>

LOG_CATEGORY(PqchPool)

namespace p2pool {
namespace pqch {

namespace {

std::string rpc_hex(const hash& h)
{
	static constexpr char d[] = "0123456789abcdef";
	std::string s;
	for (size_t i = HASH_SIZE; i-- > 0;) { s += d[h.h[i] >> 4]; s += d[h.h[i] & 15]; }
	return s;
}

// RandomX key schedule of PQCH (pow_randomx.cpp, every network): 4096-block epochs, 64-block lag
uint64_t seed_height_for(uint64_t height)
{
	constexpr uint64_t interval = 4096, lag = 64;
	if (height <= interval + lag) return 0;
	return (height - lag - 1) & ~(interval - 1);
}

constexpr size_t MAX_PENDING = 8192; // shares waiting for their parent

typedef std::vector<std::pair<std::vector<uint8_t>, uint64_t>> ShareQueue; // (serialized share, peer)

struct Pool : public P2PHandler, public StratumHandler
{
	PoolOptions opt;
	Network net = Network::REGTEST;
	std::vector<std::vector<uint8_t>> miner_scripts;

	std::recursive_mutex lock;            // protects everything below (miner threads and the loop thread)
	SideChain chain{ SideChainParams() };
	bool have_template = false;
	pqchd::Template tmpl;
	std::vector<Work> works;              // one per miner thread
	uint64_t work_generation = 0;
	uint64_t shares_found = 0, shares_received = 0, blocks_found = 0, blocks_accepted = 0, blocks_rejected = 0;
	std::ofstream log;

	unordered_map<hash, std::vector<uint8_t>> share_bytes; // serialized shares of the tree
	std::map<hash, ShareQueue> pending;   // parent id -> shares waiting for it
	size_t pending_count = 0;

	// Syncing the share chain from the other nodes: no work is handed out meanwhile (built-in or stratum), or
	// the miners would mine on a partial chain (at the minimum difficulty, on a branch nobody else follows).
	// Syncing = waiting for parents of received shares (with progress in the last SYNC_STALL_SECONDS), or, at
	// start with --peer, an empty chain for up to SYNC_START_SECONDS.
	static constexpr uint64_t SYNC_START_SECONDS = 60;
	static constexpr uint64_t SYNC_STALL_SECONDS = 60;
	bool wait_for_peers = false;
	uint64_t started_at = 0;
	uint64_t last_sync_progress = 0;
	bool was_syncing = false;
	uv_timer_t sync_timer{};

	bool syncing() const
	{
		const uint64_t now = seconds_since_epoch();
		if (pending_count && (now < last_sync_progress + SYNC_STALL_SECONDS)) return true;
		return wait_for_peers && (chain.size() == 0) && (now < started_at + SYNC_START_SECONDS);
	}
	std::set<hash> requested;             // parents asked for
	std::map<uint64_t, hash> seeds;       // RandomX key per seed height
	std::set<uint64_t> seeds_requested;
	ShareQueue waiting_for_seed;
	PowHasher net_hasher;                 // loop thread only
	PowDataset dataset;                   // fast mode: shared by the miner threads
	std::shared_mutex dataset_lock;       // miners hash under a shared lock; rebuilding takes it exclusively

	std::atomic<uint64_t> current_generation{ 0 };
	std::atomic<bool> stop{ false };
	std::atomic<uint64_t> hashes{ 0 };

	// stratum miners (phase C): each has its own work (its address in the coinbase, its own extra nonce)
	struct MinerSession
	{
		std::vector<uint8_t> script;
		Work work;
		std::string job_id;               // empty: no work sent yet
		uint32_t bad_submits = 0;
	};
	std::map<uint64_t, MinerSession> sessions;
	static constexpr uint32_t MAX_BAD_SUBMITS = 10;
	// Every submit costs one RandomX hash (light mode) on the loop thread: at most this many per second for all
	// miners together. Shares come about every 10 s on the whole share chain; more is somebody hammering.
	static constexpr uint32_t MAX_VERIFY_PER_SECOND = 10;
	uint64_t verify_second = 0;
	uint32_t verify_count = 0;

	PqchdClient* client = nullptr;
	P2P* p2p = nullptr;
	StratumServer* stratum = nullptr;
	uv_loop_t* loop = nullptr;

	typedef std::lock_guard<std::recursive_mutex> Guard;

	std::string address(const std::vector<uint8_t>& script) const
	{
		std::string a;
		address_from_p2mr_script(script, net, a);
		return a;
	}

	// ---------------------------------------------------------------- work (lock held)

	void rebuild_works()
	{
		if (!have_template) return;
		++work_generation;
		if (syncing()) {
			if (!was_syncing) LOGINFO(1, "syncing the share chain from the other pool nodes: mining waits");
			was_syncing = true;
			works.clear();
			current_generation = work_generation;
			for (auto& kv : sessions) kv.second.job_id.clear(); // their old jobs are stale now
			return;
		}
		if (was_syncing) LOGINFO(1, "share chain synced (" << chain.size() << " shares): mining");
		was_syncing = false;
		works.resize(miner_scripts.size());
		for (size_t i = 0; i < miner_scripts.size(); ++i) {
			if (!make_work(chain, tmpl, miner_scripts[i], i, work_generation, static_cast<uint64_t>(time(nullptr)), works[i])) {
				LOGERR(0, "could not build the work for miner " << i);
			}
		}
		current_generation = work_generation;
		for (auto& kv : sessions) send_stratum_job(kv.first, kv.second);
	}

	// lock held: build the work of a stratum miner and send it its new job
	void send_stratum_job(uint64_t session, MinerSession& m)
	{
		if (!have_template || !stratum || syncing()) return;
		const uint64_t extra_nonce = (1ULL << 40) | session; // the built-in miners use 0, 1, 2, ...
		if (!make_work(chain, tmpl, m.script, extra_nonce, work_generation, static_cast<uint64_t>(time(nullptr)), m.work)) {
			LOGERR(0, "could not build the work for stratum session " << session);
			return;
		}
		char id[40];
		snprintf(id, sizeof(id), "%llx.%llx", static_cast<unsigned long long>(work_generation), static_cast<unsigned long long>(session));
		m.job_id = id;
		StratumJob job;
		job.job_id = m.job_id;
		make_header(m.work.tmpl, m.work.merkle_root, static_cast<uint32_t>(m.work.timestamp), 0, job.blob);
		job.difficulty = m.work.share_difficulty;
		job.seed_hash = m.work.tmpl.seed_hash;
		job.height = m.work.tmpl.height;
		std::string* json = new std::string(stratum_job_json(job));
		CallOnLoop(loop, [this, session, json]() {
			if (stratum) stratum->send_job(session, *json);
			delete json;
		});
	}

	// ---------------------------------------------------------------- stratum miners (loop thread)

	bool on_login(uint64_t session, const std::string& addr, std::string& error) override
	{
		std::vector<uint8_t> script;
		if (!p2mr_script_from_address(addr, net, script)) {
			error = "not a P2MR address of this network";
			return false;
		}
		Guard g(lock);
		MinerSession& m = sessions[session];
		m.script = script;
		LOGINFO(3, "stratum miner " << session << " pays to " << addr);
		send_stratum_job(session, m);
		return true;
	}

	void on_disconnect(uint64_t session) override
	{
		Guard g(lock);
		sessions.erase(session);
	}

	std::string on_submit(uint64_t session, const std::string& job_id, uint32_t nonce) override
	{
		Work w;
		uint8_t header[HEADER_SIZE];
		hash pow;
		{
			// under one lock, so that the work cannot change between the check and adding the share
			Guard g(lock);
			auto it = sessions.find(session);
			if (it == sessions.end()) return "not logged in";
			MinerSession& m = it->second;
			if (!have_template || m.job_id.empty() || (job_id != m.job_id) || syncing()) return "stale job";
			const uint64_t now = seconds_since_epoch();
			if (now != verify_second) { verify_second = now; verify_count = 0; }
			if (++verify_count > MAX_VERIFY_PER_SECOND) return "node busy";
			w = m.work;
			make_header(w.tmpl, w.merkle_root, static_cast<uint32_t>(w.timestamp), nonce, header);
			// never trust the miner's hash: recompute it
			if (!net_hasher.set_key(w.tmpl.seed_hash)) return "node busy";
			pow = net_hasher.calculate(header, HEADER_SIZE);
			if (!difficulty_type(w.share_difficulty).check_pow(pow)) {
				if (++m.bad_submits > MAX_BAD_SUBMITS) {
					CallOnLoop(loop, [this, session]() { if (stratum) stratum->disconnect(session, "too many invalid shares"); });
				}
				return "low difficulty share";
			}
			const std::string error = on_found_share(w, work_generation, header, pow, "stratum miner " + std::to_string(session));
			return error.empty() ? "OK" : error;
		}
	}

	void log_share(const Share& s, bool is_block, bool own)
	{
		log << R"({"type":"share","id":")" << rpc_hex(s.id) << R"(","parent":")" << rpc_hex(s.parent)
			<< R"(","height":)" << s.height << R"(,"difficulty":)" << s.difficulty << R"(,"miner":")" << address(s.miner_script)
			<< R"(","main_height":)" << s.main_height << R"(,"block":)" << (is_block ? "true" : "false")
			<< R"(,"own":)" << (own ? "true" : "false") << "}\n";
		log.flush();
	}

	void on_template(const pqchd::Template& t)
	{
		ShareQueue retry;
		{
			Guard g(lock);
			tmpl = t;
			have_template = true;
			seeds[t.seed_height] = t.seed_hash;
			rebuild_works();
			retry.swap(waiting_for_seed);
		}
		for (auto& r : retry) process_share(r.first, r.second, false);
	}

	// ---------------------------------------------------------------- own shares (miner threads)

	void on_own_share(size_t miner, uint64_t generation, const uint8_t (&header)[HEADER_SIZE], const hash& pow)
	{
		Work w;
		{
			Guard g(lock);
			if (generation != work_generation) return; // stale: the work changed meanwhile
			w = works[miner];
		}
		on_found_share(w, generation, header, pow, "miner " + std::to_string(miner));
	}

	// A share found on work `w` (built-in or stratum miner): add it, broadcast it, submit it if it is a block.
	// Returns "" or why the share was not added.
	std::string on_found_share(const Work& w, uint64_t generation, const uint8_t (&header)[HEADER_SIZE], const hash& pow, const std::string& who)
	{
		std::vector<uint8_t> block, msg;
		std::string json;
		{
			Guard g(lock);
			if (generation != work_generation) return "stale job"; // the work changed meanwhile
			const Share s = make_share(w, header, pow);
			std::string error;
			if (!chain.add(s, error)) {
				LOGWARN(0, "own share refused: " << error);
				return error;
			}
			++shares_found;
			msg = serialize(make_share_message(w, s));
			share_bytes[s.id] = msg;
			const bool is_block = check_pow(pow, w.tmpl.target);
			log_share(s, is_block, true);
			LOGINFO(3, "share " << s.height << " by " << who << ", difficulty " << s.difficulty << (is_block ? " - a PQCH block!" : ""));

			if (is_block) {
				++blocks_found;
				block = serialize_block(header, w.coinbase, w.tmpl);
				std::string pays;
				for (const Payout& p : w.payouts) {
					if (!pays.empty()) pays += ',';
					pays += R"({"address":")" + address(p.script) + R"(","value":)" + std::to_string(p.value) + "}";
				}
				json = R"({"type":"block","hash":")" + rpc_hex(block_hash(header)) + R"(","height":)" + std::to_string(w.tmpl.height)
					+ R"(,"share":")" + rpc_hex(s.id) + R"(","coinbase_value":)" + std::to_string(w.tmpl.coinbase_value)
					+ R"(,"payouts":[)" + pays + "]";
				// Our block becomes the main-chain tip: stop mining on the old one until pqchd hands out the next
				// template (asked for right away), or the miners would only find stale blocks.
				have_template = false;
				++work_generation;
				current_generation = work_generation;
			}
			else {
				rebuild_works(); // new tip of the share chain: everybody mines on it
			}
		}

		std::vector<uint8_t>* m = new std::vector<uint8_t>(std::move(msg));
		std::vector<uint8_t>* b = block.empty() ? nullptr : new std::vector<uint8_t>(std::move(block));
		CallOnLoop(loop, [this, m, b, json]() {
			if (p2p) p2p->broadcast_share(*m);
			delete m;
			if (!b) return;
			client->submit_block(std::move(*b), [this, json](bool accepted, const std::string& reason) {
				{
					Guard g(lock);
					(accepted ? blocks_accepted : blocks_rejected)++;
					log << json << R"(,"accepted":)" << (accepted ? "true" : "false") << R"(,"reason":")" << reason << "\"}\n";
					log.flush();
					if (!accepted && !have_template) {
						have_template = true; // the tip did not change: no new template will come
						rebuild_works();
					}
				}
				client->poll_now();
			});
			delete b;
		});
		return std::string();
	}

	// ---------------------------------------------------------------- shares from other nodes (loop thread)

	bool on_share(const uint8_t* data, size_t size, uint64_t peer) override
	{
		return process_share(std::vector<uint8_t>(data, data + size), peer, false);
	}

	void request_seed(uint64_t sh)
	{
		client->get_block_hash(sh, [this, sh](bool ok, const hash& h) {
			ShareQueue retry;
			{
				Guard g(lock);
				seeds_requested.erase(sh);
				if (!ok) return;
				seeds[sh] = h;
				retry.swap(waiting_for_seed);
			}
			for (auto& r : retry) process_share(r.first, r.second, false);
		});
	}

	// `history`: the parent is not available from the network (pruned): accept the share as a root.
	// Returns false for an invalid share (the peer is disconnected).
	bool process_share(const std::vector<uint8_t>& bytes, uint64_t peer, bool history)
	{
		ShareMessage m;
		std::string error;
		if (!deserialize(bytes.data(), bytes.size(), m, error)) {
			LOGWARN(1, "bad share message: " << error);
			return false;
		}
		Share& s = m.share;

		{
			Guard g(lock);
			if (chain.contains(s.id)) return true;

			const Share* tip = chain.tip();
			const uint64_t keep = chain.params().pplns_window + chain.params().difficulty_window;
			if (tip && (s.height + keep < tip->height)) return true; // far below what this node keeps: ignore

			const Share* parent = (s.height == 0) ? nullptr : chain.get(s.parent);
			if ((s.height != 0) && !parent && !history) {
				// wait for the parent; ask the peer for it once
				if (pending_count < MAX_PENDING) {
					pending[s.parent].emplace_back(bytes, peer);
					++pending_count;
					last_sync_progress = seconds_since_epoch();
				}
				if (requested.insert(s.parent).second && p2p) p2p->send(peer, MsgType::GET_SHARE, s.parent.h, HASH_SIZE);
				return true;
			}

			// proof of work: RandomX v2 of the header with the key of its main-chain height
			const uint64_t sh = seed_height_for(s.main_height);
			auto seed = seeds.find(sh);
			if (seed == seeds.end()) {
				waiting_for_seed.emplace_back(bytes, peer);
				if (seeds_requested.insert(sh).second) request_seed(sh);
				return true;
			}
			if (!net_hasher.set_key(seed->second)) return true;
			s.pow_hash = net_hasher.calculate(s.header, HEADER_SIZE);

			// what this node can check depends on how much of the share's branch it has
			const uint64_t have = parent ? chain.branch(parent->id, chain.params().pplns_window).size() : 0;
			const uint64_t full_pplns = parent ? std::min<uint64_t>(chain.params().pplns_window - 1, parent->height + 1) : 0;
			const uint64_t full_diff = parent ? std::min<uint64_t>(chain.params().difficulty_window, parent->height + 1) : 0;
			const bool check_coinbase = !history && (have >= full_pplns);
			const bool add_as_history = history || (have < full_diff);

			if (check_coinbase && !verify_share_coinbase(chain, m, error)) {
				LOGWARN(1, "share " << s.height << " refused: " << error);
				return false;
			}
			bool became_tip = false;
			if (!chain.add(s, error, &became_tip, add_as_history)) {
				if (error == "duplicate share") return true;
				LOGWARN(1, "share " << s.height << " refused: " << error);
				return false;
			}
			++shares_received;
			last_sync_progress = seconds_since_epoch();
			share_bytes[s.id] = bytes;
			const bool is_block = have_template && check_pow(s.pow_hash, tmpl.target);
			log_share(s, is_block, false);
			LOGINFO(3, "share " << s.height << " from another node, difficulty " << s.difficulty << (became_tip ? " (new tip)" : ""));
			if (became_tip) rebuild_works();
			prune_bytes();
		}

		if (p2p) p2p->broadcast_share(bytes, peer);

		// children that were waiting for this share
		ShareQueue children;
		{
			Guard g(lock);
			auto it = pending.find(s.id);
			if (it != pending.end()) {
				children.swap(it->second);
				pending_count -= children.size();
				pending.erase(it);
			}
			requested.erase(s.id);
		}
		for (auto& c : children) process_share(c.first, c.second, false);
		return true;
	}

	void on_not_found(const hash& id, uint64_t) override
	{
		// The network no longer has this share: its children start this node's tree.
		ShareQueue children;
		{
			Guard g(lock);
			auto it = pending.find(id);
			if (it == pending.end()) return;
			children.swap(it->second);
			pending_count -= children.size();
			pending.erase(it);
			requested.erase(id);
		}
		for (auto& c : children) process_share(c.first, c.second, true);
	}

	bool get_share(const hash& id, std::vector<uint8_t>& out) override
	{
		Guard g(lock);
		auto it = share_bytes.find(id);
		if (it == share_bytes.end()) return false;
		out = it->second;
		return true;
	}

	bool get_tip(std::vector<uint8_t>& out) override
	{
		Guard g(lock);
		const Share* t = chain.tip();
		if (!t) return false;
		auto it = share_bytes.find(t->id);
		if (it == share_bytes.end()) return false;
		out = it->second;
		return true;
	}

	// lock held
	void prune_bytes()
	{
		if (share_bytes.size() <= chain.size() + 64) return;
		for (auto it = share_bytes.begin(); it != share_bytes.end();) {
			if (!chain.contains(it->first)) it = share_bytes.erase(it);
			else ++it;
		}
	}

	// ---------------------------------------------------------------- miner threads

	void miner_thread(size_t index)
	{
		PowHasher hasher;
		uint8_t header[HEADER_SIZE];
		uint32_t nonce = 0;
		while (!stop) {
			Work w;
			uint64_t generation;
			{
				Guard g(lock);
				if (!have_template || (index >= works.size())) {
					generation = 0;
				}
				else {
					w = works[index];
					generation = work_generation;
				}
			}
			if (!generation) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
			if (opt.fast) {
				bool rebuild;
				{
					std::shared_lock<std::shared_mutex> r(dataset_lock);
					rebuild = (dataset.generation() == 0) || (dataset.key() != w.tmpl.seed_hash);
				}
				if (rebuild) {
					std::unique_lock<std::shared_mutex> x(dataset_lock);
					if (!dataset.prepare(w.tmpl.seed_hash, static_cast<unsigned>(std::max<size_t>(1, std::thread::hardware_concurrency())))) {
						stop = true;
						return;
					}
				}
			}
			else if (!hasher.set_key(w.tmpl.seed_hash)) {
				stop = true;
				return;
			}
			const difficulty_type share_diff(w.share_difficulty);
			bool found = false;
			while (!found && !stop && (current_generation == generation)) {
				// batches of 16 hashes; in fast mode under the dataset's shared lock (it may be rebuilt in between)
				std::shared_lock<std::shared_mutex> r(dataset_lock, std::defer_lock);
				if (opt.fast) {
					r.lock();
					if ((dataset.key() != w.tmpl.seed_hash) || !hasher.use_dataset(dataset)) break;
				}
				for (int i = 0; i < 16; ++i) {
					make_header(w.tmpl, w.merkle_root, static_cast<uint32_t>(w.timestamp), nonce++, header);
					const hash pow = hasher.calculate(header, sizeof(header));
					++hashes;
					if (share_diff.check_pow(pow)) {
						if (r.owns_lock()) r.unlock();
						on_own_share(index, generation, header, pow);
						found = true;
						break;
					}
				}
			}
		}
	}
};

} // namespace

int run_pool(const PoolOptions& opt)
{
	Pool pool;
	pool.opt = opt;

	SideChainParams p;
	p.share_time = opt.share_time;
	p.min_difficulty = opt.min_difficulty;
	pool.chain = SideChain(p);

	bool net_known = false;
	if (!opt.network.empty()) {
		if (opt.network == "main") pool.net = Network::MAIN;
		else if (opt.network == "testnet") pool.net = Network::TEST;
		else if (opt.network == "regtest") pool.net = Network::REGTEST;
		else { fprintf(stderr, "unknown network %s\n", opt.network.c_str()); return 1; }
		net_known = true;
	}
	for (const std::string& a : opt.miners) {
		std::vector<uint8_t> script;
		bool ok = false;
		for (Network n : { Network::REGTEST, Network::TEST, Network::MAIN }) {
			if ((!net_known || (n == pool.net)) && p2mr_script_from_address(a, n, script)) {
				pool.net = n;
				net_known = ok = true;
				break;
			}
		}
		if (!ok) {
			fprintf(stderr, "not a P2MR address (or not the same network as the others): %s\n", a.c_str());
			return 1;
		}
		pool.miner_scripts.push_back(script);
	}
	if (pool.miner_scripts.empty() && opt.peers.empty() && !opt.p2p_port && opt.stratum.empty()) {
		fprintf(stderr, "no miner addresses and no pool network: nothing to do\n");
		return 1;
	}
	if (!net_known) {
		fprintf(stderr, "a node without miners needs --network main|testnet|regtest\n");
		return 1;
	}

	pool.log.open(opt.log_file, std::ios::out | std::ios::trunc);
	if (!pool.log.good()) {
		fprintf(stderr, "cannot write %s\n", opt.log_file.c_str());
		return 1;
	}

	uv_loop_t* loop = uv_default_loop_checked();
	loop->data = nullptr;
	GetLoopUserData(loop);
	pool.loop = loop;
	const bool own_tracker = (bkg_jobs_tracker == nullptr);
	if (own_tracker) bkg_jobs_tracker = new BackgroundJobTracker();

	PqchdClient client(opt.pqchd,
		[&pool](const pqchd::Template& t, bool) { pool.on_template(t); },
		[](const std::string&) {});
	pool.client = &client;
	if (!client.start(loop)) return 1;

	{
		Pool::Guard g(pool.lock);
		pool.wait_for_peers = !opt.peers.empty();
		pool.started_at = seconds_since_epoch();
	}
	// the end of syncing may come from a timeout, not from a share: check every few seconds
	uv_timer_init(loop, &pool.sync_timer);
	pool.sync_timer.data = &pool;
	uv_timer_start(&pool.sync_timer, [](uv_timer_t* t) {
		Pool* pl = static_cast<Pool*>(t->data);
		Pool::Guard g(pl->lock);
		if (pl->was_syncing && pl->have_template && !pl->syncing()) pl->rebuild_works();
	}, 5000, 5000);

	P2P* p2p = nullptr;
	if (opt.p2p_port || !opt.peers.empty()) {
		p2p = new P2P(loop, pool.net, opt.p2p_port, opt.peers, &pool);
		if (!p2p->start()) return 1;
		pool.p2p = p2p;
	}

	StratumServer* stratum = nullptr;
	if (!opt.stratum.empty()) {
		stratum = new StratumServer(loop, opt.stratum, &pool);
		if (!stratum->start()) return 1;
		Pool::Guard g(pool.lock);
		pool.stratum = stratum;
	}

	std::vector<std::thread> threads;
	for (size_t i = 0; i < pool.miner_scripts.size(); ++i) {
		threads.emplace_back([&pool, i]() { pool.miner_thread(i); });
	}

	// stop after opt.seconds (0 = run until killed); then let the last block submissions finish
	uv_timer_t stopper{};
	uv_timer_init(loop, &stopper);
	stopper.data = &pool;
	if (opt.seconds) {
		uv_timer_start(&stopper, [](uv_timer_t* t) {
			Pool* pl = static_cast<Pool*>(t->data);
			pl->stop = true;
			pl->client->stop();
			if (pl->p2p) pl->p2p->stop();
			if (pl->stratum) pl->stratum->stop();
			uv_timer_stop(&pl->sync_timer);
			uv_close(reinterpret_cast<uv_handle_t*>(&pl->sync_timer), nullptr);
			uv_close(reinterpret_cast<uv_handle_t*>(t), nullptr);
			uv_timer_t* closer = new uv_timer_t{};
			uv_timer_init(t->loop, closer);
			uv_timer_start(closer, [](uv_timer_t* c) {
				DeleteLoopUserData(c->loop);
				uv_close(reinterpret_cast<uv_handle_t*>(c), [](uv_handle_t* h) { delete reinterpret_cast<uv_timer_t*>(h); });
			}, 3000, 0);
		}, opt.seconds * 1000, 0);
	}

	uv_run(loop, UV_RUN_DEFAULT);
	for (std::thread& t : threads) t.join();
	delete p2p;
	{
		Pool::Guard g(pool.lock);
		pool.stratum = nullptr;
	}
	delete stratum;

	if (own_tracker) {
		delete bkg_jobs_tracker;
		bkg_jobs_tracker = nullptr;
	}

	const Share* tip = pool.chain.tip();
	printf("pqch-pool: %llu hashes, %llu own shares, %llu received, tip height %llu, %llu blocks found, %llu accepted, %llu rejected\n",
		static_cast<unsigned long long>(pool.hashes.load()), static_cast<unsigned long long>(pool.shares_found),
		static_cast<unsigned long long>(pool.shares_received), static_cast<unsigned long long>(tip ? tip->height : 0),
		static_cast<unsigned long long>(pool.blocks_found), static_cast<unsigned long long>(pool.blocks_accepted),
		static_cast<unsigned long long>(pool.blocks_rejected));
	if (tip) printf("pqch-pool: tip %s\n", rpc_hex(tip->id).c_str());
	return (pool.blocks_rejected == 0) ? 0 : 1;
}

} // namespace pqch
} // namespace p2pool
