#!/usr/bin/env bash
# Copyright (c) 2026 The PQCH developers. GPL-3.0 (see COPYING).
#
# Compila o pqch-pool deste clone e instala /usr/local/bin/pqch-pool (nó do pool e minerador).
#
#   sudo deploy/compilar.sh
#
# Usado por instalar_pool_testnet.sh; sozinho serve para uma máquina que só vai minerar (pqch-pool --pqch-miner),
# sem nó do pool nem serviço. Pacotes: cmake, libuv, libzmq, libcurl, nasm (Debian, Ubuntu, Kali).
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "correr com sudo"; exit 1; }
REPO="$(cd "$(dirname "$0")/.." && pwd)"

apt-get install -y -q --no-install-recommends build-essential cmake git libuv1-dev libzmq3-dev libcurl4-openssl-dev nasm >/dev/null
git -C "$REPO" submodule update --init --depth 1 external/src/RandomX external/src/cppzmq external/src/libuv \
	external/src/libzmq external/src/rapidjson external/src/robin-hood-hashing external/src/curl external/src/miniupnp >/dev/null
git -C "$REPO" submodule update --init --depth 1 external/src/grpc >/dev/null
git -C "$REPO/external/src/grpc" submodule update --init --depth 1 third_party/boringssl-with-bazel >/dev/null
cmake -S "$REPO" -B "$REPO/build-testnet" -DWITH_GRPC=OFF -DWITH_MERGE_MINING_DONATION=OFF -DWITH_UPNP=OFF >/dev/null
cmake --build "$REPO/build-testnet" -j"$(nproc)" >/dev/null
install -m 0755 "$REPO/build-testnet/p2pool" /usr/local/bin/pqch-pool
echo "pqch-pool $(git -C "$REPO" rev-parse --short HEAD) instalado em /usr/local/bin/pqch-pool"
