# pqch-pool

Decentralized mining pool for **PQCH (PostQuantum Cash)**: a fork of [P2Pool](https://github.com/SChernykh/p2pool)
v4.18.1 (commit `abc8335`, GPL-3.0, by SChernykh and contributors) with PQCH as the main chain.

There is no operator and no account: the pool nodes keep a chain of shares among themselves, and every
PQCH block found by the pool pays all miners of the PPLNS window directly in its coinbase, one P2MR
output each.

Status: **phase A done** (20 Nov 2026): one pool node on regtest finds PQCH blocks whose coinbases pay the share chain's PPLNS split. See `contrib/pqch/POOL.md` in the PQCH Core repository for the plan.

| Phase | What | Status |
| --- | --- | --- |
| A | Fork, main chain: block template, coinbase, proof of work and block submission with `pqchd`; one pool node in regtest | **done** |
| B | Pool network: share chain between nodes, PPLNS, PQCH parameters | regtest done; testnet next |
| C | Miner with stratum | planned |
| D | One-click application (Windows, Linux) | planned |
| E | Tests with outside miners, audit scope | planned |

## What changes from P2Pool

| P2Pool (Monero) | pqch-pool (PQCH) |
| --- | --- |
| `monerod` RPC and ZMQ | `pqchd`: `getblocktemplate`, `submitblock`, `-zmqpubhashblock`, cookie authentication |
| Monero block (varint header, `tx_extra`) | 80-byte header as in Bitcoin; share id and extranonce in the coinbase `scriptSig` |
| One-time keys per payout | one P2MR output per miner (the miner is identified by its P2MR address) |
| RandomX over the Monero hashing blob | RandomX v2 over the 80-byte header, key from `randomx_seed_hash` (4096 / 64 schedule) |
| Merged mining (Tari, others) | not in the first version |

## Building (phase A, development)

```
git submodule update --init --depth 1 external/src/RandomX external/src/cppzmq external/src/libuv \
    external/src/libzmq external/src/rapidjson external/src/robin-hood-hashing external/src/curl tests/googletest
git submodule update --init --depth 1 external/src/grpc
git -C external/src/grpc submodule update --init --depth 1 third_party/boringssl-with-bazel   # TLS only
cmake -B build -DWITH_GRPC=OFF -DWITH_MERGE_MINING_DONATION=OFF -DWITH_UPNP=OFF
cmake --build build -j
```

## Phase A: what exists

| Piece | Where |
| --- | --- |
| Connection to `pqchd` (`getblocktemplate` every second, `submitblock`, cookie or user/password) | `src/pqchd.*`, `src/pqchd_client.*` |
| P2MR addresses, PPLNS split, coinbase, merkle root, 80-byte header, block | `src/pqch_block.*` |
| RandomX v2 proof of work (checked against the official vectors, as `pqchd`) | `src/pqch_pow.*` |
| Share chain: share ids committed in the coinbase, PPLNS window of 2,160 shares, share difficulty for one share per 10 s | `src/pqch_sidechain.*` |
| Pool node with built-in miners | `src/pqch_pool.cpp` |
| Share tree with fork choice (most cumulative work) | `src/pqch_sidechain.*` |
| Shares between nodes: message with the coinbase and its merkle branch; check of PoW, id, coinbase = PPLNS split | `src/pqch_share.*` |
| Pool network (TCP: hello, share, get share, get tip, not found; size and rate limits; reconnection) | `src/pqch_p2p.*` |

The share chain is PQCH's own, with P2Pool's rules: P2Pool's `side_chain.cpp` is built around Monero's
transaction format (one-time keys, derived outputs, merge-mining proofs). P2Pool's networking, stratum server
and the rest are reused in phases B and C.

Commands (development):

```
p2pool --pqchd-check 127.0.0.1:<rpcport> <datadir>/<network>/.cookie        # can the pool work with this node?
p2pool --pqch-pool 127.0.0.1:<rpcport> <cookie> <P2MR address>,<P2MR address> \
       [--share-time 10] [--min-diff 100000] [--seconds 0] [--log pqch-pool.jsonl] \
       [--p2p-port 37890] [--peer <ip:port>]... [--network main|testnet|regtest]
# "-" instead of the addresses: a node without miners (needs --network)
```

Acceptance tests against a real `pqchd` (also in CI, job "Pool against pqchd"):
`tests/pqch/regtest_mine.sh`, `tests/pqch/regtest_pool.sh` and `tests/pqch/regtest_two_pools.sh` (two pqchd and
two pool nodes, each with its own miner, plus a third node without miners that joins halfway: same share
chain tip on all three, every block pays the PPLNS split of the common chain, each node's blocks pay the other
node's miner) (the second one checks, independently of the
pool's code, that every coinbase on chain equals the PPLNS split of the share chain). On regtest the main
difficulty is minimal, so every share is also a block; shares that are not blocks are covered by the unit tests
and by the testnet in phase B.

The original P2Pool README follows in `README.md`.
