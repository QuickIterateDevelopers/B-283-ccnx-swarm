# B-283 INTEROP.md — C↔Go 相互運用 試験手順と合否条件(単一の真実)

目的: **独立2実装(C `libccnx.a` / Go `package ccnx`)が RFC 8609 に忠実**であることを、
第三者が再現可能な機械判定で証明する。証明対象は 2 命題:

- **命題A(バイト一致 encode):** 同一の論理メッセージを両実装が encode すると、出力バイト列が**完全一致**する。
- **命題B(相互 decode):** 一方が encode したバイト列を他方が decode でき、名前・種別・ペイロードが原本と一致する
  (C→Go, Go→C の両方向)。

両命題が全ベクタ + ラウンドトリップで成立すれば「両者は WIRE.md=RFC8609 の同一解釈に収束している」=
相互運用の物証。基準は WIRE §6 のテストベクタ(3 本)+ 追加ラウンドトリップ。

---

## 0. 前提と CLI 契約(実装者はこの CLI を必ず提供する)

両実装に「16進バイト列を標準入出力でやり取りする」薄い試験 CLI を用意する。これが判定の唯一の接点。

### C 側: `codec_test`(client/ccnx_test.c から生成)
サブコマンドを持つ:
| 呼び出し | 動作 | 出力(stdout) |
|---------|------|--------------|
| `codec_test vectors` | WIRE §6 の 3 ベクタを内蔵の名前から encode | 各行 `name<TAB>HEX`(大文字16進, 空白なし) |
| `codec_test encode <uri> <I\|C\|R> [payloadHex] [rcHex]` | 指定名を Interest(I)/ContentObject(C)/**Interest Return(R)** で encode | 1 行 `HEX` |
| `codec_test decode <HEX>` | 16進を decode | 1 行 `kind<TAB>uri<TAB>payloadHex`(無payloadは空)。**InterestReturn のときのみ 4 列目 `rcHex`** |
| `codec_test selftest` | 内蔵ベクタ一致 + ランダム1000件 round-trip | 最終行 `PASS` or `FAIL: <理由>`、exit 0/1 |

### Go 側: `go test ./ccnx` + `ccnxcli`(server/ccnx のテスト用薄 main)
| 呼び出し | 動作 | 出力 |
|---------|------|------|
| `ccnxcli vectors` | 同じ 3 名を encode | C と同一書式 `name<TAB>HEX` |
| `ccnxcli encode <uri> <I\|C\|R> [payloadHex] [rcHex]` | encode | `HEX` |
| `ccnxcli decode <HEX>` | decode | `kind<TAB>uri<TAB>payloadHex`(+ InterestReturn 時のみ `<TAB>rcHex`) |
| `go test ./ccnx -run Vectors` | WIRE §6 期待16進との一致 + round-trip | `ok` / 失敗詳細, exit 0/1 |

HEX 表記規約(両実装で厳守): **大文字**, 1 バイト 2 桁, 区切りなし(例 `01000042...`)。
`kind` 語彙: `Interest` / `ContentObject` / `InterestReturn`。uri は WIRE §3 の正規形(先頭 `/`, 末尾 `/` 無し)。

### 0.1 Interest Return の CLI 契約(拡張。既存 2 種別の出力は不変)

★**互換性の絶対条件:** `Interest` / `ContentObject` の decode 出力は **3 列**(`kind<TAB>uri<TAB>payloadHex`)
のまま**不変**である。4 列目は `kind == InterestReturn` の行にのみ現れる。既存の A〜D 判定
(`interop/run.sh`)は 3 列前提で書かれており、この不変性が壊れると全判定が壊れる。

| 項目 | 契約 |
|------|------|
| encode 種別 | `I`=Interest / `C`=ContentObject / `R`=Interest Return。大文字 1 文字のみ受理 |
| `rcHex`(encode 第4引数) | `R` のときだけ意味を持つ。**2 桁大文字16進**(例 `01`)。省略時は `01`(NO_ROUTE)。`I`/`C` では無視 |
| decode 4 列目 | `kind == InterestReturn` のときのみ出力。値は固定ヘッダ オフセット5 の ReturnCode を **2 桁大文字16進**で表記 |
| バイト長 | Interest Return は元 Interest と**同一バイト長**(WIRE §1.3)。`encode <uri> R` の出力長は `encode <uri> I` と一致すること(判定に使える) |

ReturnCode 値(RFC 8609 §3.2.3 の割当。数値は RFC を正とする):

| 値 | 名前 | v1 フォワーダが生成するか |
|----|------|------------------------|
| `01` | T_RETURN_NO_ROUTE | **する**(FIB 一致経路なし) |
| `02` | T_RETURN_LIMIT_EXCEEDED | しない(codec は encode/decode のみ対応) |
| `03` | T_RETURN_NO_RESOURCES | しない(同上) |
| `04` | T_RETURN_PATH_ERROR | しない(同上) |
| `05` | T_RETURN_PROHIBITED | しない(同上) |
| `06` | T_RETURN_CONGESTED | しない(同上) |
| `07` | T_RETURN_MTU_TOO_LARGE | しない(同上) |
| `08` | T_RETURN_UNSUPPORTED_HASH_RESTRICTION | しない(restriction 未対応。§8) |
| `09` | T_RETURN_MALFORMED_INTEREST | しない(不正 8609 は黙って捨てる) |

「codec は全 ReturnCode を可逆に扱えるが、フォワーダは `01` しか生成しない」の 2 点を分けて主張する。
前者はベクタ試験(§1・§3)で機械判定し、後者は §6 の**未検証領域**(フォワーダ動作)に属する。
境界ベクタ(空名前 `/`・最大セグメント長・最大ペイロード・最大セグメント数)も同じ CLI 契約で追加する。

---

## 1. 命題A — バイト一致 encode(基準ベクタ)

### 手順
```sh
cd /Users/katouyoshiya/Documents/B-283-ccnx-swarm
client/codec_test vectors            > /tmp/c_vectors.txt
server/ccnxcli   vectors             > /tmp/go_vectors.txt
diff /tmp/c_vectors.txt /tmp/go_vectors.txt
```
### 合否
- **PASS:** `diff` が差分ゼロ(exit 0)。かつ各行の HEX が WIRE §6 の期待値に一致:
  - `/area/A-2` → `010000 1F 0800 0008 0001 0013 0000 000F 0001 0004 61726561 0001 0003 412D32`
    (連結: `01 00 00 1F 08 00 00 08 00 01 00 13 00 00 00 0F 00 01 00 04 61 72 65 61 00 01 00 03 41 2D 32`)
  - `/task/rescue/zone/A-2/carriers/2` → WIRE §6 ベクタ2(66 octet)
  - `/star/7/claim` + payload `robot-3` → WIRE §6 ベクタ3(49 octet, ContentObject)
- **FAIL:** 1 バイトでも相違。相違オフセットを報告(どちらが RFC 解釈を外したか WIRE.md で裁定)。

固定ヘッダの HopLimit(既定 8)・Reserved/Flags(0)・HeaderLength(8)まで完全一致すること。
Interest Lifetime はワイヤに載せない(WIRE §0)ので encode 出力に影響しない=両者一致の前提。

---

## 2. 命題B — 相互 decode(両方向)

### 2.1 C→Go(C が encode → Go が decode)
```sh
HEX=$(client/codec_test encode /task/rescue/zone/A-2/carriers/2 I)
server/ccnxcli decode "$HEX"
# 期待: Interest<TAB>/task/rescue/zone/A-2/carriers/2<TAB>(空)
```
### 2.2 Go→C(Go が encode → C が decode)
```sh
HEX=$(server/ccnxcli encode /star/7/claim C 726F626F742D33)   # payload "robot-3"
client/codec_test decode "$HEX"
# 期待: ContentObject<TAB>/star/7/claim<TAB>726F626F742D33
```
### 合否
- **PASS:** decode 側の `kind` / `uri` / `payloadHex` が encode 側の原本と一致(3 ベクタ × 2 方向 = 6 ケース全て)。
- **FAIL:** decode がエラー終了、または復元 uri/種別/payload が原本と不一致。

---

## 3. ラウンドトリップ(各実装 単独の自己証明 + 交差)

### 3.1 各実装 単独
```sh
client/codec_test selftest    # C: 内蔵ベクタ + ランダム1000件 encode→decode→再encode がバイト一致
go test ./server/ccnx         # Go: 同上(TestVectors + TestRoundTrip)
```
両者 exit 0 で PASS。境界ベクタとして次を各1件含める:
空名前(`/`)/ 最大セグメント長(256 バイト)/ 最大セグメント数(16)/ 最大ペイロード(1400 バイト)/
Interest Return(ReturnCode `01`〜`09` の各値。§0.1)。これらの上限値は C 実装の受理境界であり、
相互接続時の制限事項として §8-5 に明記してある。

### 3.2 交差ラウンドトリップ(統合スクリプト `docs/interop_check.sh` が実行)
1. C で N 個のランダム名を encode → Go で decode → Go で再 encode → **C encode と一致**するか。
2. Go で N 個を encode → C で decode → C で再 encode → **Go encode と一致**するか。
3. `vectors` の diff(命題A)+ 6 ケースの相互 decode(命題B)。

全ステップ PASS で「独立2実装が同一ワイヤに収束」を確定。1 つでも FAIL なら統合スクリプトは
exit 1 とし、相違ベクタ(名前 + C-HEX + Go-HEX + 相違オフセット)を出力する。

---

## 4. 合否サマリ(この表が最終判定)

| # | 試験 | 合格条件 | 落ちたら疑う所 |
|---|------|---------|--------------|
| A | `vectors` diff | 差分0 かつ WIRE §6 の3期待値に一致 | TLV Length のバイト順 / HeaderLength / 大小文字 |
| B1 | C→Go decode(3名) | kind/uri/payload 一致 | Name セグメント分割 / Payload TLV 有無 |
| B2 | Go→C decode(3名) | 同上 | 同上 |
| C1 | C selftest | exit 0, `PASS` | 境界(空名前/最大長)処理 |
| C2 | Go `go test ./ccnx` | exit 0, `ok` | 同上 |
| D | 交差 round-trip N件 | 再encode がバイト一致 | 未知TLVスキップ / 余剰バイト |

★本表 A〜D が保証するのは **codec(encode/decode)のバイト等価のみ**である。これに加え、
**Section E** が MUST/MUST-NOT の負検査(不正ワイヤの decode/encode 拒否)を、**Section F** が
RFC 8569 フォワーダの C↔Go 等価性(F1=表フラグ / F2=Portal 実送信を tap 観測)を機械判定する。
フォワーダ挙動の C↔Go 統一仕様は DESIGN §10、その機械判定の到達点と残件は §6.3。

デモ前チェックリスト(DESIGN §5 手順2-4)= 本表 A〜D を全て緑にしてから swarmd を起動する。
実行中のデモも DESIGN §2 の通り controller(Go encode)→robot(C decode)、robot(C encode)→observer(Go decode)を
連続実演しており、本表は「静的な物証」、動くデモは「動的な物証」として二重に相互運用を示す。

---

## 5. 判定の位置づけ(第三者検証者向け一文)

本試験が全緑であることは、C と Go が **互いのコードを一切共有せず**(別言語・別作者相当の独立実装)、
WIRE.md=RFC8609 という公開仕様のみを介してバイト単位で一致することを意味する。これは
「自作 CCNx が RFC に忠実である」ことの、実装非依存で再現可能な証明である。

★ただしこの主張は **RFC 8609(ワイヤ表現)に限定**される。RFC 8569(セマンティクス)の
フォワーダ動作等価性は A〜D の**保証範囲外**である。何が証明され何が証明されていないかを
§6 に明記する。逸脱と RFC 外拡張の一覧は DESIGN §8/§9、実機相互接続時の制限は §8 を見よ。

---

## 6. 本試験が保証する範囲・保証しない範囲(誠実な限界表明)

検証の観点上、**保証範囲を過大に述べないこと**が本節の目的である。

### 6.1 A〜D が保証するもの = codec のバイト等価のみ

| 判定 | 保証する命題 |
|------|------------|
| A | 同一の論理メッセージに対する **encode 出力バイト列**が C と Go で完全一致し、かつ WIRE §6 golden に一致する |
| B1/B2 | 一方の encode 出力を他方が decode でき、`kind`/`uri`/`payload`(+ InterestReturn は ReturnCode)が原本と一致する |
| C1/C2 | 各実装単独で encode→decode→再 encode がバイト不変であり、境界入力を落とさない |
| D | 交差 round-trip(相手が decode→再 encode)でバイトが不変である |

すなわち A〜D は **`ccnx_encode`/`ccnx_decode` ⇄ `ccnx.Encode`/`ccnx.Decode` の関数等価性**を
機械判定しているにすぎない。これは RFC 8609 への適合の物証であって、RFC 8569 への適合の物証ではない。

### 6.2 A〜D が保証**しない**もの(明示)

- **フォワーダ動作の等価性**: PIT 集約、CS ヒット応答、FIB 最長一致、HopLimit 減算、
  Interest Return の生成条件、CO の返送先集合、期限切れ掃除。これらは 1 バイトも **A〜D** には
  現れない。C↔Go の挙動は **DESIGN §10 の表**で規定され、**その一部の行のみ Section F で機械判定**
  される(§6.3。機械判定=行 2/6/8/9/10/11/12。行 1・3 は部分、行 5・7 はレビュー保証。
  **行 4「Interest Return 受信時の動作」は意図的に Go≠C の非対称**なので統一も等価検査もしない
  ——DESIGN §10 の被覆表を正とする)。網羅的 differential(ランダム trace 全一致)は未実装で、
  Section F は列挙された不変条件のみを固定する。
- **非請求 CO(announce)の扱い**: §7 の通り RFC 外のアプリ層拡張であり、ベクタ試験の対象外。
- **時間依存の挙動**: InterestLifetime / freshness / バックオフ。ワイヤに載らないため比較対象がない。
- **輻輳・再送・フラグメント**: v1 は実装しない(1 メッセージ = 1 UDP datagram)。
- **署名検証**: v1 は Validation 域を出さず、受信時も検証しない(§8, DESIGN §9)。

### 6.3 到達点: Section E/F(実装済み)と残る differential(未実装)

**実装済み(第3次是正):** DESIGN §10 の**一部の行**(行 2/6/8/9/10/11/12)を interop **Section F** で
機械判定する。**全 12 行ではない** — 行別の被覆は DESIGN §10 の被覆表を正とする(行 1・3 は部分、行 5・7 は
レビュー保証、行 4 は意図的に Go≠C の非対象)。

- **F1 表レベル** — PIT/FIB の判定フラグ(集約 / retransmit / larger HopLimit / NO_ROUTE 集合 /
  is_nexthop / 期限切れ PIT 不在)を C=`codec_test fwdtest`(`ccnx_tables.c` 直叩き)と
  Go=`portal_test.go` が同一不変条件で検査。
- **F2 Portal レベル(実送信+宛先)** — Portal がフラグに従い実際にワイヤへ出した datagram を
  **tap(本文・総数)に加え実 nexthop 面/到来面のソケットを直接読んで宛先まで**観測。
  C=`codec_test porttest`(`ccnx.c` を実 UDP で駆動)、Go=`TestPortalForwardSendBehavior`/
  `TestPortalPrevHopCO`。検査項目: 転送先=nexthop・HopLimit-1 / 別前ホップの集約抑止 /
  同一前ホップ再送・larger の MUST forward / HopLimit=1 無送出 / 唯一 nexthop==到来面の NO_ROUTE を
  到来面へ / 2 nexthop 転送で tap 2 複製(宛先ごと契約) / 前ホップ CO(trusted=CS格納+充足 overheard=0、
  untrusted=CS非格納だが overheard=1 配送)。
- **変異試験で感度確認済み**: 集約抑止・前ホップ信頼・**転送宛先(nexthop→到来元)**・**tap 複製数**の
  いずれの退行でも対応テストが赤(F1 は緑のまま F2 が赤)。「Portal がフラグに従い正しい face へ
  送ったか」まで実際に検査している。

**未実装(将来):** 網羅的 differential — 決定論的なランダム trace(時刻付き Interest/CO 送出列 +
固定 seed)を C 系・Go 系で再生し、tap の **datagram 列全体**と PIT/FIB/CS カウント遷移を
**各 tick で全一致比較**する fuzzing 差分。Section F は §10 の列挙不変条件を両側で固定するが、
列挙外の分岐を網羅はしない。これを埋めれば §10 は「列挙された事実」から「網羅的に機械判定された
事実」へ格上げされる。B層(ロボ挙動)は使わず A層 Portal のみを駆動する設計は上記のとおり。

---

## 7. 非請求 Content Object(announce)の試験上の扱い

本デモは以下の名前について、**Interest を伴わない Content Object の push**(以下 announce)を行う。
これは RFC 8569 の請求モデル(§2.4.4/§2.4.5)の**外側にあるアプリ層拡張**であり、
意図的な設計判断である(定義と根拠は DESIGN §8)。

`/star/{id}/claim` `/star/{id}/collected` `/star/{id}/deposited` `/robot/{id}/pose`
`/robot/{id}/repaired` `/task/rescue/robot/{cid}/carriers/{n}/arrived/{rid}`
`/task/rescue/robot/{cid}/carriers/{n}/pair` `/task/rescue/robot/{cid}/carriers/{n}/done`
`/bin/{team}/moved` `/bin/{team}/claim`

試験上の位置づけ:

| 論点 | 本試験での扱い |
|------|--------------|
| ワイヤ表現 | announce の datagram は**通常の ContentObject そのもの**(独自ヘッダ・独自 TLV は一切無い)。よって A〜D のベクタ試験がそのまま適用でき、Cefore の decoder でも読める |
| 転送挙動 | **非請求 CO を再転送はしない**(受信ノードは CS 格納とローカル配送で終端)。この点は RFC 8569 §2.4 に準拠する |
| 到達範囲 | フルメッシュ前提の **1 ホップ限定**。マルチホップ配送はしない(するなら別の設計が要る) |
| 受信側配送 | ローカル API(`on_content(overheard=1)` / `OnContent(..., overheard=true)`)でアプリへ渡す。これは**フォワーダ間のプロトコル挙動ではなくローカル拡張**であり、ワイヤには現れない |
| 判定対象か | announce の**発生条件・頻度・受信側の解釈**は A〜D の判定対象外(§6.2)。interop E の対象 |

★専門家読者向けの要点: 「announce をワイヤ上の新種別として発明していない」ことが重要である。
ワイヤは 8609 の ContentObject のままで、逸脱しているのは**いつ送るか**(請求なしで送る)だけである。

---

## 8. Cefore 実機(cefnetd)と相互接続する場合の制限事項

DESIGN §1/§7 の通り、ロボ 1 体の A層を実 cefnetd 裏の portal 実装に差し替える継ぎ目を用意している。
その際に**現状の v1 実装が満たさない項目**を、隠さず列挙する。いずれも「知らずに違反」ではなく
「v1 の範囲を絞った結果」であり、対処方針も併記する。

| # | 制限事項 | 具体的な影響 | 対処方針(v2) |
|---|---------|------------|--------------|
| 1 | **受信 T_INTLIFE を無視する** | cefnetd が InterestLifetime を載せた Interest を送っても、本実装は自局固定 4 秒で PIT を保持する。相手が想定するより長く/短く PIT が残る | hop-by-hop TLV のパーサを追加し、受信値を PIT 期限に採用する |
| 2 | **送信時に T_INTLIFE を載せない** | cefnetd 側は自局既定(RFC 8569 §2.2 の consumer 既定 2 秒相当)を仮定する。本実装のローカル 4 秒と食い違い、本実装がまだ待っているのに相手 PIT が消えている状況が起きる | 同上。載せる場合は WIRE §0(HeaderLength=8 固定)の全閉決定を解除する必要がある |
| 3 | **restriction 付き Interest に非対応** | KeyIdRestriction / ContentObjectHashRestriction を持つ Interest を受けても、**制約を無視して**名前一致だけで CS/PIT を扱う。RFC 上返すべき `08 T_RETURN_UNSUPPORTED_HASH_RESTRICTION` も返さない。誤った CO を返し得る | restriction TLV を decode し、(a) 制約を評価するか (b) `08` を返して明示的に拒否する |
| 4 | **署名付き CO を未検証で受理** | cefnetd から Validation 域(ValidationAlgorithm/ValidationPayload)付きの CO が来ても、本実装は未知 TLV としてスキップし、**検証せずに CS へ入れ・アプリへ配送する** | 検証を実装するまでは「署名付き CO は受理しない」方針に切り替えるのが安全 |
| 5 | **受理境界が実装固定値で小さい** | 名前 **16 セグメント** / 1 セグメント **256 バイト** / ペイロード **1400 バイト** / 1 datagram を超える入力は decode 失敗として黙って捨てる。cefnetd の実運用名は容易にこれを超える | 上限を RFC の 16 bit Length に合わせるか、超過時に `09`/`07` を返して可視化する |
| 6 | **Validation を出さない** | 本実装が出す CO は全て未署名。署名必須の相手からは全て破棄される | v1 の全閉決定(WIRE §0)。閉域デモ前提 |
| 7 | **フラグメント非対応** | 1 メッセージ = 1 UDP datagram。cefnetd 側の断片化された伝送とは接続できない | v1 範囲外 |
| 8 | **announce(非請求 CO)を送る** | cefnetd は請求のない CO を捨てるのが正しい。相互接続時、本実装の announce は**相手に届かない前提**で設計すること(§7) | 相互接続構成では announce を Interest/CO の請求形に置き換える |

★1〜5 は「相互接続すると壊れる/危険な箇所」、6〜8 は「相互接続しても静かに機能が落ちるだけの箇所」。
デモ構成(閉域 127.0.0.1・全ノードが本実装)ではいずれも顕在化しない。
