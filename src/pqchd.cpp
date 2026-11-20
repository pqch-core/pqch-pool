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
#include "pqchd.h"
#include "uv_util.h"
#include "json_rpc_request.h"
#include "rapidjson_wrapper.h"
#include <fstream>

namespace p2pool {
namespace pqchd {

namespace {

constexpr char HEX[] = "0123456789abcdef";

// 64 hex digits as shown by the RPC (big-endian) -> internal byte order (reversed)
bool parse_rpc_hash(const rapidjson::Value& v, hash& out)
{
	if (!v.IsString() || v.GetStringLength() != HASH_SIZE * 2) {
		return false;
	}
	hash h;
	if (!from_hex(v.GetString(), v.GetStringLength(), h)) {
		return false;
	}
	for (size_t i = 0; i < HASH_SIZE; ++i) {
		out.h[i] = h.h[HASH_SIZE - 1 - i];
	}
	return true;
}

bool parse_hex_bytes(const rapidjson::Value& v, std::vector<uint8_t>& out)
{
	if (!v.IsString() || (v.GetStringLength() % 2)) {
		return false;
	}
	return from_hex(v.GetString(), v.GetStringLength(), out);
}

bool get_uint64(const rapidjson::Value& obj, const char* name, uint64_t& out)
{
	auto it = obj.FindMember(name);
	if (it == obj.MemberEnd() || !it->value.IsUint64()) {
		return false;
	}
	out = it->value.GetUint64();
	return true;
}

const rapidjson::Value* get_member(const rapidjson::Value& obj, const char* name)
{
	auto it = obj.FindMember(name);
	return (it == obj.MemberEnd()) ? nullptr : &it->value;
}

// The RPC error object, if any: "message (code)"
bool rpc_error(const rapidjson::Document& doc, std::string& error)
{
	auto it = doc.FindMember("error");
	if (it == doc.MemberEnd() || it->value.IsNull()) {
		return false;
	}
	error = "RPC error";
	if (it->value.IsObject()) {
		auto msg = it->value.FindMember("message");
		auto code = it->value.FindMember("code");
		if (msg != it->value.MemberEnd() && msg->value.IsString()) {
			error = msg->value.GetString();
		}
		if (code != it->value.MemberEnd() && code->value.IsInt()) {
			error += " (" + std::to_string(code->value.GetInt()) + ')';
		}
	}
	return true;
}

} // namespace

std::string getblocktemplate_request(const std::string& longpollid)
{
	std::string req = R"({"jsonrpc":"1.0","id":"pqch-pool","method":"getblocktemplate","params":[{"rules":["segwit"])";
	if (!longpollid.empty()) {
		req += R"(,"longpollid":")";
		for (char c : longpollid) {
			// longpollid is "<hex block hash><decimal>" from pqchd; refuse anything else
			if (!isalnum(static_cast<unsigned char>(c))) {
				return std::string();
			}
			req += c;
		}
		req += '"';
	}
	req += "}]}";
	return req;
}

std::string submitblock_request(const uint8_t* block, size_t size)
{
	std::string req = R"({"jsonrpc":"1.0","id":"pqch-pool","method":"submitblock","params":[")";
	req.reserve(req.size() + size * 2 + 4);
	for (size_t i = 0; i < size; ++i) {
		req += HEX[block[i] >> 4];
		req += HEX[block[i] & 15];
	}
	req += R"("]})";
	return req;
}

