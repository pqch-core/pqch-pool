#!/usr/bin/env bash
# Copyright (c) 2026 The PQCH developers. GPL-3.0 (see COPYING).
#
# Instala um nó do pool PQCH (pqch-pool) na testnet, ao lado de um pqchd que já esteja a correr na mesma máquina.
#
#   sudo deploy/instalar_pool_testnet.sh --user <utilizador do pqchd> --datadir <pasta de dados do pqchd> \
#        [--wallet <carteira> | --morada <pqct1p...>] [--threads N] [--fast] [--listen] [--peer <ip>:37890]... [--substituir-minerador]
#
#   âncora:  sudo deploy/instalar_pool_testnet.sh --user pqch --datadir /home/pqch/.pqch --threads 1 --listen
#   server:  sudo deploy/instalar_pool_testnet.sh --user pqch-minerador --datadir /var/lib/pqch-minerador \
#                 --threads 2 --fast --peer 187.127.89.83:37890
#   máquina só de pool (o minerador direto da testnet passa a ficar parado):
#            sudo deploy/instalar_pool_testnet.sh --user pqch-minerador --datadir /var/lib/pqch-minerador \
#                 --threads 4 --fast --peer 187.127.89.83:37890 --substituir-minerador
#
# Nó sem carteira (disablewallet=1): indique a morada das recompensas com --morada pqct1p...
#
# O que faz, por esta ordem:
#   1. compila o pqch-pool deste clone (pacotes: cmake, libuv, libzmq, libcurl, nasm) e instala /usr/local/bin/pqch-pool;
#   2. pede ao pqchd uma morada P2MR nova da carteira (a recompensa do pool vai para ela) e confirma com
#      --pqchd-check que o pool consegue trabalhar com o nó;
#   3. instala o serviço systemd "pqch-pool-testnet", com o mesmo utilizador do pqchd (para ler o cookie do
#      RPC sem mexer em permissões); registo de partilhas e blocos em /var/lib/pqch-pool-testnet/pool.jsonl;
#   4. com --listen: abre a porta 37890/tcp na firewall (só esta; o RPC do pqchd continua só em 127.0.0.1).
# Com --substituir-minerador: para o pqch-minerar-testnet (minerador direto do instalar.sh minerar) e impede-o de
#   voltar a arrancar enquanto existir /var/lib/pqch-pool-testnet/minerador-substituido (drop-in com ConditionPathExists, que sobrevive a uma
#   reinstalação do pqchd); reinicia o pqchd-testnet para libertar a memória do modo rápido do minerador direto.
#   Para voltar atrás: sudo rm -r /etc/systemd/system/pqch-minerar-testnet.service.d && sudo systemctl daemon-reload
#   && sudo systemctl disable --now pqch-pool-testnet && sudo systemctl start pqch-minerar-testnet
# Pode correr outra vez (recompila e reinstala o serviço; não pede morada nova se já houver uma).
set -euo pipefail

POOL_PORT=37890
RPC_PORT=39332          # testnet
NODE_USER=""; DATADIR=""; WALLET=""; MORADA=""; THREADS=1; FAST=""; LISTEN=0; PEERS=(); SUBST=0
while (( $# )); do
	case "$1" in
		--user) NODE_USER=$2; shift 2 ;;
		--datadir) DATADIR=$2; shift 2 ;;
		--wallet) WALLET=$2; shift 2 ;;
		--morada) MORADA=$2; shift 2 ;;
		--threads) THREADS=$2; shift 2 ;;
		--fast) FAST="--fast"; shift ;;
		--listen) LISTEN=1; shift ;;
		--peer) PEERS+=("$2"); shift 2 ;;
		--substituir-minerador) SUBST=1; shift ;;
		*) echo "opção desconhecida: $1"; exit 1 ;;
	esac
