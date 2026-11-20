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

// PQCH phase A: one pool node. It takes work from pqchd, keeps the share chain, runs one built-in miner
// thread per miner address, adds the shares they find and submits the ones that are PQCH blocks. Every
// share and block is written as a JSON line to the log file, so that a test can check the coinbases
// against the share chain independently.

#include "common.h"
#include "pqch_pool.h"
#include "pqchd_client.h"
#include "pqch_sidechain.h"
#include "pqch_pow.h"
#include "json_rpc_request.h"
#include <atomic>
#include <fstream>
#include <mutex>
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

struct Pool
{
	PoolOptions opt;
	Network net = Network::REGTEST;
	std::vector<std::vector<uint8_t>> miner_scripts;

	std::mutex lock;                      // protects everything below
	SideChain chain{ SideChainParams() };
	bool have_template = false;
	pqchd::Template tmpl;
	std::vector<Work> works;              // one per miner thread
	uint64_t work_generation = 0;
	uint64_t shares_found = 0, blocks_found = 0, blocks_accepted = 0, blocks_rejected = 0;
	std::ofstream log;

	std::atomic<uint64_t> current_generation{ 0 };
	std::atomic<bool> stop{ false };
	std::atomic<uint64_t> hashes{ 0 };

	PqchdClient* client = nullptr;
	uv_loop_t* loop = nullptr;

	std::string address(const std::vector<uint8_t>& script) const
	{
		std::string a;
		address_from_p2mr_script(script, net, a);
		return a;
	}

	// lock held
	void rebuild_works()
	{
		if (!have_template) return;
		++work_generation;
		works.resize(miner_scripts.size());
		for (size_t i = 0; i < miner_scripts.size(); ++i) {
			if (!make_work(chain, tmpl, miner_scripts[i], i, work_generation, static_cast<uint64_t>(time(nullptr)), works[i])) {
				LOGERR(0, "could not build the work for miner " << i);
			}
		}
		current_generation = work_generation;
	}

	void on_template(const pqchd::Template& t)
	{
		std::lock_guard<std::mutex> g(lock);
		tmpl = t;
		have_template = true;
		rebuild_works();
	}

	// miner thread: a header met the share difficulty
	void on_share(size_t miner, uint64_t generation, const uint8_t (&header)[HEADER_SIZE], const hash& pow)
	{
		std::vector<uint8_t> block;
		std::string json;
		{
			std::lock_guard<std::mutex> g(lock);
			if (generation != work_generation) return; // stale: the work changed meanwhile
			const Work& w = works[miner];
			const Share s = make_share(w, header, pow);
			std::string error;
			if (!chain.add(s, error)) {
				LOGWARN(0, "share refused: " << error);
				return;
			}
			++shares_found;
			const bool is_block = check_pow(pow, w.tmpl.target);
			log << R"({"type":"share","id":")" << rpc_hex(s.id) << R"(","parent":")" << rpc_hex(s.parent)
				<< R"(","height":)" << s.height << R"(,"difficulty":)" << s.difficulty << R"(,"miner":")" << address(s.miner_script)
				<< R"(","main_height":)" << s.main_height << R"(,"block":)" << (is_block ? "true" : "false") << "}\n";
			LOGINFO(3, "share " << s.height << " by miner " << miner << ", difficulty " << s.difficulty << (is_block ? " - a PQCH block!" : ""));

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
			}
			if (is_block) {
				// Our block becomes the main-chain tip: stop mining on the old one until pqchd hands out the
				// next template (asked for right away below), or the miners would only find stale blocks.
				have_template = false;
				++work_generation;
				current_generation = work_generation;
			}
			else {
				rebuild_works(); // new tip of the share chain: everybody mines on it
			}
		}

		if (!block.empty()) {
			std::vector<uint8_t>* b = new std::vector<uint8_t>(std::move(block));
			CallOnLoop(loop, [this, b, json]() {
				client->submit_block(std::move(*b), [this, json](bool accepted, const std::string& reason) {
					{
						std::lock_guard<std::mutex> g(lock);
						(accepted ? blocks_accepted : blocks_rejected)++;
						log << json << R"(,"accepted":)" << (accepted ? "true" : "false") << R"(,"reason":")" << reason << "\"}\n";
						log.flush();
						if (!accepted && !have_template) {
							// the tip did not change, so no new template will come: go back to the current one
							have_template = true;
							rebuild_works();
						}
					}
					client->poll_now();
				});
				delete b;
			});
		}
	}

	void miner_thread(size_t index)
	{
		PowHasher hasher;
		uint8_t header[HEADER_SIZE];
		uint32_t nonce = 0;
		while (!stop) {
			Work w;
			uint64_t generation;
			{
				std::lock_guard<std::mutex> g(lock);
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
			if (!hasher.set_key(w.tmpl.seed_hash)) {
				stop = true;
				return;
			}
			const difficulty_type share_diff(w.share_difficulty);
			while (!stop && (current_generation == generation)) {
				make_header(w.tmpl, w.merkle_root, static_cast<uint32_t>(w.timestamp), nonce++, header);
				const hash pow = hasher.calculate(header, sizeof(header));
				++hashes;
				if (share_diff.check_pow(pow)) {
					on_share(index, generation, header, pow);
					break;
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
	if (pool.miner_scripts.empty()) {
		fprintf(stderr, "no miner addresses\n");
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

	std::vector<std::thread> threads;
	for (size_t i = 0; i < pool.miner_scripts.size(); ++i) {
		threads.emplace_back([&pool, i]() { pool.miner_thread(i); });
	}

	// stop after opt.seconds; then let the last block submissions finish
	uv_timer_t stopper{};
	uv_timer_init(loop, &stopper);
	stopper.data = &pool;
	uv_timer_start(&stopper, [](uv_timer_t* t) {
		Pool* pl = static_cast<Pool*>(t->data);
		pl->stop = true;
		pl->client->stop();
		uv_close(reinterpret_cast<uv_handle_t*>(t), nullptr);
		uv_timer_t* closer = new uv_timer_t{};
		uv_timer_init(t->loop, closer);
		uv_timer_start(closer, [](uv_timer_t* c) {
			DeleteLoopUserData(c->loop);
			uv_close(reinterpret_cast<uv_handle_t*>(c), [](uv_handle_t* h) { delete reinterpret_cast<uv_timer_t*>(h); });
		}, 3000, 0);
	}, opt.seconds * 1000, 0);

	uv_run(loop, UV_RUN_DEFAULT);
	for (std::thread& t : threads) t.join();

	if (own_tracker) {
		delete bkg_jobs_tracker;
		bkg_jobs_tracker = nullptr;
	}

	printf("pqch-pool: %llu hashes, %llu shares (chain height %llu), %llu blocks found, %llu accepted, %llu rejected\n",
		static_cast<unsigned long long>(pool.hashes.load()), static_cast<unsigned long long>(pool.shares_found),
		static_cast<unsigned long long>(pool.chain.size()), static_cast<unsigned long long>(pool.blocks_found),
		static_cast<unsigned long long>(pool.blocks_accepted), static_cast<unsigned long long>(pool.blocks_rejected));
	return (pool.blocks_rejected == 0) ? 0 : 1;
}

} // namespace pqch
} // namespace p2pool