bool parse_getblocktemplate(const char* data, size_t size, Template& out, std::string& error)
{
	rapidjson::Document doc;
	if (doc.Parse(data, size).HasParseError() || !doc.IsObject()) {
		error = "getblocktemplate: invalid JSON";
		return false;
	}
	if (rpc_error(doc, error)) {
		error = "getblocktemplate: " + error;
		return false;
	}
	const rapidjson::Value* r = get_member(doc, "result");
	if (!r || !r->IsObject()) {
		error = "getblocktemplate: no result";
		return false;
	}

	Template t;

	const rapidjson::Value* v = get_member(*r, "version");
	if (!v || !v->IsInt()) { error = "getblocktemplate: version"; return false; }
	t.version = v->GetInt();

	v = get_member(*r, "previousblockhash");
	if (!v || !parse_rpc_hash(*v, t.prev_hash)) { error = "getblocktemplate: previousblockhash"; return false; }

	if (!get_uint64(*r, "height", t.height)) { error = "getblocktemplate: height"; return false; }

	v = get_member(*r, "bits");
	if (!v || !v->IsString() || v->GetStringLength() != 8) { error = "getblocktemplate: bits"; return false; }
	{
		std::vector<uint8_t> b;
		if (!from_hex(v->GetString(), 8, b) || b.size() != 4) { error = "getblocktemplate: bits"; return false; }
		t.bits = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 8) | b[3];
	}

	v = get_member(*r, "target");
	if (!v || !parse_rpc_hash(*v, t.target)) { error = "getblocktemplate: target"; return false; }

	if (!get_uint64(*r, "curtime", t.curtime) || !get_uint64(*r, "mintime", t.mintime)) { error = "getblocktemplate: curtime/mintime"; return false; }
	if (!get_uint64(*r, "coinbasevalue", t.coinbase_value)) { error = "getblocktemplate: coinbasevalue"; return false; }

	// PQCH-specific: the RandomX key. A template without it does not come from pqchd.
	v = get_member(*r, "randomx_seed_hash");
	if (!v || !parse_rpc_hash(*v, t.seed_hash)) { error = "getblocktemplate: randomx_seed_hash (not a PQCH node?)"; return false; }
	if (!get_uint64(*r, "randomx_seed_height", t.seed_height) || (t.seed_height >= t.height)) { error = "getblocktemplate: randomx_seed_height"; return false; }

	// SegWit is active from genesis on every PQCH network, so the commitment is always there.
	v = get_member(*r, "default_witness_commitment");
	if (!v || !parse_hex_bytes(*v, t.witness_commitment) || t.witness_commitment.empty()) { error = "getblocktemplate: default_witness_commitment"; return false; }

	if (!get_uint64(*r, "weightlimit", t.weight_limit) || !get_uint64(*r, "sigoplimit", t.sigop_limit)) { error = "getblocktemplate: weightlimit/sigoplimit"; return false; }

	v = get_member(*r, "longpollid");
	if (v && v->IsString()) {
		t.longpollid = v->GetString();
	}

	v = get_member(*r, "transactions");
	if (!v || !v->IsArray()) { error = "getblocktemplate: transactions"; return false; }
	t.transactions.reserve(v->Size());
	for (const auto& tx : v->GetArray()) {
		if (!tx.IsObject()) { error = "getblocktemplate: transaction"; return false; }
		TemplateTx x;
		const rapidjson::Value* f = get_member(tx, "data");
		if (!f || !parse_hex_bytes(*f, x.data) || x.data.empty()) { error = "getblocktemplate: transaction data"; return false; }
		f = get_member(tx, "txid");
		if (!f || !parse_rpc_hash(*f, x.txid)) { error = "getblocktemplate: transaction txid"; return false; }
		f = get_member(tx, "hash");
		if (!f || !parse_rpc_hash(*f, x.wtxid)) { error = "getblocktemplate: transaction hash"; return false; }
		if (!get_uint64(tx, "fee", x.fee) || !get_uint64(tx, "weight", x.weight) || !get_uint64(tx, "sigops", x.sigops)) {
			error = "getblocktemplate: transaction fee/weight/sigops";
			return false;
		}
		f = get_member(tx, "depends");
		if (f && f->IsArray()) {
			for (const auto& d : f->GetArray()) {
				if (!d.IsUint() || (d.GetUint() == 0) || (d.GetUint() > t.transactions.size())) {
					error = "getblocktemplate: transaction depends";
					return false;
				}
				x.depends.push_back(d.GetUint());
			}
		}
		t.transactions.emplace_back(std::move(x));
	}

	out = std::move(t);
	return true;
}

bool parse_submitblock(const char* data, size_t size, std::string& reason)
{
	rapidjson::Document doc;
	if (doc.Parse(data, size).HasParseError() || !doc.IsObject()) {
		reason = "invalid JSON";
		return false;
	}
	if (rpc_error(doc, reason)) {
		return false;
	}
	auto it = doc.FindMember("result");
	if (it == doc.MemberEnd()) {
		reason = "no result";
		return false;
	}
	if (it->value.IsNull()) {
		reason.clear();
		return true;
	}
	reason = it->value.IsString() ? it->value.GetString() : "rejected";
	return false;
}

bool read_cookie(const std::string& path, std::string& auth)
{
	std::ifstream f(path, std::ios::binary);
	if (!f.good()) {
		return false;
	}
	std::string line;
	std::getline(f, line);
	while (!line.empty() && ((line.back() == '\r') || (line.back() == '\n') || (line.back() == ' '))) {
		line.pop_back();
	}
	const size_t colon = line.find(':');
	if ((colon == std::string::npos) || (colon == 0) || (colon + 1 >= line.size())) {
		return false;
	}
	auth = JSONRPCRequest::BITCOIN_AUTH_PREFIX + line;
	return true;
}

std::string make_auth(const std::string& user, const std::string& password)
{
	return JSONRPCRequest::BITCOIN_AUTH_PREFIX + user + ':' + password;
}

} // namespace pqchd
} // namespace p2pool
