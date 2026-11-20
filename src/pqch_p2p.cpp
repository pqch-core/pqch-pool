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
#include "pqch_p2p.h"
#include <algorithm>

LOG_CATEGORY(PqchP2P)

namespace p2pool {
namespace pqch {

namespace {
constexpr uint8_t MAGIC[4] = { 'P', 'Q', 'C', 'P' };
constexpr size_t HEADER = 9;
}

struct P2P::Peer
{
	P2P* owner = nullptr;
	uint64_t id = 0;
	bool outgoing = false;
	std::string address;            // host:port for outgoing peers, remote address for incoming
	uv_tcp_t tcp{};
	uv_connect_t connect_req{};
	bool connected = false;
	bool hello_received = false;
	bool closing = false;
	std::vector<uint8_t> rbuf;
	uint64_t window_start = 0;      // rate limit: messages in the current second
	uint32_t window_count = 0;
};

P2P::P2P(uv_loop_t* loop, Network net, uint16_t listen_port, const std::vector<std::string>& peers, P2PHandler* handler)
	: m_loop(loop), m_net(net), m_listenPort(listen_port), m_outgoing(peers), m_handler(handler)
{
}

P2P::~P2P()
{
	stop();
}

bool P2P::start()
{
	if (m_listenPort) {
		uv_tcp_init(m_loop, &m_server);
		m_server.data = this;
		sockaddr_in6 addr6{};
		uv_ip6_addr("::", m_listenPort, &addr6);
		int err = uv_tcp_bind(&m_server, reinterpret_cast<const sockaddr*>(&addr6), 0); // dual stack
		if (err) {
			sockaddr_in addr4{};
			uv_ip4_addr("0.0.0.0", m_listenPort, &addr4);
			err = uv_tcp_bind(&m_server, reinterpret_cast<const sockaddr*>(&addr4), 0);
		}
		if (!err) err = uv_listen(reinterpret_cast<uv_stream_t*>(&m_server), 32, on_connection);
		if (err) {
			LOGERR(0, "cannot listen on port " << m_listenPort << ": " << uv_err_name(err));
			uv_close(reinterpret_cast<uv_handle_t*>(&m_server), nullptr);
			return false;
		}
		m_serverStarted = true;
		LOGINFO(1, "listening for pool nodes on port " << m_listenPort);
	}
	uv_timer_init(m_loop, &m_timer);
	m_timer.data = this;
	uv_timer_start(&m_timer, on_timer, 0, RECONNECT_SECONDS * 1000);
	m_timerStarted = true;
	return true;
}

void P2P::stop()
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
	std::vector<Peer*> peers = m_peers;
	for (Peer* p : peers) close_peer(p);
}

size_t P2P::peer_count() const
{
	return static_cast<size_t>(std::count_if(m_peers.begin(), m_peers.end(), [](const Peer* p) { return p->hello_received && !p->closing; }));
}

P2P::Peer* P2P::add_peer(bool outgoing, const std::string& address)
{
	Peer* p = new Peer();
	p->owner = this;
	p->id = m_nextPeerId++;
	p->outgoing = outgoing;
	p->address = address;
	uv_tcp_init(m_loop, &p->tcp);
	p->tcp.data = p;
	m_peers.push_back(p);
	return p;
}

void P2P::on_timer(uv_timer_t* timer)
{
	static_cast<P2P*>(timer->data)->connect_outgoing();
}

void P2P::connect_outgoing()
{
	if (m_stopped) return;
	for (const std::string& a : m_outgoing) {
		const bool have = std::any_of(m_peers.begin(), m_peers.end(), [&](const Peer* p) { return p->outgoing && (p->address == a) && !p->closing; });
		if (have || (m_peers.size() >= MAX_PEERS)) continue;

		const size_t colon = a.rfind(':');
		if (colon == std::string::npos) continue;
		std::string host = a.substr(0, colon);
		if (!host.empty() && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
		const int port = atoi(a.c_str() + colon + 1);

		sockaddr_storage addr{};
		if (uv_ip4_addr(host.c_str(), port, reinterpret_cast<sockaddr_in*>(&addr)) &&
			uv_ip6_addr(host.c_str(), port, reinterpret_cast<sockaddr_in6*>(&addr))) {
			LOGWARN(1, "peer " << a << ": not an IP address");
			continue;
		}
		Peer* p = add_peer(true, a);
		p->connect_req.data = p;
		const int err = uv_tcp_connect(&p->connect_req, &p->tcp, reinterpret_cast<const sockaddr*>(&addr), [](uv_connect_t* req, int status) {
			Peer* peer = static_cast<Peer*>(req->data);
			if (status) {
				LOGINFO(4, "peer " << peer->address << ": cannot connect (" << uv_err_name(status) << ")");
				peer->owner->close_peer(peer);
				return;
			}
			peer->owner->start_peer(peer);
		});
		if (err) close_peer(p);
	}
}

void P2P::on_connection(uv_stream_t* server, int status)
{
	P2P* self = static_cast<P2P*>(server->data);
	if (status || self->m_stopped) return;
	Peer* p = self->add_peer(false, std::string());
	if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&p->tcp)) || (self->m_peers.size() > MAX_PEERS)) {
		self->close_peer(p);
		return;
	}
	sockaddr_storage addr{};
	int len = sizeof(addr);
	if (!uv_tcp_getpeername(&p->tcp, reinterpret_cast<sockaddr*>(&addr), &len)) {
		char buf[64] = {};
		if (addr.ss_family == AF_INET6) uv_ip6_name(reinterpret_cast<const sockaddr_in6*>(&addr), buf, sizeof(buf));
		else uv_ip4_name(reinterpret_cast<const sockaddr_in*>(&addr), buf, sizeof(buf));
		p->address = buf;
	}
	self->start_peer(p);
}

