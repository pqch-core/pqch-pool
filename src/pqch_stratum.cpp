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
#include "pqch_stratum.h"
#include "rapidjson_wrapper.h"
#include <algorithm>

LOG_CATEGORY(PqchStratum)

namespace p2pool {
namespace pqch {

namespace {

const char HEX[] = "0123456789abcdef";

std::string to_hex(const uint8_t* p, size_t n)
{
	std::string s;
	s.reserve(n * 2);
	for (size_t i = 0; i < n; ++i) { s += HEX[p[i] >> 4]; s += HEX[p[i] & 15]; }
	return s;
}

bool from_hex(const char* s, size_t len, uint8_t* out, size_t n)
{
	if (len != n * 2) return false;
	for (size_t i = 0; i < n; ++i) {
		int v = 0;
		for (int k = 0; k < 2; ++k) {
			const char c = s[i * 2 + k];
			int d;
			if ((c >= '0') && (c <= '9')) d = c - '0';
			else if ((c >= 'a') && (c <= 'f')) d = c - 'a' + 10;
			else if ((c >= 'A') && (c <= 'F')) d = c - 'A' + 10;
			else return false;
			v = v * 16 + d;
		}
		out[i] = static_cast<uint8_t>(v);
	}
	return true;
}

// JSON string escaping for the few strings we echo (ids, error messages)
std::string json_str(const std::string& s)
{
	std::string r = "\"";
	for (char c : s) {
		if ((c == '"') || (c == '\\')) { r += '\\'; r += c; }
		else if (static_cast<unsigned char>(c) < 0x20) r += ' ';
		else r += c;
	}
	return r + "\"";
}

} // namespace

void set_header_nonce(uint8_t (&header)[HEADER_SIZE], uint32_t nonce)
{
	for (int i = 0; i < 4; ++i) header[NONCE_OFFSET + i] = static_cast<uint8_t>(nonce >> (8 * i));
}

std::string stratum_job_json(const StratumJob& job)
{
	return R"({"job_id":)" + json_str(job.job_id) + R"(,"blob":")" + to_hex(job.blob, HEADER_SIZE) + R"(","difficulty":)" +
		std::to_string(job.difficulty) + R"(,"seed_hash":")" + to_hex(job.seed_hash.h, HASH_SIZE) + R"(","height":)" +
		std::to_string(job.height) + R"(,"algo":")" + STRATUM_ALGO + "\"}";
}

static bool parse_job_value(const rapidjson::Value& v, StratumJob& out, std::string& error)
{
	if (!v.IsObject()) { error = "job is not an object"; return false; }
	auto str = [&v](const char* name, const rapidjson::Value*& r) {
		auto it = v.FindMember(name);
		if ((it == v.MemberEnd()) || !it->value.IsString()) return false;
		r = &it->value;
		return true;
	};
	const rapidjson::Value *id, *blob, *seed, *algo;
	if (!str("job_id", id) || !str("blob", blob) || !str("seed_hash", seed) || !str("algo", algo)) { error = "job: missing field"; return false; }
	if (strcmp(algo->GetString(), STRATUM_ALGO)) { error = std::string("job: unknown algorithm ") + algo->GetString(); return false; }
	auto d = v.FindMember("difficulty");
	auto h = v.FindMember("height");
	if ((d == v.MemberEnd()) || !d->value.IsUint64() || !d->value.GetUint64() || (h == v.MemberEnd()) || !h->value.IsUint64()) {
		error = "job: bad difficulty or height";
		return false;
	}
	if ((id->GetStringLength() == 0) || (id->GetStringLength() > 64)) { error = "job: bad job_id"; return false; }
	if (!from_hex(blob->GetString(), blob->GetStringLength(), out.blob, HEADER_SIZE)) { error = "job: bad blob"; return false; }
	if (!from_hex(seed->GetString(), seed->GetStringLength(), out.seed_hash.h, HASH_SIZE)) { error = "job: bad seed_hash"; return false; }
	out.job_id = id->GetString();
	out.difficulty = d->value.GetUint64();
	out.height = h->value.GetUint64();
	return true;
}

bool parse_stratum_job(const char* json, size_t size, StratumJob& out, std::string& error)
{
	rapidjson::Document doc;
	if (doc.Parse(json, size).HasParseError()) { error = "job: bad JSON"; return false; }
	return parse_job_value(doc, out, error);
}

bool parse_stratum_job_notify(const char* line, size_t size, StratumJob& out, std::string& error)
{
	rapidjson::Document doc;
	if (doc.Parse(line, size).HasParseError() || !doc.IsObject()) { error = "job: bad JSON"; return false; }
	auto p = doc.FindMember("params");
	if (p == doc.MemberEnd()) { error = "job without params"; return false; }
	return parse_job_value(p->value, out, error);
}

// ---------------------------------------------------------------- server

struct StratumServer::Session
{
	StratumServer* owner = nullptr;
	uint64_t id = 0;
	std::string address;          // remote address
	uv_tcp_t tcp{};
	bool logged_in = false;
	bool closing = false;
	std::string rbuf;
	uint64_t connected_at = 0;
	uint64_t last_message = 0;
	uint64_t window_start = 0;
	uint32_t window_count = 0;
};

StratumServer::StratumServer(uv_loop_t* loop, const std::string& listen, StratumHandler* handler)
	: m_loop(loop), m_listen(listen), m_handler(handler)
{
}

StratumServer::~StratumServer()
{
	stop();
}

bool StratumServer::start()
{
	const size_t colon = m_listen.rfind(':');
	if (colon == std::string::npos) { LOGERR(0, "stratum: listen address must be ip:port, got " << m_listen); return false; }
	std::string host = m_listen.substr(0, colon);
	if (!host.empty() && (host.front() == '[') && (host.back() == ']')) host = host.substr(1, host.size() - 2);
	const int port = atoi(m_listen.c_str() + colon + 1);
	sockaddr_storage addr{};
	if ((port <= 0) || (port > 65535) ||
		(uv_ip4_addr(host.c_str(), port, reinterpret_cast<sockaddr_in*>(&addr)) && uv_ip6_addr(host.c_str(), port, reinterpret_cast<sockaddr_in6*>(&addr)))) {
		LOGERR(0, "stratum: bad listen address " << m_listen);
		return false;
	}
	uv_tcp_init(m_loop, &m_server);
	m_server.data = this;
	int err = uv_tcp_bind(&m_server, reinterpret_cast<const sockaddr*>(&addr), 0);
	if (!err) err = uv_listen(reinterpret_cast<uv_stream_t*>(&m_server), 64, on_connection);
	if (err) {
		LOGERR(0, "stratum: cannot listen on " << m_listen << ": " << uv_err_name(err));
		uv_close(reinterpret_cast<uv_handle_t*>(&m_server), nullptr);
		return false;
	}
	m_serverStarted = true;

	uv_timer_init(m_loop, &m_timer);
	m_timer.data = this;
	uv_timer_start(&m_timer, [](uv_timer_t* t) {
		StratumServer* self = static_cast<StratumServer*>(t->data);
		const uint64_t now = seconds_since_epoch();
		std::vector<Session*> idle, no_login;
		for (Session* s : self->m_sessions) {
			if (s->last_message + IDLE_SECONDS < now) idle.push_back(s);
			else if (!s->logged_in && (s->connected_at + LOGIN_SECONDS < now)) no_login.push_back(s);
		}
		for (Session* s : idle) self->close_session(s, "idle");
		for (Session* s : no_login) self->close_session(s, "no login");
		for (auto it = self->m_bans.begin(); it != self->m_bans.end();) it = (it->second <= now) ? self->m_bans.erase(it) : std::next(it);
		for (auto it = self->m_connections.begin(); it != self->m_connections.end();) {
			it = (it->second.first + 1 < now / 60) ? self->m_connections.erase(it) : std::next(it);
		}
	}, 10000, 10000);
	m_timerStarted = true;
	LOGINFO(1, "stratum: listening for miners on " << m_listen);
	return true;
}

void StratumServer::stop()
{
	if (m_stopped) return;
	m_stopped = true;
	if (m_timerStarted) {
		uv_timer_stop(&m_timer);
		uv_close(reinterpret_cast<uv_handle_t*>(&m_timer), nullptr);
		m_timerStarted = false;
	}
	if (m_serverStarted) {
		uv_close(reinterpret_cast<uv_handle_t*>(&m_server), nullptr);
		m_serverStarted = false;
	}
	std::vector<Session*> sessions = m_sessions;
	for (Session* s : sessions) close_session(s, nullptr);
}

size_t StratumServer::session_count() const
{
	return static_cast<size_t>(std::count_if(m_sessions.begin(), m_sessions.end(), [](const Session* s) { return s->logged_in && !s->closing; }));
}

void StratumServer::on_connection(uv_stream_t* server, int status)
{
	StratumServer* self = static_cast<StratumServer*>(server->data);
	if (status || self->m_stopped) return;
	Session* s = new Session();
	s->owner = self;
	s->id = self->m_nextSession++;
	s->connected_at = s->last_message = seconds_since_epoch();
	uv_tcp_init(self->m_loop, &s->tcp);
	s->tcp.data = s;
	self->m_sessions.push_back(s);
	if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&s->tcp))) {
		self->close_session(s, nullptr);
		return;
	}
	sockaddr_storage addr{};
	int len = sizeof(addr);
	if (!uv_tcp_getpeername(&s->tcp, reinterpret_cast<sockaddr*>(&addr), &len)) {
		char buf[64] = {};
		if (addr.ss_family == AF_INET6) uv_ip6_name(reinterpret_cast<const sockaddr_in6*>(&addr), buf, sizeof(buf));
		else uv_ip4_name(reinterpret_cast<const sockaddr_in*>(&addr), buf, sizeof(buf));
		s->address = buf;
		if (s->address.rfind("::ffff:", 0) == 0) s->address.erase(0, 7);
	}
	const char* reason = nullptr;
	if (self->m_sessions.size() > MAX_SESSIONS) reason = "too many miners";
	else self->admit(s->address, reason);
	if (reason) {
		LOGINFO(4, "stratum: refusing " << s->address << ": " << reason);
		self->close_session(s, nullptr);
		return;
	}
	uv_tcp_nodelay(&s->tcp, 1);
	uv_read_start(reinterpret_cast<uv_stream_t*>(&s->tcp),
		[](uv_handle_t*, size_t suggested, uv_buf_t* buf) {
			buf->base = static_cast<char*>(malloc(suggested));
			buf->len = buf->base ? static_cast<decltype(buf->len)>(suggested) : 0;
		},
		[](uv_stream_t* st, ssize_t n, const uv_buf_t* buf) {
			Session* ss = static_cast<Session*>(st->data);
			if (n > 0) ss->owner->on_data(ss, buf->base, static_cast<size_t>(n));
			else if (n < 0) ss->owner->close_session(ss, nullptr);
			free(buf->base);
		});
	LOGINFO(4, "stratum: connection from " << s->address);
}

