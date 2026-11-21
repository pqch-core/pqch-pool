#!/usr/bin/env bash
# PQCH phase B test (regtest): two pool nodes, each with its own pqchd and its own miner, connected by the pool
# network, form one pool.
#
#   tests/pqch/regtest_two_pools.sh <dir with pqchd and pqch-cli> <pqch-pool binary> [seconds]
#
# Two regtest pqchd nodes connected to each other; pool node A (miner MA) listens for pool nodes, pool node B
# (miner MB) connects to it. Afterwards, checked independently of the pool's code:
#   - both pool nodes ended on the same share chain tip and the shares form one tree;
#   - each node received shares from the other;
#   - every pool block in the main chain (as both pqchd nodes see it) pays exactly the PPLNS split of the
#     share chain (weights = share difficulties, floor, remainder to the finder);
#   - blocks found by A pay MB and blocks found by B pay MA: one pool, not two.
# On regtest every share is also a block, so the two nodes sometimes find a block at the same height: the one
# that loses ("inconclusive") is an orphan, not an error.
set -euo pipefail

BIN=${1:?pqchd directory}
POOL=${2:?pqch-pool binary}
RUN=${3:-120}
DA=$(mktemp -d); DB=$(mktemp -d)
RA=49561; RB=49562; PA=49571; PB=49572; POOL_PORT=37891
stop_all() {
	"$BIN/pqch-cli" -regtest -datadir="$DA" -rpcport=$RA stop >/dev/null 2>&1 || true
	"$BIN/pqch-cli" -regtest -datadir="$DB" -rpcport=$RB stop >/dev/null 2>&1 || true
	sleep 2; rm -rf "$DA" "$DB"
}
trap stop_all EXIT

"$BIN/pqchd" -regtest -datadir="$DA" -daemon -port=$PA -rpcport=$RA -fallbackfee=0.0001 >/dev/null
"$BIN/pqchd" -regtest -datadir="$DB" -daemon -port=$PB -rpcport=$RB -connect=127.0.0.1:$PA -fallbackfee=0.0001 >/dev/null
for d in "$DA" "$DB"; do for _ in $(seq 1 120); do [ -s "$d/regtest/.cookie" ] && break; sleep 0.5; done; done
a() { "$BIN/pqch-cli" -regtest -datadir="$DA" -rpcport=$RA -rpcwait "$@"; }
b() { "$BIN/pqch-cli" -regtest -datadir="$DB" -rpcport=$RB -rpcwait "$@"; }
a createwallet w >/dev/null; b createwallet w >/dev/null
a generatetoaddress 110 "$(a getnewaddress)" >/dev/null
for _ in $(seq 1 60); do [ "$(b getblockcount)" = 110 ] && break; sleep 1; done
[ "$(b getblockcount)" = 110 ] || { echo "the two pqchd nodes are not in sync"; exit 1; }
MA=$(a getnewaddress); MB=$(b getnewaddress)

"$POOL" --pqch-pool 127.0.0.1:$RA "$DA/regtest/.cookie" "$MA" --share-time 5 --min-diff 20 --seconds "$RUN" \
	--p2p-port $POOL_PORT --log "$DA/pool.jsonl" > "$DA/pool.out" 2>&1 &
PID_A=$!
sleep 2
"$POOL" --pqch-pool 127.0.0.1:$RB "$DB/regtest/.cookie" "$MB" --share-time 5 --min-diff 20 --seconds "$RUN" \
	--peer 127.0.0.1:$POOL_PORT --log "$DB/pool.jsonl" > "$DB/pool.out" 2>&1 &
PID_B=$!
# pool node C: no miners, joins halfway through and has to fetch the shares it missed from A
sleep $((RUN / 2))
"$POOL" --pqch-pool 127.0.0.1:$RA "$DA/regtest/.cookie" - --network regtest --share-time 5 --min-diff 20 \
	--seconds $((RUN / 2 - 2)) --peer 127.0.0.1:$POOL_PORT --log "$DA/poolc.jsonl" > "$DA/poolc.out" 2>&1 &
PID_C=$!
wait $PID_A || true; wait $PID_B || true; wait $PID_C || true
grep "pqch-pool:" "$DA/pool.out" | sed 's/^/A: /'; grep "pqch-pool:" "$DB/pool.out" | sed 's/^/B: /'
grep "pqch-pool:" "$DA/poolc.out" | sed 's/^/C: /'

sleep 3 # let the main chain settle
[ "$(a getbestblockhash)" = "$(b getbestblockhash)" ] || { echo "the two pqchd nodes disagree on the main chain"; exit 1; }

# coinbases of every block either pool found, as pqchd stores them (confirmations -1 = not in the main chain)
for d in "$DA" "$DB"; do
	python3 -c "
import json,sys
for l in open('$d/pool.jsonl'):
    r=json.loads(l)
    if r['type']=='block': print(r['hash'])"
