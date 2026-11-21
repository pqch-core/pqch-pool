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

// The PQCH miner (phase C): stratum client on the libuv loop, RandomX v2 in worker threads.

#include "common.h"
#include "pqch_miner.h"
#include "pqch_stratum.h"
#include "pqch_pow.h"
#include "uv_util.h"
#include "rapidjson_wrapper.h"
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>

LOG_CATEGORY(PqchMiner)

namespace p2pool {
namespace pqch {

namespace {

constexpr uint64_t RECONNECT_SECONDS = 10;
constexpr uint64_t KEEPALIVE_SECONDS = 60;
constexpr uint64_t REPORT_SECONDS = 60;
constexpr size_t MAX_LINE = 4096;

struct Miner
{
	MinerOptions opt;
	uv_loop_t* loop = nullptr;

	// connection (loop thread)
	uv_tcp_t* tcp = nullptr;              // null: not connected
	uv_connect_t connect_req{};
	bool logged_in = false;
	std::string rbuf;
	uint64_t next_id = 1;
	uint64_t last_connect = 0;
	uint64_t last_keepalive = 0;
	uv_timer_t timer{};
	bool stopping = false;
	bool login_refused = false;

	// current job (shared with the threads)
	std::mutex job_lock;
	StratumJob job;
	std::atomic<uint64_t> job_generation{ 0 }; // 0: no job
	PowDataset dataset;
	std::shared_mutex dataset_lock;

	std::atomic<bool> stop{ false };
	std::atomic<uint64_t> hashes{ 0 };
	uint64_t found = 0, accepted = 0, stale = 0, rejected = 0, last_hashes = 0, last_report = 0;

	// ---------------------------------------------------------------- connection

	void connect()
	{
		if (tcp || stopping) return;
		last_connect = seconds_since_epoch();
		const size_t colon = opt.pool.rfind(':');
		std::string host = opt.pool.substr(0, colon);
		if (!host.empty() && (host.front() == '[') && (host.back() == ']')) host = host.substr(1, host.size() - 2);
		const int port = atoi(opt.pool.c_str() + colon + 1);
		sockaddr_storage addr{};
		if (uv_ip4_addr(host.c_str(), port, reinterpret_cast<sockaddr_in*>(&addr)) &&
			uv_ip6_addr(host.c_str(), port, reinterpret_cast<sockaddr_in6*>(&addr))) {
			LOGERR(0, "pool " << opt.pool << ": not an IP address");
			stop_all();
			return;
		}
		tcp = new uv_tcp_t{};
		uv_tcp_init(loop, tcp);
		tcp->data = this;
		connect_req.data = this;
		const int err = uv_tcp_connect(&connect_req, tcp, reinterpret_cast<const sockaddr*>(&addr), [](uv_connect_t* req, int status) {
			Miner* m = static_cast<Miner*>(req->data);
			if (status) {
				LOGWARN(1, "cannot connect to the pool " << m->opt.pool << " (" << uv_err_name(status) << "); trying again in " << RECONNECT_SECONDS << " s");
				m->disconnect();
				return;
			}
			m->on_connected();
		});
		if (err) disconnect();
	}

