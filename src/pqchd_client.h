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

#pragma once

// PQCH: keeps the pool supplied with work from pqchd.
//
// It asks pqchd for a block template once per POLL_MS (pqchd keeps its template cached and only rebuilds
// it on a new tip or after new mempool transactions, so this is cheap on a local node), reports every
// template that differs from the last one (new tip, or a new longpollid = new transactions), and submits
// found blocks. With 60-second blocks and 10-second shares, polling adds at most POLL_MS of delay and
// needs no ZMQ on the node. Everything runs on the libuv loop given to start().

#include "pqchd.h"
#include "uv_util.h"
#include <functional>
#include <memory>

namespace p2pool {

class PqchdClient : public nocopy_nomove
{
public:
	static constexpr uint64_t POLL_MS = 1000;

	struct Config
	{
		std::string address = "127.0.0.1";
		int port = 0;
		std::string auth;   // pqchd::read_cookie() or pqchd::make_auth()
	};

	typedef std::function<void(const pqchd::Template&, bool new_tip)> TemplateCallback;
	typedef std::function<void(const std::string& error)> ErrorCallback;
	typedef std::function<void(bool accepted, const std::string& reason)> SubmitCallback;

	PqchdClient(const Config& config, TemplateCallback on_template, ErrorCallback on_error);
	~PqchdClient();

	// Must be called on the thread that runs `loop`.
	bool start(uv_loop_t* loop);
	void stop();

	// Submit a serialized block (header + transactions, with witness). The callback runs on the loop thread.
	void submit_block(std::vector<uint8_t>&& block, SubmitCallback cb);

	uint64_t templates_received() const { return m_templatesReceived; }

private:
	static void on_timer(uv_timer_t* timer);
	void poll();

	Config m_config;
	TemplateCallback m_onTemplate;
	ErrorCallback m_onError;

	uv_loop_t* m_loop = nullptr;
	uv_timer_t m_timer{};
	bool m_timerStarted = false;
	bool m_requestInFlight = false;
	bool m_stopped = false;

	bool m_haveTemplate = false;
	hash m_lastPrevHash;
	std::string m_lastLongpollId;
	uint64_t m_templatesReceived = 0;
	std::string m_lastError;

	// RPC callbacks hold this flag and do nothing once the client is gone (a request may still be in flight).
	std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

// `pqch-pool --pqchd-check <host:port> <cookie file | user:password>`: connect to pqchd, print the first
// block template and exit (0 = the pool can work with this node).
int pqchd_check(const char* host_port, const char* credentials);

} // namespace p2pool
