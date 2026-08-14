#!/usr/bin/env bash
# B-283 docs/interop_check.sh — 契約(DESIGN §5 手順4 / INTEROP §3.2)が名指しする入口。
# 実体は interop/run.sh(ビルド担当のハーネス)。ここは名前の別名に徹する。
exec bash "$(cd "$(dirname "$0")/.." && pwd)/interop/run.sh" "$@"