void P2P::start_peer(Peer* p)
{
	p->connected = true;
	uv_tcp_nodelay(&p->tcp, 1);
	uv_read_start(reinterpret_cast<uv_stream_t*>(&p->tcp),
		[](uv_handle_t*, size_t suggested, uv_buf_t* buf) {
			buf->base = static_cast<char*>(malloc(suggested));
			buf->len = buf->base ? static_cast<decltype(buf->len)>(suggested) : 0;
		},
		[](uv_stream_t* s, ssize_t n, const uv_buf_t* buf) {
			Peer* peer = static_cast<Peer*>(s->data);
			if (n > 0) peer->owner->on_data(peer, buf->base, static_cast<size_t>(n));
			else if (n < 0) peer->owner->close_peer(peer);
			free(buf->base);
		});
	send_hello(p);
	LOGINFO(3, "connected to pool node " << p->address);
}

void P2P::send_hello(Peer* p)
{
	uint8_t h[7];
	for (int i = 0; i < 4; ++i) h[i] = static_cast<uint8_t>(VERSION >> (8 * i));
	h[4] = static_cast<uint8_t>(m_net);
	h[5] = static_cast<uint8_t>(m_listenPort);
	h[6] = static_cast<uint8_t>(m_listenPort >> 8);
	send(p->id, MsgType::HELLO, h, sizeof(h));
}

void P2P::send(uint64_t peer, MsgType type, const uint8_t* payload, size_t size)
{
	auto it = std::find_if(m_peers.begin(), m_peers.end(), [peer](const Peer* p) { return p->id == peer; });
	if ((it == m_peers.end()) || !(*it)->connected || (*it)->closing || (size > MAX_PAYLOAD)) return;

	struct WriteReq { uv_write_t req; std::vector<char> data; };
	WriteReq* w = new WriteReq();
	w->data.resize(HEADER + size);
	memcpy(w->data.data(), MAGIC, 4);
	w->data[4] = static_cast<char>(type);
	for (int i = 0; i < 4; ++i) w->data[5 + i] = static_cast<char>(size >> (8 * i));
	if (size) memcpy(w->data.data() + HEADER, payload, size);
	uv_buf_t buf = uv_buf_init(w->data.data(), static_cast<unsigned int>(w->data.size()));
	if (uv_write(&w->req, reinterpret_cast<uv_stream_t*>(&(*it)->tcp), &buf, 1, [](uv_write_t* req, int) { delete reinterpret_cast<WriteReq*>(req); })) {
		delete w;
	}
}

