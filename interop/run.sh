#!/usr/bin/env bash
# B-283 interop/run.sh — C↔Go ワイヤ相互運用ハーネス(INTEROP.md の機械判定)
#
# 契約: docs/INTEROP.md。独立2実装 C(libccnx.a)/ Go(package ccnx)が
# WIRE.md=RFC8609 の同一解釈に収束していることを、第三者再現可能に証明する。
#
# 判定の唯一の接点(INTEROP §0)= 既存の薄い試験 CLI:
#   C:  client/codec_test  {vectors|encode <uri> <I|C> [payloadHex]|decode <HEX>|selftest}
#   Go: server/ccnxcli     {vectors|encode <uri> <I|C> [payloadHex]|decode <HEX>}
#       + go test ./ccnx
# 本スクリプトはこの2 CLI を orchestrate するだけで、8609 バイトを自前で組まない
# (相互運用の証明を第3の実装で薄めない)。
#
# 実行する判定(INTEROP §4 合否サマリ):
#   A  vectors diff              : C-encode == Go-encode かつ WIRE §6 期待値(golden)に一致
#   B1 C→Go decode(3ベクタ)     : kind/uri/payload 一致
#   B2 Go→C decode(3ベクタ)     : 同上
#   C1 C selftest                : exit 0 / PASS
#   C2 Go go test ./ccnx         : exit 0 / ok
#   D  交差 round-trip(N件)      : 相手が decode→再 encode してバイト一致
#   E  否定ベクタ                : WIRE §7.2/§7.3 の拒否条件を両実装が同一に拒否
#                                  (T_PAD in Name / Message 前 TLV / PacketType と
#                                   Message 型の不整合 / 空名前・T_PAD の encode 拒否)
#
# exit 0 = 全緑。1つでも FAIL なら exit 1 と相違内容を出力。

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

CLIENT="$ROOT/client"
SERVER="$ROOT/server"
CODEC_TEST="$CLIENT/codec_test"
CCNXCLI="$SERVER/ccnxcli"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/b283-interop.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

pass=0 fail=0
ok()   { printf '  PASS  %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  FAIL  %s\n' "$1"; fail=$((fail+1)); }
die()  { printf 'FATAL: %s\n' "$1" >&2; exit 2; }
sect() { printf '\n== %s ==\n' "$1"; }

# ---- WIRE §6 golden(大文字16進, 空白なし)。C/Go 双方がこれに一致すること ----
golden_hex() {
  case "$1" in
    /area/A-2) echo "0100001F08000008000100130000000F000100046172656100010003412D32" ;;
    /task/rescue/zone/A-2/carriers/2) echo "01000042080000080001003600000032000100047461736B00010006726573637565000100047A6F6E6500010003412D320001000863617272696572730001000132" ;;
    /star/7/claim) echo "010100310000000800020025000000160001000473746172000100013700010005636C61696D00010007726F626F742D33" ;;
    *) echo "" ;;
  esac
}
# 注: golden は WIRE.md §6 のベクタから機械転記。ズレを疑う場合は WIRE.md を正とする。