done | sort -u | while read -r h; do
	# a block that lost a race on the other node ("inconclusive") may never reach node A: an orphan too
	{ a getblock "$h" 2 2>/dev/null || echo '{"hash":"'"$h"'","confirmations":-1,"tx":[{"vout":[]}]}'; } |
		python3 -c "import sys,json; b=json.load(sys.stdin); print(json.dumps({'hash': b['hash'], 'confirmations': b['confirmations'], 'vout': b['tx'][0]['vout']}))"
done > "$DA/coinbases.jsonl"

python3 - "$DA/pool.jsonl" "$DB/pool.jsonl" "$DA/coinbases.jsonl" "$MA" "$MB" "$DA/pool.out" "$DB/pool.out" "$DA/poolc.out" "$DA/poolc.jsonl" <<'EOF'
import json, re, sys

PPLNS_WINDOW = 2160
logs = [[json.loads(l) for l in open(p)] for p in sys.argv[1:3]]
coinbases = {c["hash"]: c for c in map(json.loads, open(sys.argv[3]))}
ma, mb = sys.argv[4:6]
tips = [re.search(r"tip ([0-9a-f]{64})", open(p).read()).group(1) for p in sys.argv[6:8]]
late = re.search(r"tip ([0-9a-f]{64})", open(sys.argv[8]).read())
late_shares = [json.loads(l) for l in open(sys.argv[9])]

# all shares either node knows (each node logs its own and the ones it received)
shares = {}
for log in logs:
    for r in log:
        if r["type"] == "share":
            shares[r["id"]] = r
received = [sum(1 for r in log if r["type"] == "share" and not r["own"]) for log in logs]
assert received[0] > 0 and received[1] > 0, f"a node received no shares from the other: {received}"

def branch(share_id, n):
    out = []
    while share_id in shares and len(out) < n:
        s = shares[share_id]
        out.append(s)
        if s["height"] == 0:
            break
        share_id = s["parent"]
    return out

# one tree: every share's parent is known (or it is a first share)
for s in shares.values():
    assert s["height"] == 0 or s["parent"] in shares, f"share {s['id']} has an unknown parent"
# Same tip, or (both nodes stop at the same moment) one node's last shares had not reached the other yet:
# then one tip is an ancestor of the other, never on another branch.
ids = [{x["id"] for x in branch(t, 10**9)} for t in tips]
assert tips[0] == tips[1] or tips[0] in ids[1] or tips[1] in ids[0], f"the two pool nodes ended on different branches: {tips}"
best = branch(tips[0] if tips[1] in ids[0] else tips[1], 10**9)
assert best[-1]["height"] == 0, "the tip does not go back to the first share"

checked = 0
orphans = 0
paid_by = {"A": {}, "B": {}}
for node, log in zip("AB", logs):
    for r in log:
        if r["type"] != "block":
            continue
        cb = coinbases[r["hash"]]
        if cb["confirmations"] < 1:
            orphans += 1
            assert r["reason"] in ("inconclusive", "duplicate", "") or not r["accepted"], r
            continue
        assert r["accepted"], f"block {r['hash']} in the chain but reported rejected: {r['reason']}"
        window = branch(r["share"], PPLNS_WINDOW)
        weights = {}
        for w in window:
            weights[w["miner"]] = weights.get(w["miner"], 0) + w["difficulty"]
        total = r["coinbase_value"]
        wsum = sum(weights.values())
        expected = {m: total * w // wsum for m, w in weights.items()}
        expected[shares[r["share"]]["miner"]] += total - sum(expected.values())
        expected = {m: v for m, v in expected.items() if v > 0}
        onchain = {}
        for o in cb["vout"]:
            v = round(o["value"] * 100_000_000)
            if v:
                onchain[o["scriptPubKey"]["address"]] = onchain.get(o["scriptPubKey"]["address"], 0) + v
        assert onchain == expected, f"block {r['height']} by {node}: coinbase {onchain} != PPLNS {expected}"
        for m, v in onchain.items():
            paid_by[node][m] = paid_by[node].get(m, 0) + v
        checked += 1

assert checked >= 2, f"too few blocks in the main chain: {checked}"

# the late node fetched the earlier shares (down to the first one) and followed the tip until it stopped
assert late, "the late pool node has no tip"
assert any(s["height"] == 0 for s in late_shares), "the late node did not fetch the chain back to its start"
late_tip_height = shares[late.group(1)]["height"] if late.group(1) in shares else -1
assert late_tip_height >= best[0]["height"] - 2, f"the late node is behind: {late_tip_height} vs {best[0]['height']}"
assert paid_by["A"].get(mb, 0) > 0 or paid_by["B"].get(ma, 0) > 0, f"no node paid the other node's miner: {paid_by}"
print(f"regtest_two_pools: OK - {len(shares)} shares in one tree (tip height {best[0]['height']}), "
      f"received A {received[0]} / B {received[1]}, {checked} blocks in the main chain each = PPLNS of the shared "
      f"chain, {orphans} orphans; late node C synced {len(late_shares)} shares, tip height {late_tip_height}; A's blocks paid {paid_by['A']}, B's blocks paid {paid_by['B']}")
EOF
