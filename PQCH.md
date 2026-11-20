# pqch-pool

Decentralized mining pool for **PQCH (PostQuantum Cash)**: a fork of [P2Pool](https://github.com/SChernykh/p2pool)
v4.18.1 (commit `abc8335`, GPL-3.0, by SChernykh and contributors) with PQCH as the main chain.

There is no operator and no account: the pool nodes keep a chain of shares among themselves, and every
PQCH block found by the pool pays all miners of the PPLNS window directly in its coinbase, one P2MR
output each.

Status: **phase A in progress** (see `contrib/pqch/POOL.md` in the PQCH Core repository).

| Phase | What | Status |
| --- | --- | --- |
| A | Fork, main chain: block template, coinbase, proof of work and block submission with `pqchd`; one pool node in regtest | in progress |
| B | Pool network: share chain between nodes, PPLNS, PQCH parameters | planned |
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

The original P2Pool README follows in `README.md`.
