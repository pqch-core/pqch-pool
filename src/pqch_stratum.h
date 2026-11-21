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

// PQCH (phase C): stratum between a pool node and miners that do not run a node. One JSON object per line,
// in the style of the Monero stratum (login / job / submit), with PQCH's 80-byte header:
//
//   miner -> node  {"id":1,"method":"login","params":{"login":"<P2MR address>","agent":"pqch-miner/1"}}
//   node -> miner  {"id":1,"jsonrpc":"2.0","error":null,"result":{"id":"<session>","status":"OK"}}
//   node -> miner  {"jsonrpc":"2.0","method":"job","params":<job>}         (right after login, and on new work)
//   miner -> node  {"id":2,"method":"submit","params":{"id":"<session>","job_id":"<id>","nonce":<uint32>}}
//   node -> miner  {"id":2,"jsonrpc":"2.0","error":null,"result":{"status":"OK"}}    (or an error object)
//   miner -> node  {"id":3,"method":"keepalived"}                                    -> {"status":"KEEPALIVED"}
//
//   <job> = {"job_id":"<16 hex>","blob":"<160 hex: header, nonce bytes 76..79 zero>","difficulty":<uint64>,
//            "seed_hash":"<64 hex, internal byte order>","height":<main-chain height>,"algo":"rx/pqch"}
//
// The miner hashes the blob with RandomX v2 keyed with seed_hash, the nonce written little-endian at byte 76,
// and submits nonces whose hash meets `difficulty` (the share difficulty: every submit is a share of the share
// chain, paid by PPLNS to the login address). The node recomputes the hash; it never trusts the miner's.

#include "pqch_block.h"
#include "uv_util.h"
#include <map>

namespace p2pool {
namespace pqch {

constexpr const char* STRATUM_ALGO = "rx/pqch";

struct StratumJob
{
	std::string job_id;
	uint8_t blob[HEADER_SIZE] = {};
	uint64_t difficulty = 0;
	hash seed_hash;
	uint64_t height = 0;
};

std::string stratum_job_json(const StratumJob& job);
bool parse_stratum_job(const char* json, size_t size, StratumJob& out, std::string& error);  // a <job> object
bool parse_stratum_job_notify(const char* line, size_t size, StratumJob& out, std::string& error); // {"method":"job","params":<job>}
void set_header_nonce(uint8_t (&header)[HEADER_SIZE], uint32_t nonce);

class StratumHandler
{
public:
	virtual ~StratumHandler() = default;
	// Loop thread. A miner logged in with `address`: return false (and an error) to refuse it.
	virtual bool on_login(uint64_t session, const std::string& address, std::string& error) = 0;
	// Loop thread. A nonce for a job of this session: returns "OK" or an error message.
	virtual std::string on_submit(uint64_t session, const std::string& job_id, uint32_t nonce) = 0;
	virtual void on_disconnect(uint64_t session) = 0;
};

class StratumServer
{
public:
	static constexpr size_t MAX_LINE = 4096;
	static constexpr uint32_t MAX_MESSAGES_PER_SECOND = 20;
	static constexpr size_t MAX_SESSIONS = 256;
	static constexpr uint64_t IDLE_SECONDS = 600;   // no message for this long (keepalived every 60 s): bye
	// per remote address (the stratum can be public): connections at once, new connections per minute, time
	// to log in, and how long an address that broke the protocol stays refused
	static constexpr size_t MAX_SESSIONS_PER_IP = 8;
	static constexpr uint32_t MAX_CONNECTIONS_PER_IP_PER_MINUTE = 20;
	static constexpr uint64_t LOGIN_SECONDS = 30;
	static constexpr uint64_t BAN_SECONDS = 600;

	StratumServer(uv_loop_t* loop, const std::string& listen, StratumHandler* handler); // listen: "ip:port"
	~StratumServer();

	bool start();
	void stop();
	void send_job(uint64_t session, const std::string& job_json); // loop thread
	void disconnect(uint64_t session, const char* reason);         // loop thread; refuses its address for BAN_SECONDS
	size_t session_count() const;

	struct Session;

private:
	static void on_connection(uv_stream_t* server, int status);
	void on_data(Session* s, const char* data, size_t size);
	bool handle_line(Session* s, const char* line, size_t size);
	void reply(Session* s, const std::string& id, const std::string& result, const std::string& error);
	void write(Session* s, const std::string& line);
	void close_session(Session* s, const char* reason, bool ban = false);
	bool admit(const std::string& ip, const char*& reason);

	uv_loop_t* m_loop;
	std::string m_listen;
	StratumHandler* m_handler;
	uv_tcp_t m_server{};
	bool m_serverStarted = false;
	uv_timer_t m_timer{};
	bool m_timerStarted = false;
	bool m_stopped = false;
	uint64_t m_nextSession = 1;
	std::vector<Session*> m_sessions;
	std::map<std::string, uint64_t> m_bans;                                  // ip -> refused until
	std::map<std::string, std::pair<uint64_t, uint32_t>> m_connections;      // ip -> (minute, new connections)
};

} // namespace pqch
} // namespace p2pool
