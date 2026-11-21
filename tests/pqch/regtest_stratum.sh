#!/usr/bin/env bash
# PQCH phase C acceptance test: a miner without a node mines on a pool node over stratum.
#
#   tests/pqch/regtest_stratum.sh <dir with pqchd and pqch-cli> <pqch-pool binary> [seconds]
#
# Starts a fresh regtest pqchd and one pool node with one built-in miner (address A) and a stratum port; then
# runs the stand-alone miner (--pqch-miner, address B, 2 threads) against that port. Checks:
#   - the miner logged in, found shares and the pool accepted them all (none rejected);
#   - the share chain has shares of both A and B;
#   - every pool block was accepted and pays exactly the PPLNS split (deploy/verificar_pool_testnet.py,
#     independent of the pool's code), and B, the stratum miner, was paid;
#   - a login with an address of another network is refused.
set -euo pipefail

BIN=${1:?pqchd directory}
POOL=${2:?pqch-pool binary}
SECONDS_TO_RUN=${3:-90}
RPC_PORT=${RPC_PORT:-49556}
STRATUM_PORT=${STRATUM_PORT:-49557}
HERE=$(cd "$(dirname "$0")" && pwd)
DIR=$(mktemp -d)
trap '"$BIN/pqch-cli" -regtest -datadir="$DIR" -rpcport="$RPC_PORT" stop >/dev/null 2>&1 || true; kill $(jobs -p) 2>/dev/null || true; sleep 2; rm -rf "$DIR"' EXIT

"$BIN/pqchd" -regtest -datadir="$DIR" -daemon -listen=0 -rpcport="$RPC_PORT" -fallbackfee=0.0001 >/dev/null
for _ in $(seq 1 120); do [ -s "$DIR/regtest/.cookie" ] && break; sleep 0.5; done
cli() { "$BIN/pqch-cli" -regtest -datadir="$DIR" -rpcport="$RPC_PORT" -rpcwait "$@"; }
cli createwallet w >/dev/null
cli generatetoaddress 110 "$(cli getnewaddress)" >/dev/null
A=$(cli getnewaddress); B=$(cli getnewaddress)
START=$(cli getblockcount)

"$POOL" --pqch-pool "127.0.0.1:$RPC_PORT" "$DIR/regtest/.cookie" "$A" --share-time 5 --min-diff 20 \
	--seconds "$((SECONDS_TO_RUN + 10))" --log "$DIR/pool.jsonl" --stratum "127.0.0.1:$STRATUM_PORT" > "$DIR/pool.out" 2>&1 &
POOL_PID=$!
sleep 3

# a login with a testnet address must be refused
if "$POOL" --pqch-miner "127.0.0.1:$STRATUM_PORT" pqct1pjmsknlc73kmcqw87nadat9xhvpwgu48x3pagweqqa8ffl3ssc7gq06rc6z --seconds 5 > "$DIR/bad.out" 2>&1; then
	echo "FAIL: a login with an address of another network was accepted"; cat "$DIR/bad.out"; exit 1
fi
grep -q "refused the login" "$DIR/bad.out" || { echo "FAIL: no login refusal"; cat "$DIR/bad.out"; exit 1; }

"$POOL" --pqch-miner "127.0.0.1:$STRATUM_PORT" "$B" --threads 2 --seconds "$SECONDS_TO_RUN" > "$DIR/miner.out" 2>&1 || {
	echo "FAIL: the miner exited with an error"; tail -20 "$DIR/miner.out"; exit 1; }
wait "$POOL_PID" || { echo "FAIL: the pool exited with an error"; tail -20 "$DIR/pool.out"; exit 1; }

grep "pqch-miner:" "$DIR/miner.out"
grep "pqch-pool:" "$DIR/pool.out"
ACCEPTED=$(sed -n 's/.* \([0-9]*\) accepted.*/\1/p' "$DIR/miner.out" | tail -1)
[ "${ACCEPTED:-0}" -gt 0 ] || { echo "FAIL: no share of the stratum miner was accepted"; exit 1; }

python3 - "$DIR/pool.jsonl" "$A" "$B" <<'PY'
import json, sys
shares = [json.loads(l) for l in open(sys.argv[1]) if '"type":"share"' in l]
by = {m: sum(1 for s in shares if s["miner"] == m) for m in sys.argv[2:4]}
assert all(by.values()), f"shares per miner: {by}"
print(f"shares per miner: built-in {by[sys.argv[2]]}, stratum {by[sys.argv[3]]}")
PY

python3 "$HERE/../../deploy/verificar_pool_testnet.py" --user - --datadir "$DIR" --log "$DIR/pool.jsonl" --rede regtest \
	--cli "$BIN/pqch-cli" --rpcport "$RPC_PORT" --blocos "$(( $(cli getblockcount) - START ))" | tee "$DIR/check.out" | tail -3
grep -q " 0 errados" "$DIR/check.out" || { echo "FAIL: PPLNS check"; exit 1; }
grep -q "blocos do pool" "$DIR/check.out" && ! grep -q "^0 blocos" "$DIR/check.out" || { echo "FAIL: no pool block"; exit 1; }
for h in $(seq $((START + 1)) "$(cli getblockcount)"); do
	cli getblock "$(cli getblockhash "$h")" 2 | grep -q "\"$B\"" && { echo "stratum miner paid in block $h"; exit 0; }
done
echo "FAIL: the stratum miner was never paid"; exit 1
