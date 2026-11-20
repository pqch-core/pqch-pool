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

// PQCH: the network between pool nodes (phase B). Plain TCP on the libuv loop.
//
// Every message: "PQCP" | type (1 byte) | payload length (4 bytes LE) | payload.
//   HELLO      version (4) | network (1) | listen port (2)        first message both ways; other network -> bye
//   SHARE      a share message (pqch_share.h)                     new shares and answers to GET_SHARE / GET_TIP
//   GET_SHARE  share id (32)                                      ask a peer for a share (a missing parent)
//   GET_TIP    (empty)                                            ask a peer for its tip (after HELLO)
//   NOT_FOUND  share id (32)                                      the peer does not have that share (pruned)
//
// A peer that sends garbage, an invalid share, or more than MAX_MESSAGES_PER_SECOND is disconnected.
// Outgoing peers (--peer) are reconnected every RECONNECT_SECONDS.

#include "pqch_block.h"
#include "uv_util.h"
#include <functional>

namespace p2pool {
namespace pqch {

enum class MsgType : uint8_t { HELLO = 1, SHARE = 2, GET_SHARE = 3, GET_TIP = 4, NOT_FOUND = 5 };

class P2PHandler
{
public:
	virtual ~P2PHandler() = default;
	// A share message from peer `peer` (loop thread). Return false to disconnect the peer (invalid share).
	virtual bool on_share(const uint8_t* data, size_t size, uint64_t peer) = 0;
	// A share this node has, serialized; false if unknown.
	virtual bool get_share(const hash& id, std::vector<uint8_t>& out) = 0;
	virtual bool get_tip(std::vector<uint8_t>& out) = 0;
	// The peer does not have share `id`.
	virtual void on_not_found(const hash& id, uint64_t peer) = 0;
};

class P2P
{
public:
	static constexpr uint32_t VERSION = 1;
	static constexpr size_t MAX_PAYLOAD = 70 * 1024;
	static constexpr uint32_t MAX_MESSAGES_PER_SECOND = 200;
	static constexpr uint64_t RECONNECT_SECONDS = 10;
	static constexpr size_t MAX_PEERS = 64;

	P2P(uv_loop_t* loop, Network net, uint16_t listen_port, const std::vector<std::string>& peers, P2PHandler* handler);
	~P2P();

	bool start();
	void stop();

	void broadcast_share(const std::vector<uint8_t>& share, uint64_t except_peer = 0);
	void send(uint64_t peer, MsgType type, const uint8_t* payload, size_t size);
	void disconnect(uint64_t peer, const char* reason);
	size_t peer_count() const;

	struct Peer;

private:
	static void on_connection(uv_stream_t* server, int status);
	static void on_timer(uv_timer_t* timer);
	void connect_outgoing();
	Peer* add_peer(bool outgoing, const std::string& address);
	void start_peer(Peer* p);
	void on_data(Peer* p, const char* data, size_t size);
	bool handle(Peer* p, MsgType type, const uint8_t* payload, size_t size);
	void send_hello(Peer* p);
	void close_peer(Peer* p);

	uv_loop_t* m_loop;
	Network m_net;
	uint16_t m_listenPort;
	std::vector<std::string> m_outgoing;
	P2PHandler* m_handler;

	uv_tcp_t m_server{};
	bool m_serverStarted = false;
	uv_timer_t m_timer{};
	bool m_timerStarted = false;
	bool m_stopped = false;

	uint64_t m_nextPeerId = 1;
	std::vector<Peer*> m_peers;
};

} // namespace pqch
} // namespace p2pool