done
[[ $EUID -eq 0 ]] || { echo "correr com sudo"; exit 1; }
[[ -n "$NODE_USER" && -n "$DATADIR" ]] || { echo "faltam --user e --datadir (ver o início do script)"; exit 1; }
id "$NODE_USER" >/dev/null || exit 1
[[ "$THREADS" =~ ^[0-9]+$ ]] && (( THREADS >= 1 && THREADS <= 64 )) || { echo "--threads entre 1 e 64"; exit 1; }
(( LISTEN || ${#PEERS[@]} )) || { echo "indique --listen e/ou pelo menos um --peer"; exit 1; }
(( THREADS <= $(nproc) )) || { echo "--threads $THREADS é mais do que os $(nproc) processadores desta máquina"; exit 1; }
STATE=/var/lib/pqch-pool-testnet
if (( SUBST )); then
	echo "== 0/4 Parar o minerador direto"
	systemctl list-unit-files pqch-minerar-testnet.service | grep -q pqch-minerar-testnet || { echo "não há pqch-minerar-testnet nesta máquina"; exit 1; }
	install -d -m 0750 -o "$NODE_USER" -g "$NODE_USER" "$STATE"
	touch "$STATE/minerador-substituido"
	install -d /etc/systemd/system/pqch-minerar-testnet.service.d
	cat > /etc/systemd/system/pqch-minerar-testnet.service.d/pool.conf <<DROPIN
# Instalado por instalar_pool_testnet.sh --substituir-minerador: com o pool instalado, esta máquina mina pelo pool.
[Unit]
ConditionPathExists=!$STATE/minerador-substituido
DROPIN
	systemctl daemon-reload
	systemctl stop pqch-minerar-testnet
	systemctl disable pqch-minerar-testnet >/dev/null 2>&1 || true
	echo "a reiniciar o pqchd-testnet (liberta a memória do minerador direto)"
	systemctl restart pqchd-testnet
	sudo -u "$NODE_USER" /usr/local/bin/pqch-cli -testnet -datadir="$DATADIR" -rpcwait -rpcwaittimeout=300 getblockcount >/dev/null
	echo "minerador direto parado; pqchd-testnet a correr"
fi
if [[ -n "$FAST" ]]; then
	MEM_GB=$(awk '/MemAvailable/ {print int($2/1048576)}' /proc/meminfo)
	(( MEM_GB >= 3 )) || { echo "--fast precisa de ~2,1 GB livres; esta máquina tem ${MEM_GB} GB disponíveis"; exit 1; }
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cli() { sudo -u "$NODE_USER" /usr/local/bin/pqch-cli -testnet -datadir="$DATADIR" "$@"; }

echo "== 1/4 Compilar o pqch-pool"
apt-get install -y -q --no-install-recommends build-essential cmake git libuv1-dev libzmq3-dev libcurl4-openssl-dev nasm >/dev/null
git -C "$REPO" submodule update --init --depth 1 external/src/RandomX external/src/cppzmq external/src/libuv \
	external/src/libzmq external/src/rapidjson external/src/robin-hood-hashing external/src/curl external/src/miniupnp >/dev/null
git -C "$REPO" submodule update --init --depth 1 external/src/grpc >/dev/null
git -C "$REPO/external/src/grpc" submodule update --init --depth 1 third_party/boringssl-with-bazel >/dev/null
cmake -S "$REPO" -B "$REPO/build-testnet" -DWITH_GRPC=OFF -DWITH_MERGE_MINING_DONATION=OFF -DWITH_UPNP=OFF >/dev/null
cmake --build "$REPO/build-testnet" -j"$(nproc)" >/dev/null
install -m 0755 "$REPO/build-testnet/p2pool" /usr/local/bin/pqch-pool
echo "pqch-pool $(git -C "$REPO" rev-parse --short HEAD) instalado"

echo "== 2/4 Morada e ligação ao pqchd"
COOKIE=$(find "$DATADIR" -maxdepth 2 -name .cookie -path '*test*' | head -1)
[[ -n "$COOKIE" ]] || { echo "não encontrei o cookie da testnet em $DATADIR (o pqchd-testnet está a correr?)"; exit 1; }
install -d -m 0750 -o "$NODE_USER" -g "$NODE_USER" "$STATE"
if [[ -n "$MORADA" ]]; then
	ADDR=$MORADA
	cli validateaddress "$ADDR" | grep -q '"isvalid": true' || { echo "morada inválida para a testnet: $ADDR"; exit 1; }
	echo "$ADDR" > "$STATE/morada"; chown "$NODE_USER:$NODE_USER" "$STATE/morada"
elif [[ -s "$STATE/morada" ]]; then
	ADDR=$(cat "$STATE/morada")
else
	cli listwallets >/dev/null 2>&1 || { echo "este pqchd não tem carteira (disablewallet=1): indique a morada das recompensas com --morada pqct1p..."; exit 1; }
	[[ -n "$WALLET" ]] || WALLET=$(cli listwallets | python3 -c 'import sys,json; w=json.load(sys.stdin); print(w[0] if w else "")')
	# Depois de um reinício o pqchd pode não ter a carteira carregada: usa a primeira da pasta de dados.
	[[ -n "$WALLET" ]] || WALLET=$(cli listwalletdir | python3 -c 'import sys,json; w=json.load(sys.stdin)["wallets"]; print(w[0]["name"] if w else "")')
	[[ -z "$WALLET" ]] || cli loadwallet "$WALLET" >/dev/null 2>&1 || true
	[[ -n "$WALLET" ]] || { echo "o pqchd não tem carteira carregada; indique --wallet"; exit 1; }
	ADDR=$(cli -rpcwallet="$WALLET" getnewaddress "pqch-pool")
	echo "$ADDR" > "$STATE/morada"; chown "$NODE_USER:$NODE_USER" "$STATE/morada"
fi
[[ "$ADDR" == pqct1p* ]] || { echo "morada inesperada: $ADDR"; exit 1; }
echo "recompensas do pool para $ADDR (carteira do pqchd)"
sudo -u "$NODE_USER" /usr/local/bin/pqch-pool --pqchd-check 127.0.0.1:$RPC_PORT "$COOKIE"

echo "== 3/4 Serviço pqch-pool-testnet"
MINERS=$(printf "%s," $(for _ in $(seq 1 "$THREADS"); do echo "$ADDR"; done)); MINERS=${MINERS%,}
NETOPTS=""
(( LISTEN )) && NETOPTS="--p2p-port $POOL_PORT"
for p in "${PEERS[@]}"; do NETOPTS="$NETOPTS --peer $p"; done
cat > /etc/systemd/system/pqch-pool-testnet.service <<UNIT
[Unit]
Description=Nó do pool PQCH (testnet)
After=network-online.target pqchd-testnet.service
Wants=network-online.target

[Service]
User=$NODE_USER
Group=$NODE_USER
WorkingDirectory=$STATE
ExecStart=/usr/local/bin/pqch-pool --pqch-pool 127.0.0.1:$RPC_PORT $COOKIE $MINERS --network testnet --share-time 10 --min-diff 1000 $FAST $NETOPTS --log $STATE/pool.jsonl
Restart=on-failure
RestartSec=30
Nice=10
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=read-only
ReadWritePaths=$STATE

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload
systemctl enable pqch-pool-testnet >/dev/null
systemctl restart pqch-pool-testnet

echo "== 4/4 Firewall"
if (( LISTEN )); then
	if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
		ufw allow $POOL_PORT/tcp comment 'pqch-pool testnet' >/dev/null && echo "porta $POOL_PORT/tcp aberta"
	else
		echo "ufw não está ativa: abra a porta $POOL_PORT/tcp na firewall desta máquina"
	fi
else
	echo "nada a abrir (este nó liga-se para fora)"
fi
sleep 5
systemctl --no-pager --lines=5 status pqch-pool-testnet | sed -n '1,4p'
echo "Pronto. Registo: journalctl -u pqch-pool-testnet -f   Partilhas: $STATE/pool.jsonl"
