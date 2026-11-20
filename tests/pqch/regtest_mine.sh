#!/usr/bin/env bash
# PQCH: end-to-end test of the pool's main-chain path against a real pqchd (regtest).
#
#   tests/pqch/regtest_mine.sh <dir with pqchd and pqch-cli> <pqch-pool binary>
#
# Starts a fresh regtest pqchd, puts transactions in the mempool, has the pool build blocks paying several
# P2MR addresses (pqch-pool --pqchd-mine-test) and checks with pqch-cli that pqchd accepted them and that
# each coinbase pays exactly the expected split (subsidy + fees, PPLNS weights, remainder to the finder).
set -euo pipefail

BIN=${1:?pqchd directory}
POOL=${2:?pqch-pool binary}
RPC_PORT=${RPC_PORT:-49553} # not the default 49443, to stay clear of another regtest node
DIR=$(mktemp -d)
trap '"$BIN/pqch-cli" -regtest -datadir="$DIR" -rpcport="$RPC_PORT" stop >/dev/null 2>&1 || true; sleep 2; rm -rf "$DIR"' EXIT

"$BIN/pqchd" -regtest -datadir="$DIR" -daemon -listen=0 -rpcport="$RPC_PORT" -fallbackfee=0.0001 >/dev/null
for _ in $(seq 1 120); do [ -s "$DIR/regtest/.cookie" ] && break; sleep 0.5; done # -rpcwait does not wait for the cookie file
cli() { "$BIN/pqch-cli" -regtest -datadir="$DIR" -rpcport="$RPC_PORT" -rpcwait "$@"; }
cli createwallet w >/dev/null
cli generatetoaddress 110 "$(cli getnewaddress)" >/dev/null

A1=$(cli getnewaddress); A2=$(cli getnewaddress); A3=$(cli getnewaddress)
for round in 1 2 3; do
	cli sendtoaddress "$(cli getnewaddress)" "1.$round" >/dev/null
	before=$(cli getblockcount)
	value=$(cli getblocktemplate '{"rules":["segwit"]}' | python3 -c 'import sys,json;print(json.load(sys.stdin)["coinbasevalue"])')
	"$POOL" --pqchd-mine-test "127.0.0.1:$RPC_PORT" "$DIR/regtest/.cookie" "$A1:3,$A2:2,$A3:1" | grep -q "submitblock: accepted"
	[ "$(cli getblockcount)" -eq $((before + 1)) ]
	cli getblock "$(cli getbestblockhash)" 2 | python3 -c "
import sys, json
b = json.load(sys.stdin)
outs = {o['scriptPubKey'].get('address'): round(o['value'] * 100000000) for o in b['tx'][0]['vout'] if o['value'] > 0}
total = $value
exp = {'$A1': total * 3 // 6, '$A2': total * 2 // 6, '$A3': total // 6}
exp['$A1'] += total - sum(exp.values())   # rounding remainder to the finder
assert outs == exp, (outs, exp)
assert len(b['tx']) >= 2, 'mempool transaction not included'
print('round $round: block', b['height'], 'pays', outs)
"
done
echo "regtest_mine: OK"