// A new connection from `ip`: refused while the address is banned, has MAX_SESSIONS_PER_IP connections already
// or opened too many in the current minute.
bool StratumServer::admit(const std::string& ip, const char*& reason)
{
	const uint64_t now = seconds_since_epoch();
	auto ban = m_bans.find(ip);
	if ((ban != m_bans.end()) && (ban->second > now)) { reason = "address refused for a while"; return false; }
	auto& rate = m_connections[ip];
	if (rate.first != now / 60) rate = { now / 60, 0 };
	if (++rate.second > MAX_CONNECTIONS_PER_IP_PER_MINUTE) { reason = "too many connections per minute"; return false; }
	size_t n = 0;
	for (const Session* t : m_sessions) if (!t->closing && (t->address == ip)) ++n;
	if (n > MAX_SESSIONS_PER_IP) { reason = "too many connections from this address"; return false; } // n includes the new one
	return true;
}

void StratumServer::close_session(Session* s, const char* reason, bool ban)
{
	if (s->closing) return;
	s->closing = true;
	if (ban && !s->address.empty()) m_bans[s->address] = seconds_since_epoch() + BAN_SECONDS;
	if (reason) LOGINFO(3, "stratum: closing miner " << s->address << ": " << reason);
	m_sessions.erase(std::remove(m_sessions.begin(), m_sessions.end(), s), m_sessions.end());
	if (s->logged_in) m_handler->on_disconnect(s->id);
	uv_close(reinterpret_cast<uv_handle_t*>(&s->tcp), [](uv_handle_t* h) { delete static_cast<Session*>(h->data); });
}

