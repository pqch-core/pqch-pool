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
#include "pqchd_client.h"
#include "json_rpc_request.h"
#include "pqch_block.h"
#include "pqch_pow.h"
#include <algorithm>

LOG_CATEGORY(Pqchd)

namespace p2pool {

namespace {

std::string hash_to_rpc_hex(const hash& h)
{
	static constexpr char d[] = "0123456789abcdef";
	std::string s;
	s.reserve(HASH_SIZE * 2);
	for (size_t i = HASH_SIZE; i-- > 0;) {
		s += d[h.h[i] >> 4];
		s += d[h.h[i] & 15];
	}
	return s;
}

} // namespace

PqchdClient::PqchdClient(const Config& config, TemplateCallback on_template, ErrorCallback on_error)
	: m_config(config)
	, m_onTemplate(std::move(on_template))
	, m_onError(std::move(on_error))
	, m_lastPrevHash()
{
}

PqchdClient::~PqchdClient()
{
	stop();
	*m_alive = false;
}

bool PqchdClient::start(uv_loop_t* loop)
{
	m_loop = loop ? loop : uv_default_loop_checked();
	m_stopped = false;

	int err = uv_timer_init(m_loop, &m_timer);
	if (err) {
		LOGERR(1, "uv_timer_init failed, error " << uv_err_name(err));
		return false;
	}
	m_timer.data = this;
	err = uv_timer_start(&m_timer, on_timer, 0, POLL_MS);
	if (err) {
		LOGERR(1, "uv_timer_start failed, error " << uv_err_name(err));
		uv_close(reinterpret_cast<uv_handle_t*>(&m_timer), nullptr);
		return false;
	}
	m_timerStarted = true;
	LOGINFO(1, "polling pqchd at " << m_config.address << ':' << m_config.port << " every " << POLL_MS << " ms");
	return true;
}

void PqchdClient::stop()
{
	m_stopped = true;
	if (m_timerStarted) {
		uv_timer_stop(&m_timer);
		uv_close(reinterpret_cast<uv_handle_t*>(&m_timer), nullptr);
		m_timerStarted = false;
	}
}

void PqchdClient::on_timer(uv_timer_t* timer)
{
	static_cast<PqchdClient*>(timer->data)->poll();
}

void PqchdClient::poll()
{
	if (m_stopped || m_requestInFlight) {
		return;
	}
	m_requestInFlight = true;

	std::shared_ptr<bool> alive = m_alive;

	JSONRPCRequest::call(m_config.address, m_config.port, pqchd::getblocktemplate_request(), m_config.auth, std::string(), false, std::string(),
		[this, alive](const JSONRPCRequest::CallbackData& data)
		{
			if (!*alive) {
				return;
			}
			pqchd::Template t;
			std::string error;
			if (!pqchd::parse_getblocktemplate(data.m_response.data(), data.m_response.size(), t, error)) {
				if (error != m_lastError) {
					LOGWARN(1, error);
					m_lastError = error;
				}
				if (m_onError) {
					m_onError(error);
				}
				return;
			}
			m_lastError.clear();

			const bool new_tip = !m_haveTemplate || (t.prev_hash != m_lastPrevHash);
			if (!new_tip && (t.longpollid == m_lastLongpollId) && !m_forceNext) {
				return; // nothing changed
			}
			m_forceNext = false;
			m_haveTemplate = true;
			m_lastPrevHash = t.prev_hash;
			m_lastLongpollId = t.longpollid;
			++m_templatesReceived;

			LOGINFO(new_tip ? 3 : 5, "new template: height " << t.height << ", " << t.transactions.size() << " transactions, coinbase value " << t.coinbase_value << (new_tip ? " (new tip)" : ""));
			if (m_onTemplate) {
				m_onTemplate(t, new_tip);
			}
		},
		[this, alive](const JSONRPCRequest::CallbackData& data)
		{
			if (!*alive) {
				return;
			}
			m_requestInFlight = false;
			if (!data.m_error.empty()) {
				if ((data.m_error.find("HTTP error 401") != std::string::npos) && !m_config.cookie_file.empty()) {
					std::string auth;
					if (pqchd::read_cookie(m_config.cookie_file, auth) && (auth != m_config.auth)) {
						LOGINFO(1, "pqchd has a new cookie (restarted?): using it");
						m_config.auth = auth;
						m_forceNext = true;
						return;
					}
				}
				m_forceNext = true;
				const std::string error = "getblocktemplate request to pqchd failed: " + data.m_error;
				if (error != m_lastError) {
					LOGWARN(1, error);
					m_lastError = error;
				}
				if (m_onError) {
					m_onError(error);
				}
			}
		},
		m_loop);
}

void PqchdClient::submit_block(std::vector<uint8_t>&& block, SubmitCallback cb)
{
	std::shared_ptr<bool> alive = m_alive;
	std::shared_ptr<bool> answered = std::make_shared<bool>(false);
	const std::string req = pqchd::submitblock_request(block.data(), block.size());

	JSONRPCRequest::call(m_config.address, m_config.port, req, m_config.auth, std::string(), false, std::string(),
		[cb, answered](const JSONRPCRequest::CallbackData& data)
		{
			std::string reason;
			const bool accepted = pqchd::parse_submitblock(data.m_response.data(), data.m_response.size(), reason);
			if (accepted) {
				LOGINFO(0, log::LightGreen() << "block accepted by pqchd");
			}
			else {
				LOGWARN(0, "block rejected by pqchd: " << reason);
			}
			*answered = true;
			if (cb) {
				cb(accepted, reason);
			}
		},
		[cb, answered](const JSONRPCRequest::CallbackData& data)
		{
			if (!*answered) {
				const std::string reason = data.m_error.empty() ? std::string("no answer") : data.m_error;
				LOGERR(0, "submitblock to pqchd failed: " << reason);
				if (cb) {
					cb(false, reason);
				}
			}
		},
		m_loop);
	(void)alive;
}

void PqchdClient::get_block_hash(uint64_t height, std::function<void(bool, const hash&)> cb)
{
	std::shared_ptr<bool> answered = std::make_shared<bool>(false);
	const std::string req = R"({"jsonrpc":"1.0","id":"pqch-pool","method":"getblockhash","params":[)" + std::to_string(height) + "]}";
	JSONRPCRequest::call(m_config.address, m_config.port, req, m_config.auth, std::string(), false, std::string(),
		[cb, answered](const JSONRPCRequest::CallbackData& data)
		{
			*answered = true;
			hash h;
			const std::string r(data.m_response.begin(), data.m_response.end());
			const size_t p = r.find(R"("result":")");
			if ((p == std::string::npos) || (p + 10 + 64 > r.size())) { cb(false, h); return; }
			hash shown;
			if (!from_hex(r.c_str() + p + 10, 64, shown)) { cb(false, h); return; }
			for (size_t i = 0; i < HASH_SIZE; ++i) h.h[i] = shown.h[HASH_SIZE - 1 - i];
			cb(true, h);
		},
		[cb, answered](const JSONRPCRequest::CallbackData&)
		{
			if (!*answered) cb(false, hash());
		},
		m_loop);
}

bool parse_pqchd_connection(const char* host_port, const char* credentials, PqchdClient::Config& config, std::string& error)
{
	const std::string hp = host_port;
	const size_t colon = hp.rfind(':');
	if ((colon == std::string::npos) || (colon == 0)) {
		error = "expected <host:port>, got \"" + hp + "\"";
		return false;
	}
	config.address = hp.substr(0, colon);
	config.port = atoi(hp.c_str() + colon + 1);
	if ((config.port <= 0) || (config.port > 65535)) {
		error = "invalid port in \"" + hp + "\"";
		return false;
	}
	const std::string cred = credentials;
	if (pqchd::read_cookie(cred, config.auth)) {
		config.cookie_file = cred;
	}
	else {
		const size_t c = cred.find(':');
		if ((c == std::string::npos) || (c == 0)) {
			error = "\"" + cred + "\" is neither a readable cookie file nor user:password";
			return false;
		}
		config.auth = pqchd::make_auth(cred.substr(0, c), cred.substr(c + 1));
	}
	return true;
}

// Build a block from `t` paying the "address:weight,address:weight,..." of `spec` (the first address is the
// finder), and mine it with RandomX v2 (light mode). For regtest-level difficulty only.
static bool mine_test_block(const pqchd::Template& t, const char* spec, std::vector<uint8_t>& block, std::string& error)
{
	std::vector<pqch::PayoutWeight> weights;
	pqch::Network net = pqch::Network::REGTEST;
	bool net_known = false;

	std::string list = spec;
	size_t pos = 0;
	while (pos <= list.size()) {
		size_t end = list.find(',', pos);
		if (end == std::string::npos) end = list.size();
		const std::string item = list.substr(pos, end - pos);
		pos = end + 1;
		if (item.empty()) continue;

		const size_t colon = item.find(':');
		const std::string address = item.substr(0, colon);
		const uint64_t weight = (colon == std::string::npos) ? 1 : strtoull(item.c_str() + colon + 1, nullptr, 10);

		std::vector<uint8_t> script;
		bool ok = false;
		for (pqch::Network n : { pqch::Network::REGTEST, pqch::Network::TEST, pqch::Network::MAIN }) {
			if ((!net_known || n == net) && pqch::p2mr_script_from_address(address, n, script)) {
				net = n;
				net_known = true;
				ok = true;
				break;
			}
		}
		if (!ok || !weight) {
			error = "not a P2MR address of the same network, or zero weight: " + item;
			return false;
		}
		weights.push_back({ script, weight });
	}
	if (weights.empty()) {
		error = "no addresses";
		return false;
	}

	const std::vector<pqch::Payout> payouts = pqch::split_reward(t.coinbase_value, weights, weights[0].script);
	for (const pqch::Payout& p : payouts) {
		std::string a;
		pqch::address_from_p2mr_script(p.script, net, a);
		printf("  pays %llu quanta to %s\n", static_cast<unsigned long long>(p.value), a.c_str());
	}

	hash share_id; // the pool puts the share's sidechain id here; any 32 bytes for this test
	for (size_t i = 0; i < HASH_SIZE; ++i) share_id.h[i] = static_cast<uint8_t>(0xa0 + i);

	pqch::Coinbase cb;
	if (!pqch::make_coinbase(t, payouts, share_id, 0, cb)) {
		error = "could not build the coinbase";
		return false;
	}
	std::vector<hash> txids{ cb.txid };
	for (const auto& tx : t.transactions) txids.push_back(tx.txid);
	const hash root = pqch::merkle_root(txids);

	pqch::PowHasher hasher;
	if (!hasher.set_key(t.seed_hash)) {
		error = "RandomX initialization failed";
		return false;
	}
	uint8_t header[pqch::HEADER_SIZE];
	const uint32_t time = static_cast<uint32_t>(std::max(t.curtime, t.mintime));
	for (uint32_t nonce = 0; nonce < 2000000; ++nonce) {
		pqch::make_header(t, root, time, nonce, header);
		if (pqch::check_pow(hasher.calculate(header, sizeof(header)), t.target)) {
			printf("  mined: nonce %u, block %s\n", nonce, hash_to_rpc_hex(pqch::block_hash(header)).c_str());
			block = pqch::serialize_block(header, cb, t);
			return true;
		}
	}
	error = "no nonce found (difficulty too high for a test)";
	return false;
}

static int run_tool(const char* host_port, const char* credentials, const char* mine_spec)
{
	PqchdClient::Config config;
	std::string conn_error;
	if (!parse_pqchd_connection(host_port, credentials, config, conn_error)) {
		fprintf(stderr, "pqchd: %s\n", conn_error.c_str());
		return 1;
	}

	uv_loop_t* loop = uv_default_loop_checked();
	loop->data = nullptr;
	GetLoopUserData(loop); // needed by JSONRPCRequest (CallOnLoop); deleted in finish() so that uv_run returns

	// JSONRPCRequest registers its requests with the background job tracker, which the pool creates in its
	// constructor; this check runs without a pool.
	const bool own_tracker = (bkg_jobs_tracker == nullptr);
	if (own_tracker) {
		bkg_jobs_tracker = new BackgroundJobTracker();
	}

	int result = 1;
	int failures = 0;
	bool done = false;
	std::string last_error;

	PqchdClient* client = nullptr;
	uv_timer_t deadline{};
	uv_timer_t closer{};

	// Called from inside RPC callbacks, which run from the loop user data's own callback queue: the loop
	// user data is deleted on the next loop iteration, not here.
	auto finish = [&]() {
		if (done) {
			return;
		}
		done = true;
		client->stop();
		uv_timer_stop(&deadline);
		uv_close(reinterpret_cast<uv_handle_t*>(&deadline), nullptr);
		uv_timer_init(loop, &closer);
		uv_timer_start(&closer, [](uv_timer_t* t) {
			DeleteLoopUserData(t->loop);
			uv_close(reinterpret_cast<uv_handle_t*>(t), nullptr);
		}, 0, 0);
	};

	client = new PqchdClient(config,
		[&](const pqchd::Template& t, bool)
		{
			if (done) {
				return;
			}
			printf("pqchd OK: height %llu, previous block %s\n", static_cast<unsigned long long>(t.height), hash_to_rpc_hex(t.prev_hash).c_str());
			printf("  bits %08x, coinbase value %llu quanta, %zu transactions\n", t.bits, static_cast<unsigned long long>(t.coinbase_value), t.transactions.size());
			printf("  RandomX key (seed height %llu): %s\n", static_cast<unsigned long long>(t.seed_height), hash_to_rpc_hex(t.seed_hash).c_str());
			if (!mine_spec) {
				result = 0;
				finish();
				return;
			}
			client->stop(); // one template is enough
			std::vector<uint8_t> block;
			std::string error;
			if (!mine_test_block(t, mine_spec, block, error)) {
				last_error = "mine test: " + error;
				finish();
				return;
			}
			client->submit_block(std::move(block), [&](bool accepted, const std::string& reason) {
				printf("submitblock: %s\n", accepted ? "accepted" : reason.c_str());
				result = accepted ? 0 : 1;
				if (!accepted) last_error = "block rejected by pqchd: " + reason;
				finish();
			});
		},
		[&](const std::string& error)
		{
			last_error = error;
			if (++failures >= 3) {
				finish();
			}
		});

	uv_timer_init(loop, &deadline);
	deadline.data = &finish;
	uv_timer_start(&deadline, [](uv_timer_t* t) {
		(*static_cast<decltype(finish)*>(t->data))();
	}, 15000, 0);

	if (!client->start(loop)) {
		finish();
		uv_run(loop, UV_RUN_DEFAULT);
		delete client;
		return 1;
	}
	uv_run(loop, UV_RUN_DEFAULT);

	if (own_tracker) {
		delete bkg_jobs_tracker;
		bkg_jobs_tracker = nullptr;
	}

	if (result) {
		fprintf(stderr, "FAILED: %s\n", last_error.empty() ? "no answer within 15 seconds" : last_error.c_str());
	}
	delete client;
	return result;
}

} // namespace p2pool

namespace p2pool {

int pqchd_check(const char* host_port, const char* credentials)
{
	return run_tool(host_port, credentials, nullptr);
}

int pqchd_mine_test(const char* host_port, const char* credentials, const char* payout_spec)
{
	return run_tool(host_port, credentials, payout_spec);
}

} // namespace p2pool