# 最初に相違するバイト位置(0起点)を返す。無ければ -1
first_diff() {
  local a="$1" b="$2" i=0 n=${#1}
  [ ${#2} -lt "$n" ] && n=${#2}
  while [ $i -lt "$n" ]; do
    if [ "${a:$i:2}" != "${b:$i:2}" ]; then echo $((i/2)); return; fi
    i=$((i+2))
  done
  if [ ${#a} -ne ${#b} ]; then echo $((n/2)); return; fi
  echo -1
}

upper() { printf '%s' "$1" | tr 'a-f' 'A-F'; }

# ================================================================
# 0. 前提ビルド
# ================================================================
sect "0. build prerequisites"
if [ ! -x "$CODEC_TEST" ]; then
  echo "  building C (make all)..."
  make -C "$ROOT" all >/dev/null || die "C ビルド失敗(make all)。C 担当のソース未着か。"
fi
[ -x "$CODEC_TEST" ] || die "client/codec_test が無い。"

if [ ! -x "$CCNXCLI" ]; then
  echo "  building Go ccnxcli..."
  built=0
  for pkg in ./ccnx/cmd/ccnxcli ./cmd/ccnxcli ./ccnxcli; do
    if ( cd "$SERVER" && go build -o ccnxcli "$pkg" ) >/dev/null 2>&1; then built=1; break; fi
  done
  [ $built -eq 1 ] || die "server/ccnxcli をビルドできない。Go 担当が server/ccnx/cmd/ccnxcli を提供しているか確認。"
fi
[ -x "$CCNXCLI" ] || die "server/ccnxcli が無い。"
echo "  C:  $CODEC_TEST"
echo "  Go: $CCNXCLI"

# ベクタ定義(uri, I|C, payloadHex)
V_URIS=("/area/A-2" "/task/rescue/zone/A-2/carriers/2" "/star/7/claim")
V_KIND=("I" "I" "C")
V_PAY=("" "" "726F626F742D33")   # robot-3

# ================================================================
# A. vectors diff(C==Go かつ golden 一致)
# ================================================================
sect "A. byte-identical encode (vectors)"
"$CODEC_TEST" vectors > "$TMP/c_vectors.txt" 2>/dev/null || bad "C codec_test vectors が異常終了"
"$CCNXCLI"   vectors  > "$TMP/go_vectors.txt" 2>/dev/null || bad "Go ccnxcli vectors が異常終了"
cp "$TMP/c_vectors.txt" /tmp/b283_c_vectors.txt 2>/dev/null || true
cp "$TMP/go_vectors.txt" /tmp/b283_go_vectors.txt 2>/dev/null || true

if diff -u "$TMP/c_vectors.txt" "$TMP/go_vectors.txt" > "$TMP/vdiff.txt" 2>&1; then
  ok "C vectors == Go vectors (diff 差分0)"
else
  bad "vectors diff に差分あり:"
  sed 's/^/        /' "$TMP/vdiff.txt"
fi

# golden 照合(C 側 vectors を基準に、各行 name<TAB>HEX を WIRE §6 期待値と比較)
while IFS=$'\t' read -r name hex; do
  [ -z "$name" ] && continue
  g="$(golden_hex "$name")"
  if [ -z "$g" ]; then continue; fi
  h="$(upper "$hex")"
  if [ "$h" = "$g" ]; then
    ok "golden 一致: $name"
  else
    off="$(first_diff "$h" "$g")"
    bad "golden 不一致: $name (相違 byte offset=$off)"
    printf '        got   : %s\n' "$h"
    printf '        expect: %s\n' "$g"
  fi
done < "$TMP/c_vectors.txt"

# ================================================================
# B. 相互 decode(両方向 × 3ベクタ = 6)
# ================================================================
decode_check() {
  # $1=方向ラベル $2=decoderCLI... expected: $exp_kind/$exp_uri/$exp_pay
  local label="$1"; shift
  local out="$1"
  local dk du dp
  dk="$(printf '%s' "$out" | cut -f1)"
  du="$(printf '%s' "$out" | cut -f2)"
  dp="$(printf '%s' "$out" | cut -f3)"
  dp="$(upper "$dp")"
  if [ "$dk" = "$EXP_KIND" ] && [ "$du" = "$EXP_URI" ] && [ "$dp" = "$EXP_PAY" ]; then
    ok "$label : $EXP_KIND $EXP_URI"
  else
    bad "$label : 原本 [$EXP_KIND|$EXP_URI|$EXP_PAY] ≠ 復元 [$dk|$du|$dp]"
  fi
}

sect "B1. C encode -> Go decode"
for i in 0 1 2; do
  uri="${V_URIS[$i]}"; kind="${V_KIND[$i]}"; pay="${V_PAY[$i]}"
  [ "$kind" = "I" ] && EXP_KIND="Interest" || EXP_KIND="ContentObject"
  EXP_URI="$uri"; EXP_PAY="$(upper "$pay")"
  HEX="$("$CODEC_TEST" encode "$uri" "$kind" $pay 2>/dev/null)"
  [ -z "$HEX" ] && { bad "B1 C encode 失敗: $uri"; continue; }
  OUT="$("$CCNXCLI" decode "$HEX" 2>/dev/null)" || { bad "B1 Go decode 異常終了: $uri"; continue; }
  decode_check "C→Go" "$OUT"
done

sect "B2. Go encode -> C decode"
for i in 0 1 2; do
  uri="${V_URIS[$i]}"; kind="${V_KIND[$i]}"; pay="${V_PAY[$i]}"
  [ "$kind" = "I" ] && EXP_KIND="Interest" || EXP_KIND="ContentObject"
  EXP_URI="$uri"; EXP_PAY="$(upper "$pay")"
  HEX="$("$CCNXCLI" encode "$uri" "$kind" $pay 2>/dev/null)"
  [ -z "$HEX" ] && { bad "B2 Go encode 失敗: $uri"; continue; }
  OUT="$("$CODEC_TEST" decode "$HEX" 2>/dev/null)" || { bad "B2 C decode 異常終了: $uri"; continue; }
  decode_check "Go→C" "$OUT"
done

# ================================================================
# C. 各実装 単独 self-test
# ================================================================
sect "C1. C selftest (codec_test selftest)"
if OUT="$("$CODEC_TEST" selftest 2>&1)"; then
  if printf '%s' "$OUT" | grep -q "PASS"; then ok "C selftest PASS"; else bad "C selftest exit0 だが PASS 行無し: $OUT"; fi
else
  bad "C selftest FAIL: $(printf '%s' "$OUT" | tail -1)"
fi

sect "C2. Go go test ./ccnx"
if OUT="$(cd "$SERVER" && go test ./ccnx 2>&1)"; then
  ok "Go go test ./ccnx ok"
else
  bad "Go go test ./ccnx FAIL:"
  printf '%s\n' "$OUT" | sed 's/^/        /'
fi

# ================================================================
# D. 交差 round-trip(N件): 相手が decode→再encode してバイト一致
# ================================================================
sect "D. cross round-trip (N random)"
N=8
rt_c_to_go=0; rt_go_to_c=0
for k in $(seq 1 $N); do
  # ランダム名(WIRE §3: ASCII 英数と '-'、空セグメント禁止)。半分にペイロードを付ける。
  uri="/r/$k/seg-$RANDOM"
  if [ $((k % 2)) -eq 0 ]; then kind="C"; pay="$(printf 'p%d' "$k" | xxd -p | tr -d '\n' | tr 'a-f' 'A-F')"; else kind="I"; pay=""; fi

  # C→Go→再encode==C
  CHEX="$(upper "$("$CODEC_TEST" encode "$uri" "$kind" $pay 2>/dev/null)")"
  GDEC="$("$CCNXCLI" decode "$CHEX" 2>/dev/null)"
  gk="$(printf '%s' "$GDEC" | cut -f1)"; gu="$(printf '%s' "$GDEC" | cut -f2)"; gp="$(printf '%s' "$GDEC" | cut -f3)"
  gkind="I"; [ "$gk" = "ContentObject" ] && gkind="C"
  GRE="$(upper "$("$CCNXCLI" encode "$gu" "$gkind" $gp 2>/dev/null)")"
  if [ -n "$CHEX" ] && [ "$CHEX" = "$GRE" ]; then rt_c_to_go=$((rt_c_to_go+1)); else
    off="$(first_diff "$CHEX" "$GRE")"; bad "D C→Go→re: $uri (offset=$off) C=$CHEX GoRe=$GRE"
  fi

  # Go→C→再encode==Go
  GHEX="$(upper "$("$CCNXCLI" encode "$uri" "$kind" $pay 2>/dev/null)")"
  CDEC="$("$CODEC_TEST" decode "$GHEX" 2>/dev/null)"
  ck="$(printf '%s' "$CDEC" | cut -f1)"; cu="$(printf '%s' "$CDEC" | cut -f2)"; cp="$(printf '%s' "$CDEC" | cut -f3)"
  ckind="I"; [ "$ck" = "ContentObject" ] && ckind="C"
  CRE="$(upper "$("$CODEC_TEST" encode "$cu" "$ckind" $cp 2>/dev/null)")"
  if [ -n "$GHEX" ] && [ "$GHEX" = "$CRE" ]; then rt_go_to_c=$((rt_go_to_c+1)); else
    off="$(first_diff "$GHEX" "$CRE")"; bad "D Go→C→re: $uri (offset=$off) Go=$GHEX CRe=$CRE"
  fi
done
[ $rt_c_to_go -eq $N ] && ok "D C→Go→re-encode 一致 ($N/$N)" || bad "D C→Go→re-encode ($rt_c_to_go/$N)"
[ $rt_go_to_c -eq $N ] && ok "D Go→C→re-encode 一致 ($N/$N)" || bad "D Go→C→re-encode ($rt_go_to_c/$N)"

# ================================================================
# E. 否定ベクタ(WIRE §7.2 / §7.3。両実装が同一に拒否すること)
# ================================================================
sect "E. negative vectors (both must reject)"

# decode 拒否: ラベル<TAB>HEX。WIRE §7.3 D12/D13/D14 の恒久ベクタ。
NEG_DEC_LABELS=(
  "D14 T_PAD in Name"
  "D12 TLV before Message"
  "D13 PT_RETURN + T_OBJECT body"
  "D13 PT_INTEREST + T_OBJECT body"
)
NEG_DEC_HEX=(
  "010000150800000800010009000000050FFE000178"
  "010000230800000800030000000100130000000F000100046172656100010003412D32"
  "010200310000000800020025000000160001000473746172000100013700010005636C61696D00010007726F626F742D33"
  "010000310000000800020025000000160001000473746172000100013700010005636C61696D00010007726F626F742D33"
)
for i in "${!NEG_DEC_HEX[@]}"; do
  label="${NEG_DEC_LABELS[$i]}"; hexv="${NEG_DEC_HEX[$i]}"
  if "$CODEC_TEST" decode "$hexv" >/dev/null 2>&1; then
    bad "E C decode が誤って受理: $label"
  else
    ok "E C decode 拒否: $label"
  fi
  if "$CCNXCLI" decode "$hexv" >/dev/null 2>&1; then
    bad "E Go decode が誤って受理: $label"
  else
    ok "E Go decode 拒否: $label"
  fi
done

# decode 受理の対照(前方互換 A1): 同じ 00030000 でも Message の後ろなら受理
POS_HEX="010000230800000800010013"'0000000F000100046172656100010003412D32'"00030000"
if "$CODEC_TEST" decode "$POS_HEX" >/dev/null 2>&1; then
  ok "E C decode 受理: A1 TLV after Message"
else
  bad "E C decode が誤って拒否: A1 TLV after Message"
fi
if "$CCNXCLI" decode "$POS_HEX" >/dev/null 2>&1; then
  ok "E Go decode 受理: A1 TLV after Message"
else
  bad "E Go decode が誤って拒否: A1 TLV after Message"
fi

# encode 拒否: E2(空名前 CO)/ E8(T_PAD セグメント)
if "$CODEC_TEST" encode "/" C >/dev/null 2>&1; then
  bad "E C encode が誤って成功: 空名前 ContentObject"
else
  ok "E C encode 拒否: 空名前 ContentObject"
fi
if "$CCNXCLI" encode "/" C >/dev/null 2>&1; then
  bad "E Go encode が誤って成功: 空名前 ContentObject"
else
  ok "E Go encode 拒否: 空名前 ContentObject"
fi
if "$CODEC_TEST" encode "/0x0FFE=x" I >/dev/null 2>&1; then
  bad "E C encode が誤って成功: T_PAD セグメント"
else
  ok "E C encode 拒否: T_PAD セグメント"
fi
if "$CCNXCLI" encode "/0x0FFE=x" I >/dev/null 2>&1; then
  bad "E Go encode が誤って成功: T_PAD セグメント"
else
  ok "E Go encode 拒否: T_PAD セグメント"
fi

# encode 拒否: ReturnCode=0 の Interest Return(RFC 8609 §3.2.3.3
# "A return code of '0' MUST NOT be used")。R 01 は正当(対照)。
if "$CODEC_TEST" encode "/a" R 00 >/dev/null 2>&1; then
  bad "E C encode が誤って成功: ReturnCode=0 InterestReturn"
else
  ok "E C encode 拒否: ReturnCode=0 InterestReturn"
fi
if "$CCNXCLI" encode "/a" R 00 >/dev/null 2>&1; then
  bad "E Go encode が誤って成功: ReturnCode=0 InterestReturn"
else
  ok "E Go encode 拒否: ReturnCode=0 InterestReturn"
fi
# 対照: ReturnCode=1(NO_ROUTE)は両側 encode 成功しバイト一致
C_R01="$("$CODEC_TEST" encode "/a" R 01 2>/dev/null)"
GO_R01="$("$CCNXCLI" encode "/a" R 01 2>/dev/null)"
if [ -n "$C_R01" ] && [ "$C_R01" = "$GO_R01" ]; then
  ok "E ReturnCode=1 は C/Go バイト一致: $C_R01"
else
  bad "E ReturnCode=1 不一致 C=$C_R01 Go=$GO_R01"
fi

# ================================================================
# F. フォワーダ(RFC 8569)C↔Go 等価性 — 2 段で機械判定する。
#   F1 表レベル: PIT/FIB が返す判定フラグ(集約/retransmit/larger/NO_ROUTE)。
#        C  = codec_test fwdtest(ccnx_tables.c 直叩き)
#        Go = go test -run 'Forwarder|PITExpired'(portal_test.go)
#   F2 Portal レベル: Portal がそのフラグに従い「実際にワイヤへ出した datagram」を
#      loopback + tap で観測する。表フラグは正しいのに Portal が転送/抑止/NO_ROUTE/
#      前ホップ処理を誤る回帰(例: 集約 Interest を再転送、untrusted CO をキャッシュ)は
#      F1 では緑のままだが F2 で赤くなる。
#        C  = codec_test porttest(ccnx.c を実 UDP+tap で駆動)
#        Go = go test -run 'PortalForward|PortalPrevHop'(portal_test.go 統合)
#   検査項目(両側同一): 新規 Interest 転送(HopLimit-1)/ 別前ホップの集約抑止 /
#     同一前ホップ再送と larger HopLimit の MUST forward / HopLimit=1 の無送出 /
#     唯一 nexthop==到来面の NO_ROUTE Interest Return / 前ホップ CO(trusted=CS格納
#     +充足overheard=0、untrusted=CS非格納だが overheard=1 配送)。
# ================================================================
sect "F. forwarder equivalence (PIT/FIB + Portal on-wire, RFC 8569)"
# F1 table-level flags
if OUT="$("$CODEC_TEST" fwdtest 2>&1)"; then
  if printf '%s' "$OUT" | grep -q "PASS"; then ok "F1 C fwdtest PASS (表: 期限切れPIT/maxHopLimit/NO_ROUTE集合)"; else bad "F1 C fwdtest exit0 だが PASS 行無し: $OUT"; fi
else
  bad "F1 C fwdtest FAIL: $(printf '%s' "$OUT" | tail -1)"
fi
if OUT="$(cd "$SERVER" && go test ./ccnx -run 'Forwarder|PITExpired|ReturnCode' 2>&1)"; then
  ok "F1 Go table test ok (portal_test.go: 同一フラグ不変条件)"
else
  bad "F1 Go table test FAIL: $(printf '%s' "$OUT" | tail -2)"
fi
# F2 Portal on-wire behavior (tap 観測)
if OUT="$("$CODEC_TEST" porttest 2>&1)"; then
  if printf '%s' "$OUT" | grep -q "PASS"; then ok "F2 C porttest PASS (実送信: 転送/抑止/NO_ROUTE/前ホップCO)"; else bad "F2 C porttest exit0 だが PASS 行無し: $OUT"; fi
else
  bad "F2 C porttest FAIL: $(printf '%s' "$OUT" | tail -1)"
fi
if OUT="$(cd "$SERVER" && go test ./ccnx -run 'PortalForward|PortalPrevHop' 2>&1)"; then
  ok "F2 Go portal integration ok (portal_test.go: tap 観測で同一挙動)"
else
  bad "F2 Go portal integration FAIL: $(printf '%s' "$OUT" | tail -2)"
fi

# ================================================================
# 合否サマリ
# ================================================================
sect "SUMMARY"
printf '  PASS=%d  FAIL=%d\n' "$pass" "$fail"
if [ "$fail" -eq 0 ]; then
  echo "  ==> INTEROP GREEN: 独立2実装が同一ワイヤ(WIRE.md=RFC8609)に収束"
  exit 0
else
  echo "  ==> INTEROP RED: 上記 FAIL を WIRE.md で裁定せよ"
  exit 1
fi
