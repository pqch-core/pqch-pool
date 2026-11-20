#!/usr/bin/env python3
# Copyright (c) 2026 The PQCH developers. GPL-3.0 (see COPYING).
#
# Verifica, independentemente do código do pool, que cada bloco do pool na testnet paga a divisão PPLNS
# da cadeia de partilhas. Corre num nó do pool (lê o registo pool.jsonl e pergunta os blocos ao pqchd local):
#
#   sudo python3 deploy/verificar_pool_testnet.py --user pqch --datadir /home/pqch/.pqch [--blocos 500]
#
# Para cada um dos últimos N blocos da cadeia principal: se a coinbase traz o id de uma partilha conhecida
# (scriptSig = altura | 0x20 id | 0x08 nonce extra), é um bloco do pool. A divisão esperada é calculada a
# partir das partilhas (a do bloco e as anteriores, até 2160), com as mesmas regras de arredondamento
# (resto para quem encontrou o bloco) e comparada com as saídas da coinbase. Só lê; não muda nada.
import argparse
import json
import subprocess
import sys

PPLNS_WINDOW = 2160
MAX_PAYOUTS = 500

ap = argparse.ArgumentParser()
ap.add_argument("--user", required=True)
ap.add_argument("--datadir", required=True)
ap.add_argument("--log", default="/var/lib/pqch-pool-testnet/pool.jsonl")
ap.add_argument("--blocos", type=int, default=500)
ap.add_argument("--rede", default="testnet", choices=["testnet", "regtest"])  # regtest: só para testar este script
ap.add_argument("--cli", default="/usr/local/bin/pqch-cli")
ap.add_argument("--rpcport")
a = ap.parse_args()


def cli(*args):
    cmd = ["sudo", "-u", a.user] if a.user != "-" else []
    cmd += [a.cli, f"-{a.rede}", f"-datadir={a.datadir}"] + ([f"-rpcport={a.rpcport}"] if a.rpcport else [])
    out = subprocess.run(cmd + list(args),
                         check=True, capture_output=True, text=True).stdout
    return json.loads(out) if out.strip()[:1] in "[{" else out.strip()


shares = {}
for line in open(a.log):
    d = json.loads(line)
    if d["type"] == "share":
        shares[d["id"]] = d
print(f"{len(shares)} partilhas no registo {a.log}")


def expected_split(share, total):
    weights = {}
    s, n = share, 0
    while s is not None and n < PPLNS_WINDOW:
        weights[s["miner"]] = weights.get(s["miner"], 0) + s["difficulty"]
        s, n = shares.get(s["parent"]), n + 1
    if n < min(PPLNS_WINDOW, share["height"] + 1):
        return None, n  # falta história no registo
    merged = sorted(weights.items(), key=lambda kv: -kv[1])[:MAX_PAYOUTS]  # empates: raros; avisa-se abaixo
    wsum = sum(w for _, w in merged)
    out = {m: total * w // wsum for m, w in merged}
    rem = total - sum(out.values())
    out[share["miner"] if share["miner"] in out else merged[0][0]] += rem
    return {m: v for m, v in out.items() if v}, n


tip = cli("getblockcount")
tip = int(tip)
pool_blocks = ok = 0
falhas = []
for h in range(max(1, tip - a.blocos + 1), tip + 1):
    b = cli("getblock", cli("getblockhash", str(h)), "2")
    cb = b["tx"][0]
    sig = bytes.fromhex(cb["vin"][0]["coinbase"])
    n = sig[0]
    if 1 <= n <= 8 and len(sig) >= 1 + n + 33 and sig[1 + n] == 0x20:
        sid = sig[2 + n:34 + n][::-1].hex()
    else:
        continue
    if sid not in shares:
        continue
    pool_blocks += 1
    total = sum(round(o["value"] * 100_000_000) for o in cb["vout"])
    exp, depth = expected_split(shares[sid], total)
    if exp is None:
        print(f"bloco {h}: partilha {shares[sid]['height']} sem história completa no registo ({depth}); não verificado")
        continue
    got = {}
    for o in cb["vout"]:
        v = round(o["value"] * 100_000_000)
        if v:
            addr = o["scriptPubKey"].get("address", "?")
            got[addr] = got.get(addr, 0) + v
    if got == exp:
        ok += 1
        quem = ", ".join(f"{m[:12]}…{m[-6:]} {v / 1e8:.8f}" for m, v in sorted(got.items(), key=lambda kv: -kv[1]))
        print(f"bloco {h} OK  (partilha {shares[sid]['height']}, {depth} partilhas na janela): {quem}")
    else:
        falhas.append(h)
        print(f"bloco {h} ERRADO: coinbase {got} != PPLNS {exp}")

print(f"\n{pool_blocks} blocos do pool nos últimos {a.blocos}; {ok} com a divisão PPLNS certa; {len(falhas)} errados")
sys.exit(1 if falhas else 0)
