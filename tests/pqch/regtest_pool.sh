#!/usr/bin/env bash
# PQCH phase A acceptance test: one pool node on regtest finds PQCH blocks and every coinbase pays the
# miners exactly according to the share chain (PPLNS).
#
#   tests/pqch/regtest_pool.sh <dir with pqchd and pqch-cli> <pqch-pool binary> [seconds]
#
# Starts a fresh regtest pqchd, runs the pool with two built-in miners (two P2MR addresses) and then checks,
# independently of the pool's code:
#   - the shares form one chain (each one's parent is the previous one, heights 0, 1, 2, ...);
#   - every block the pool found was accepted and is in pqchd's active chain;
#   - every block's coinbase, as stored by pqchd, pays exactly the PPLNS split of the shares up to and
#     including the block's share (weights = share difficulties, floor, remainder to the finder) and the
#     whole coinbase value (subsidy + fees);
#   - both miners were paid.
set -euo pipefail

BIN=${1:?pqchd directory}
POOL=${2:?pqch-pool binary}
SECONDS_TO_RUN=${3:-90}
RPC_PORT=${RPC_PORT:-49554}
DIR=$(mktemp -d)
trap '"$BIN/pqch-cli" -regtest -datadir="$DIR" -rpcport="$RPC_PORT" stop >/dev/null 2>&1 || true; sleep 2; rm -rf "$DIR"' EXIT

"$BIN/pqchd" -regtest -datadir="$DIR" -daemon -listen=0 -rpcport="$RPC_PORT" -fallbackfee=0.0001 >/dev/null
cli() { "$BIN/pqch-cli" -regtest -datadir="$DIR" -rpcport="$RPC_PORT" -rpcwait "$@"; }
cli createwallet w >/dev/null
cli generatetoaddress 110 "$(cli getnewaddress)" >/dev/null
M1=$(cli getnewaddress); M2=$(cli getnewaddress)
for i in 1 2 3; do cli sendtoaddress "$(cli getnewaddress)" "0.$i" >/dev/null; done # fees for the first block

"$POOL" --pqch-pool "127.0.0.1:$RPC_PORT" "$DIR/regtest/.cookie" "$M1,$M2" \
	--share-time 5 --min-diff 20 --seconds "$SECONDS_TO_RUN" --log "$DIR/pool.jsonl" | grep "pqch-pool:"

# coinbase of every block, as pqchd stored it
python3 - "$DIR/pool.jsonl" "$M1" "$M2" <<'EOF' > "$DIR/blocks.txt"
import json, sys
for line in open(sys.argv[1]):
    d = json.loads(line)
    if d["type"] == "block":
        print(d["hash"])
EOF
: > "$DIR/coinbases.jsonl"
while read -r h; do
	cli getblock "$h" 2 | python3 -c "import sys,json; b=json.load(sys.stdin); print(json.dumps({'hash': b['hash'], 'confirmations': b['confirmations'], 'vout': b['tx'][0]['vout']}))" >> "$DIR/coinbases.jsonl"
done < "$DIR/blocks.txt"

python3 - "$DIR/pool.jsonl" "$DIR/coinbases.jsonl" "$M1" "$M2" <<'EOF'
import json, sys

PPLNS_WINDOW = 2160
records = [json.loads(l) for l in open(sys.argv[1])]
coinbases = {c["hash"]: c for c in map(json.loads, open(sys.argv[2]))}
miners = sys.argv[3:5]

shares = [r for r in records if r["type"] == "share"]
blocks = [r for r in records if r["type"] == "block"]
assert len(shares) >= 4, f"too few shares: {len(shares)}"
assert blocks, "the pool found no block"

# one chain
by_id = {}
for i, s in enumerate(shares):
    assert s["height"] == i, (i, s["height"])
    assert s["parent"] == ("00" * 32 if i == 0 else shares[i - 1]["id"]), f"share {i}: wrong parent"
    by_id[s["id"]] = s

paid = {m: 0 for m in miners}
for b in blocks:
    assert b["accepted"], f"block {b['hash']} rejected: {b['reason']}"
    cb = coinbases[b["hash"]]
    assert cb["confirmations"] >= 1, f"block {b['hash']} is not in the active chain"

    # expected PPLNS split: the window that ends with the block's share
    s = by_id[b["share"]]
    window = shares[max(0, s["height"] - PPLNS_WINDOW + 1): s["height"] + 1]
    weights = {}
    for w in window:
        weights[w["miner"]] = weights.get(w["miner"], 0) + w["difficulty"]
    total = b["coinbase_value"]
    wsum = sum(weights.values())
    expected = {m: total * w // wsum for m, w in weights.items()}
    expected[s["miner"]] += total - sum(expected.values())   # remainder to the finder
    expected = {m: v for m, v in expected.items() if v > 0}

    onchain = {}
    for o in cb["vout"]:
        v = round(o["value"] * 100_000_000)
        if v:
            onchain[o["scriptPubKey"]["address"]] = onchain.get(o["scriptPubKey"]["address"], 0) + v
    assert onchain == expected, f"block {b['height']}: coinbase {onchain} != PPLNS {expected}"
    assert sum(onchain.values()) == total, f"block {b['height']}: pays {sum(onchain.values())} of {total}"
    for m, v in onchain.items():
        paid[m] += v

assert all(paid[m] > 0 for m in miners), f"a miner was never paid: {paid}"
print(f"regtest_pool: OK - {len(shares)} shares in one chain, {len(blocks)} blocks accepted, "
      f"every coinbase = PPLNS of the share chain; paid {paid}")
EOF
