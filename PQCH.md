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
| B | Pool network: share chain between nodes, PPLNS, PQCH parameters | done (regtest and testnet, 20 Nov 2026) |
| C | Miner with stratum | done (regtest and testnet, 21 Nov 2026) |
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
       [--p2p-port 37890] [--peer <ip:port>]... [--network main|testnet|regtest] [--fast] [--stratum <ip:port>]
# "-" instead of the addresses: a node without miners (needs --network)
p2pool --pqch-miner <pool node ip>:<stratum port> <P2MR address> [--threads N] [--fast] [--seconds N]
# phase C: a miner without a node; every share it finds is paid to its address by PPLNS
```

Acceptance tests against a real `pqchd` (also in CI, job "Pool against pqchd"):
`tests/pqch/regtest_mine.sh`, `tests/pqch/regtest_pool.sh` and `tests/pqch/regtest_two_pools.sh` (two pqchd and
two pool nodes, each with its own miner, plus a third node without miners that joins halfway: same share
chain tip on all three, every block pays the PPLNS split of the common chain, each node's blocks pay the other
node's miner) (the second one checks, independently of the
pool's code, that every coinbase on chain equals the PPLNS split of the share chain). On regtest the main
difficulty is minimal, so every share is also a block; shares that are not blocks are covered by the unit tests
and by the testnet in phase B.

Phase B on testnet: `deploy/instalar_pool_testnet.sh`. Layout: the anchor VPS listens on 37890 (1 thread);
the Kali machine mines only through the pool (`--substituir-minerador` stops its direct miner); the home server
keeps mining the testnet directly, so pool blocks compete with solo blocks as they will on mainnet.
Criterion: both pool nodes on the same share chain tip, and every pool block on testnet pays the PPLNS split of
that chain. Met on 20 Nov 2026 (pqchd 0.3.1, pqch-pool 51ebbb6): within 15 minutes the two nodes shared one
chain of 150 shares (difficulty retargeted from 1000 to ~50000 at one share per 10 s) and found testnet blocks
21777-21782, found by both nodes, each paying both miners. `deploy/verificar_pool_testnet.py`, independent of
the pool's code, gave 6 of 6 blocks with the exact PPLNS split, run on each node with its own share log.

## Phase C: stratum

`src/pqch_stratum.*` (protocol and server) and `src/pqch_miner.*` (the miner). One JSON object per line, in the
style of the Monero stratum: `login` with a P2MR address, `job` pushed by the node (the 80-byte header with the
nonce zeroed, the share difficulty, the RandomX key), `submit` with a 32-bit nonce, `keepalived`. Each miner gets
its own work: its address in the coinbase and its own extra nonce, so its shares are paid to it by PPLNS like the
built-in miners'. The node recomputes every submitted hash (it never trusts the miner's), refuses addresses of
another network, and limits each connection (4 KiB lines, 20 messages/s, 10 invalid shares, 256 miners, 10
minutes idle) and each remote address: 8 connections at once, 20 new ones per minute, 30 s to log in, and an
address that breaks the protocol (bad JSON, too many messages, 10 invalid shares) is refused for 10 minutes; the
node verifies at most 10 submitted shares per second in total (a light-mode RandomX hash each). The installer
opens it to private networks (`--stratum-lan 37889`) or to everyone (`--stratum-publico 37889`, the anchor).

Test: `tests/pqch/regtest_stratum.sh` (also in CI): a pool node with one built-in miner and a stratum port, and
the stand-alone miner with another address; the miner's shares are accepted, both addresses have shares in the
chain, every block passes `deploy/verificar_pool_testnet.py` and the stratum miner is paid; a login with a
testnet address on regtest is refused; a 9th connection from one address is refused, and so is an address
that sent garbage.

Testnet (21 Nov 2026): the home server, with no pool node, ran `--pqch-miner` (2 threads, light mode, 80 H/s)
against the Kali node's stratum on the local network. After 90 minutes `deploy/verificar_pool_testnet.py` gave
435 pool blocks among the last 500, all 435 with the exact PPLNS split, and the server's address paid in each
recent block alongside the two pool nodes' addresses; the PPLNS window was full (2160 shares).

The original P2Pool README follows in `README.md`.