	void disconnect()
	{
		if (!tcp) return;
		logged_in = false;
		rbuf.clear();
		job_generation = 0; // the threads wait for a new job
		uv_close(reinterpret_cast<uv_handle_t*>(tcp), [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
		tcp = nullptr;
	}

	void on_connected()
	{
		uv_tcp_nodelay(tcp, 1);
		uv_read_start(reinterpret_cast<uv_stream_t*>(tcp),
			[](uv_handle_t*, size_t suggested, uv_buf_t* buf) {
				buf->base = static_cast<char*>(malloc(suggested));
				buf->len = buf->base ? static_cast<decltype(buf->len)>(suggested) : 0;
			},
			[](uv_stream_t* s, ssize_t n, const uv_buf_t* buf) {
				Miner* m = static_cast<Miner*>(s->data);
				if (n > 0) m->on_data(buf->base, static_cast<size_t>(n));
				else if (n < 0) {
					LOGWARN(1, "the pool closed the connection; trying again in " << RECONNECT_SECONDS << " s");
					m->disconnect();
				}
				free(buf->base);
			});
		LOGINFO(1, "connected to the pool " << opt.pool);
		send(R"({"id":)" + std::to_string(next_id++) + R"(,"method":"login","params":{"login":")" + opt.address + R"(","agent":"pqch-miner/1"}})");
	}

	void send(const std::string& line)
	{
		if (!tcp) return;
		struct WriteReq { uv_write_t req; std::string data; };
		WriteReq* w = new WriteReq();
		w->data = line + "\n";
		uv_buf_t buf = uv_buf_init(&w->data[0], static_cast<unsigned int>(w->data.size()));
		if (uv_write(&w->req, reinterpret_cast<uv_stream_t*>(tcp), &buf, 1, [](uv_write_t* req, int) { delete reinterpret_cast<WriteReq*>(req); })) {
			delete w;
		}
	}

	void on_data(const char* data, size_t size)
	{
		rbuf.append(data, size);
		size_t pos = 0;
		for (;;) {
			const size_t nl = rbuf.find('\n', pos);
			if (nl == std::string::npos) break;
			if (nl > pos) handle_line(rbuf.data() + pos, nl - pos);
			if (!tcp) return;
			pos = nl + 1;
		}
		rbuf.erase(0, pos);
		if (rbuf.size() > MAX_LINE) {
			LOGWARN(1, "the pool sent a line that is too long");
			disconnect();
		}
	}

	void handle_line(const char* line, size_t size)
	{
		rapidjson::Document doc;
		if (doc.Parse(line, size).HasParseError() || !doc.IsObject()) {
			LOGWARN(1, "the pool sent bad JSON");
			disconnect();
			return;
		}
		auto method = doc.FindMember("method");
		if ((method != doc.MemberEnd()) && method->value.IsString() && !strcmp(method->value.GetString(), "job")) {
			StratumJob j;
			std::string error;
			if (!parse_stratum_job_notify(line, size, j, error)) { LOGWARN(1, error); return; }
			{
				std::lock_guard<std::mutex> g(job_lock);
				job = j;
			}
			++job_generation;
			LOGINFO(4, "new job " << j.job_id << ": height " << j.height << ", share difficulty " << j.difficulty);
			return;
		}

		// a reply: login or submit
		auto error = doc.FindMember("error");
		const bool ok = (error == doc.MemberEnd()) || error->value.IsNull();
		std::string message;
		if (!ok && error->value.IsObject()) {
			auto msg = error->value.FindMember("message");
			if ((msg != error->value.MemberEnd()) && msg->value.IsString()) message = msg->value.GetString();
		}
		auto result = doc.FindMember("result");
		const bool keepalive = ok && (result != doc.MemberEnd()) && result->value.IsObject() && result->value.HasMember("status") &&
			result->value["status"].IsString() && !strcmp(result->value["status"].GetString(), "KEEPALIVED");
		if (keepalive) return;

		if (!logged_in) {
			if (!ok) {
				LOGERR(0, "the pool refused the login: " << message);
				login_refused = true;
				stop_all();
				return;
			}
			logged_in = true;
			LOGINFO(1, "logged in; rewards go to " << opt.address);
			return;
		}
		if (ok) {
			++accepted;
			LOGINFO(1, "share accepted by the pool (" << accepted << " accepted)");
		}
		else if ((message == "stale job") || (message == "node busy")) {
			++stale;
			LOGINFO(2, "share arrived after the work changed (stale)");
		}
		else {
			++rejected;
			LOGWARN(1, "share rejected by the pool: " << message);
		}
	}

	// worker thread -> loop
	void submit(const std::string& job_id, uint32_t nonce)
	{
		std::string* line = new std::string(R"(,"method":"submit","params":{"job_id":")" + job_id + R"(","nonce":)" + std::to_string(nonce) + "}}");
		CallOnLoop(loop, [this, line]() {
			if (logged_in) {
				++found;
				send(R"({"id":)" + std::to_string(next_id++) + *line);
			}
			delete line;
		});
	}

	void on_timer()
	{
		const uint64_t now = seconds_since_epoch();
		if (!tcp && !stopping && (now >= last_connect + RECONNECT_SECONDS)) connect();
		if (logged_in && (now >= last_keepalive + KEEPALIVE_SECONDS)) {
			last_keepalive = now;
			send(R"({"id":)" + std::to_string(next_id++) + R"(,"method":"keepalived"})");
		}
		if (now >= last_report + REPORT_SECONDS) {
			const uint64_t h = hashes.load();
			if (last_report) {
				LOGINFO(0, "hashrate " << (h - last_hashes) / (now - last_report) << " H/s; shares: " << accepted << " accepted, "
					<< stale << " stale, " << rejected << " rejected");
			}
			last_hashes = h;
			last_report = now;
		}
	}

	void stop_all()
	{
		if (stopping) return;
		stopping = true;
		stop = true;
		disconnect();
		uv_timer_stop(&timer);
		uv_close(reinterpret_cast<uv_handle_t*>(&timer), nullptr);
		DeleteLoopUserData(loop);
	}

	// ---------------------------------------------------------------- worker threads

	void worker(unsigned index)
	{
		PowHasher hasher;
		uint8_t header[HEADER_SIZE];
		while (!stop) {
			const uint64_t gen = job_generation.load();
			if (!gen) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
			StratumJob j;
			{
				std::lock_guard<std::mutex> g(job_lock);
				j = job;
			}
			if (opt.fast) {
				bool rebuild;
				{
					std::shared_lock<std::shared_mutex> r(dataset_lock);
					rebuild = (dataset.generation() == 0) || (dataset.key() != j.seed_hash);
				}
				if (rebuild) {
					std::unique_lock<std::shared_mutex> x(dataset_lock);
					if ((dataset.generation() == 0) || (dataset.key() != j.seed_hash)) {
						LOGINFO(1, "preparing the RandomX dataset (about a minute)");
						if (!dataset.prepare(j.seed_hash, std::max(1u, std::thread::hardware_concurrency()))) {
							LOGERR(0, "cannot allocate the RandomX dataset (about 2.1 GiB); run without --fast");
							stop = true;
							return;
						}
					}
				}
			}
			else if (!hasher.set_key(j.seed_hash)) {
				LOGERR(0, "cannot allocate the RandomX cache");
				stop = true;
				return;
			}
			memcpy(header, j.blob, HEADER_SIZE);
			const difficulty_type diff(j.difficulty);
			// each thread takes every opt.threads-th nonce
			for (uint64_t n = index; (n <= UINT32_MAX) && !stop && (job_generation.load() == gen);) {
				std::shared_lock<std::shared_mutex> r(dataset_lock, std::defer_lock);
				if (opt.fast) {
					r.lock();
					if ((dataset.key() != j.seed_hash) || !hasher.use_dataset(dataset)) break;
				}
				for (int i = 0; (i < 16) && (n <= UINT32_MAX); ++i, n += opt.threads) {
					set_header_nonce(header, static_cast<uint32_t>(n));
					const hash pow = hasher.calculate(header, HEADER_SIZE);
					++hashes;
					if (diff.check_pow(pow)) submit(j.job_id, static_cast<uint32_t>(n));
				}
			}
			// nonces exhausted: wait for the next job
			while (!stop && (job_generation.load() == gen)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
};

} // namespace

int run_miner(const MinerOptions& opt)
{
	if ((opt.pool.rfind(':') == std::string::npos) || opt.address.empty() || !opt.threads || (opt.threads > 256)) {
		fprintf(stderr, "--pqch-miner ip:port address [--threads 1..256] [--fast] [--seconds N]\n");
		return 1;
	}
	Miner m;
	m.opt = opt;
	uv_loop_t* loop = uv_default_loop_checked();
	loop->data = nullptr;
	GetLoopUserData(loop);
	m.loop = loop;

	uv_timer_init(loop, &m.timer);
	m.timer.data = &m;
	uv_timer_start(&m.timer, [](uv_timer_t* t) { static_cast<Miner*>(t->data)->on_timer(); }, 1000, 1000);
	m.connect();

	std::vector<std::thread> threads;
	for (unsigned i = 0; i < opt.threads; ++i) threads.emplace_back([&m, i]() { m.worker(i); });
	LOGINFO(0, "PQCH miner: " << opt.threads << " thread(s), " << (opt.fast ? "fast" : "light") << " mode, pool " << opt.pool);

	uv_timer_t stopper{};
	if (opt.seconds) {
		uv_timer_init(loop, &stopper);
		stopper.data = &m;
		uv_timer_start(&stopper, [](uv_timer_t* t) {
			uv_close(reinterpret_cast<uv_handle_t*>(t), nullptr);
			static_cast<Miner*>(t->data)->stop_all();
		}, opt.seconds * 1000, 0);
	}

	uv_run(loop, UV_RUN_DEFAULT);
	m.stop = true;
	for (std::thread& t : threads) t.join();

	printf("pqch-miner: %llu hashes, %llu shares found, %llu accepted, %llu stale, %llu rejected\n",
		static_cast<unsigned long long>(m.hashes.load()), static_cast<unsigned long long>(m.found),
		static_cast<unsigned long long>(m.accepted), static_cast<unsigned long long>(m.stale), static_cast<unsigned long long>(m.rejected));
	return (m.login_refused || m.rejected) ? 1 : 0;
}

} // namespace pqch
} // namespace p2pool