void StratumServer::write(Session* s, const std::string& line)
{
	if (s->closing) return;
	struct WriteReq { uv_write_t req; std::string data; };
	WriteReq* w = new WriteReq();
	w->data = line + "\n";
	uv_buf_t buf = uv_buf_init(&w->data[0], static_cast<unsigned int>(w->data.size()));
	if (uv_write(&w->req, reinterpret_cast<uv_stream_t*>(&s->tcp), &buf, 1, [](uv_write_t* req, int) { delete reinterpret_cast<WriteReq*>(req); })) {
		delete w;
	}
}

void StratumServer::reply(Session* s, const std::string& id, const std::string& result, const std::string& error)
{
	if (error.empty()) write(s, R"({"id":)" + id + R"(,"jsonrpc":"2.0","error":null,"result":)" + result + "}");
	else write(s, R"({"id":)" + id + R"(,"jsonrpc":"2.0","error":{"code":-1,"message":)" + json_str(error) + R"(},"result":null})");
}

void StratumServer::send_job(uint64_t session, const std::string& job_json)
{
	auto it = std::find_if(m_sessions.begin(), m_sessions.end(), [session](const Session* s) { return s->id == session; });
	if ((it == m_sessions.end()) || !(*it)->logged_in) return;
	write(*it, R"({"jsonrpc":"2.0","method":"job","params":)" + job_json + "}");
}