void P2P::broadcast_share(const std::vector<uint8_t>& share, uint64_t except_peer)
{
	for (Peer* p : m_peers) {
		if ((p->id != except_peer) && p->hello_received && !p->closing) send(p->id, MsgType::SHARE, share.data(), share.size());
	}
}

void P2P::disconnect(uint64_t peer, const char* reason)
{
	auto it = std::find_if(m_peers.begin(), m_peers.end(), [peer](const Peer* p) { return p->id == peer; });
	if (it == m_peers.end()) return;
	LOGWARN(1, "disconnecting pool node " << (*it)->address << ": " << reason);
	close_peer(*it);
}

void P2P::close_peer(Peer* p)
{
	if (p->closing) return;
	p->closing = true;
	m_peers.erase(std::remove(m_peers.begin(), m_peers.end(), p), m_peers.end());
	uv_close(reinterpret_cast<uv_handle_t*>(&p->tcp), [](uv_handle_t* h) { delete static_cast<Peer*>(h->data); });
}

void P2P::on_data(Peer* p, const char* data, size_t size)
{
	if (p->closing) return;
	p->rbuf.insert(p->rbuf.end(), data, data + size);

	size_t pos = 0;
	while (p->rbuf.size() - pos >= HEADER) {
		const uint8_t* h = p->rbuf.data() + pos;
		if (memcmp(h, MAGIC, 4)) { disconnect(p->id, "bad magic"); return; }
		uint32_t len = 0;
		for (int i = 0; i < 4; ++i) len |= static_cast<uint32_t>(h[5 + i]) << (8 * i);
		if (len > MAX_PAYLOAD) { disconnect(p->id, "message too large"); return; }
		if (p->rbuf.size() - pos < HEADER + len) break;

		const uint64_t now = seconds_since_epoch();
		if (now != p->window_start) { p->window_start = now; p->window_count = 0; }
		if (++p->window_count > MAX_MESSAGES_PER_SECOND) { disconnect(p->id, "too many messages"); return; }

		const uint64_t id = p->id;
		if (!handle(p, static_cast<MsgType>(h[4]), h + HEADER, len)) {
			disconnect(id, "invalid message");
			return;
		}
		if (p->closing) return;
		pos += HEADER + len;
	}
	p->rbuf.erase(p->rbuf.begin(), p->rbuf.begin() + static_cast<std::ptrdiff_t>(pos));
}

bool P2P::handle(Peer* p, MsgType type, const uint8_t* payload, size_t size)
{
	if (!p->hello_received && (type != MsgType::HELLO)) return false;

	switch (type) {
	case MsgType::HELLO:
		if ((size != 7) || p->hello_received) return false;
		if (payload[4] != static_cast<uint8_t>(m_net)) {
			LOGWARN(1, "pool node " << p->address << " is on another network");
			return false;
		}
		p->hello_received = true;
		send(p->id, MsgType::GET_TIP, nullptr, 0);
		return true;

	case MsgType::SHARE:
		return m_handler->on_share(payload, size, p->id);

	case MsgType::GET_SHARE: {
		if (size != HASH_SIZE) return false;
		hash id;
		memcpy(id.h, payload, HASH_SIZE);
		std::vector<uint8_t> out;
		if (m_handler->get_share(id, out)) send(p->id, MsgType::SHARE, out.data(), out.size());
		else send(p->id, MsgType::NOT_FOUND, id.h, HASH_SIZE);
		return true;
	}

	case MsgType::GET_TIP: {
		if (size) return false;
		std::vector<uint8_t> out;
		if (m_handler->get_tip(out)) send(p->id, MsgType::SHARE, out.data(), out.size());
		return true;
	}

	case MsgType::NOT_FOUND: {
		if (size != HASH_SIZE) return false;
		hash id;
		memcpy(id.h, payload, HASH_SIZE);
		m_handler->on_not_found(id, p->id);
		return true;
	}
	}
	return false; // unknown message type
}

} // namespace pqch
} // namespace p2pool
