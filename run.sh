#!/usr/bin/env bash
# B-283 run.sh — デモ一発起動(ビルド → Go 操縦席起動 → URL 表示)
#
# 契約: DESIGN.md §2/§5。swarmd(Go シングルプロセス)が Cロボ×N を os/exec で起動し、
# http :8080 で UI を配信、WS で browser と双方向、LAN の実 8609 を観測して中継する。
# 本スクリプトは「make → go build swarmd → swarmd 起動 → ブラウザ URL 表示」だけを行う。
#
# 使い方:
#   ./run.sh              # ビルドして swarmd をフォアグラウンド起動
#   ./run.sh --check      # 起動前に interop A〜D を実行(全緑でなければ中止)
#   ./run.sh --build-only # ビルドのみ(起動しない)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

DO_CHECK=0 BUILD_ONLY=0
for a in "$@"; do
  case "$a" in
    --check)      DO_CHECK=1 ;;
    --build-only) BUILD_ONLY=1 ;;
    -h|--help)    grep -E '^#( |$)' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown arg: $a" >&2; exit 2 ;;
  esac
done

echo "[1/4] make (C: libccnx.a / swarm_robot / codec_test)"
make -C "$ROOT" all

echo "[2/4] go build swarmd (Go: 操縦席 + package ccnx 第2実装)"
( cd "$ROOT/server" && go build -o swarmd . )

if [ "$DO_CHECK" -eq 1 ]; then
  echo "[*] interop check (INTEROP.md 命題 A〜D)"
  bash "$ROOT/interop/run.sh" || { echo "interop RED — 起動中止(WIRE.md で裁定せよ)" >&2; exit 1; }
fi

if [ "$BUILD_ONLY" -eq 1 ]; then
  echo "build-only 完了: server/swarmd / client/swarm_robot / client/codec_test"
  exit 0
fi

URL="http://localhost:8080"
echo "[3/4] starting swarmd — 開いてください: $URL"
echo "      (swarmd が Cロボ群を exec。Wireshark は  lo0 udp portrange 9200-9300  に張る)"
echo "[4/4] Ctrl-C で停止"

# 可能ならブラウザを自動で開く(失敗しても無視)
( sleep 1; { command -v open >/dev/null && open "$URL"; } || { command -v xdg-open >/dev/null && xdg-open "$URL"; } ) >/dev/null 2>&1 &

exec "$ROOT/server/swarmd"
