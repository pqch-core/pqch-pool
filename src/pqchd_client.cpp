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
			if (!new_tip && (t.longpollid == m_lastLongpollId)) {
				return; // nothing changed
			}
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

int pqchd_check(const char* host_port, const char* credentials)
{
	PqchdClient::Config config;
	const std::string hp = host_port;
	const size_t colon = hp.rfind(':');
	if ((colon == std::string::npos) || (colon == 0)) {
		fprintf(stderr, "--pqchd-check: expected <host:port>, got \"%s\"\n", host_port);
		return 1;
	}
	config.address = hp.substr(0, colon);
	config.port = atoi(hp.c_str() + colon + 1);
	if ((config.port <= 0) || (config.port > 65535)) {
		fprintf(stderr, "--pqchd-check: invalid port in \"%s\"\n", host_port);
		return 1;
	}

	const std::string cred = credentials;
	if (!pqchd::read_cookie(cred, config.auth)) {
		const size_t c = cred.find(':');
		if ((c == std::string::npos) || (c == 0)) {
			fprintf(stderr, "--pqchd-check: \"%s\" is neither a readable cookie file nor user:password\n", credentials);
			return 1;
		}
		config.auth = pqchd::make_auth(cred.substr(0, c), cred.substr(c + 1));
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
			printf("pqchd OK: height %llu, previous block %s\n", static_cast<unsigned long long>(t.height), hash_to_rpc_hex(t.prev_hash).c_str());
			printf("  bits %08x, coinbase value %llu quanta, %zu transactions\n", t.bits, static_cast<unsigned long long>(t.coinbase_value), t.transactions.size());
			printf("  RandomX key (seed height %llu): %s\n", static_cast<unsigned long long>(t.seed_height), hash_to_rpc_hex(t.seed_hash).c_str());
			result = 0;
			finish();
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
		fprintf(stderr, "pqchd check FAILED: %s\n", last_error.empty() ? "no answer within 15 seconds" : last_error.c_str());
	}
	delete client;
	return result;
}

} // namespace p2pool