void StratumServer::disconnect(uint64_t session, const char* reason)
{
	auto it = std::find_if(m_sessions.begin(), m_sessions.end(), [session](const Session* s) { return s->id == session; });
	if (it != m_sessions.end()) close_session(*it, reason, true);
}

void StratumServer::on_data(Session* s, const char* data, size_t size)
{
	if (s->closing) return;
	s->rbuf.append(data, size);
	size_t pos = 0;
	for (;;) {
		const size_t nl = s->rbuf.find('\n', pos);
		if (nl == std::string::npos) break;
		const uint64_t now = seconds_since_epoch();
		if (now != s->window_start) { s->window_start = now; s->window_count = 0; }
		if (++s->window_count > MAX_MESSAGES_PER_SECOND) { close_session(s, "too many messages", true); return; }
		s->last_message = now;
		if ((nl > pos) && !handle_line(s, s->rbuf.data() + pos, nl - pos)) { close_session(s, "invalid message", true); return; }
		if (s->closing) return;
		pos = nl + 1;
	}
	s->rbuf.erase(0, pos);
	if (s->rbuf.size() > MAX_LINE) close_session(s, "line too long", true);
}

bool StratumServer::handle_line(Session* s, const char* line, size_t size)
{
	if (size > MAX_LINE) return false;
	rapidjson::Document doc;
	if (doc.Parse(line, size).HasParseError() || !doc.IsObject()) return false;

	// the request id is echoed back: a number or a short string
	std::string id = "null";
	auto idv = doc.FindMember("id");
	if (idv != doc.MemberEnd()) {
		if (idv->value.IsUint64()) id = std::to_string(idv->value.GetUint64());
		else if (idv->value.IsString() && (idv->value.GetStringLength() <= 64)) id = json_str(idv->value.GetString());
	}
	auto mv = doc.FindMember("method");
	if ((mv == doc.MemberEnd()) || !mv->value.IsString()) return false;
	const std::string method = mv->value.GetString();
	auto pv = doc.FindMember("params");
	const bool have_params = (pv != doc.MemberEnd()) && pv->value.IsObject();

	if (method == "login") {
		if (s->logged_in || !have_params) return false;
		auto lv = pv->value.FindMember("login");
		if ((lv == pv->value.MemberEnd()) || !lv->value.IsString() || (lv->value.GetStringLength() > 128)) return false;
		std::string error;
		if (!m_handler->on_login(s->id, lv->value.GetString(), error)) {
			reply(s, id, std::string(), error);
			close_session(s, error.c_str());
			return true;
		}
		s->logged_in = true;
		LOGINFO(3, "stratum: miner " << s->address << " logged in (session " << s->id << ")");
		char sid[17];
		snprintf(sid, sizeof(sid), "%016llx", static_cast<unsigned long long>(s->id));
		reply(s, id, R"({"id":")" + std::string(sid) + R"(","status":"OK"})", std::string());
		return true;
	}
	if (!s->logged_in) return false;

	if (method == "submit") {
		if (!have_params) return false;
		auto jv = pv->value.FindMember("job_id");
		auto nv = pv->value.FindMember("nonce");
		if ((jv == pv->value.MemberEnd()) || !jv->value.IsString() || (nv == pv->value.MemberEnd()) || !nv->value.IsUint()) return false;
		const std::string status = m_handler->on_submit(s->id, jv->value.GetString(), nv->value.GetUint());
		if (status == "OK") reply(s, id, R"({"status":"OK"})", std::string());
		else reply(s, id, std::string(), status);
		return true;
	}
	if (method == "keepalived") {
		reply(s, id, R"({"status":"KEEPALIVED"})", std::string());
		return true;
	}
	reply(s, id, std::string(), "unknown method");
	return true;
}

} // namespace pqch
} // namespace p2pool
