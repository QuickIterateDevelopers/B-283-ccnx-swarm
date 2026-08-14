# B-283 WIRE.md — 8609 ワイヤ形式 オクテット厳密仕様(単一の真実)

本書は C版 `libccnx.a` と Go版 `package ccnx` が**バイト一致**で従う唯一の規定である。
実装者はここに書かれた以外のバイトを出してはならない。曖昧が生じたら本書を正とする。
参照: RFC 8609 "CCNx Messages in TLV Format" / RFC 8569 "CCNx Semantics"。

全整数は**ネットワークバイトオーダ(ビッグエンディアン)**。TLV の Type と Length は各 **2 オクテット**。
本デモは 1 メッセージを 1 UDP datagram に収める(フラグメントなし)。

---

## 0. パケット全体構造(上位→下位)

```
+========================+  オフセット 0
| 固定ヘッダ (8 octet)    |
+========================+  オフセット 8 = HeaderLength
| hop-by-hop ヘッダ域     |  ★v1 では常に空(長さ 0)
+========================+  オフセット = HeaderLength
| CCNx Message TLV        |  Interest(0x0001) または ContentObject(0x0002)
|   +--------------------+ |
|   | Name TLV (0x0000)  | |
|   |   NameSegment...   | |
|   +--------------------+ |
|   | Payload TLV(0x0001)| |  optional
|   +--------------------+ |
+========================+  オフセット = PacketLength(パケット終端)
| Validation             |  ★v1 では出さない(未署名)
+========================+
```

**v1 の全閉決定(逸脱禁止):**
- **hop-by-hop ヘッダ域は常に空**。したがって常に **HeaderLength = 8**。
  Interest Lifetime(4s)はワイヤに載せず、受信側 PIT のローカル固定タイマで管理する。
  → 両実装が最小同一バイトを出すことを保証する。
- **Validation(署名)域は出さない**。PacketLength は Message TLV の終端に一致する。
- Type/Length は 2 octet BE 固定。Length は「その TLV の Value のオクテット数」(Type/Length 自身は含めない)。

---

## 1. 固定ヘッダ(8 octet, RFC 8609 §3.2)

### 1.1 Interest 固定ヘッダ(PacketType = 0x00)
```
 オフセット  0        1        2        3
           +--------+--------+--------+--------+
      0    |Version |  PT    |    PacketLength |
           +--------+--------+--------+--------+
      4    |HopLimit| Reserv |  Flags |HdrLen  |
           +--------+--------+--------+--------+
```
| オフセット | フィールド | 幅 | v1 の値 |
|---|---|---|---|
| 0 | Version      | 1 | **0x01**(必須) |
| 1 | PacketType   | 1 | **0x00** = PT_INTEREST |
| 2 | PacketLength | 2 | パケット総オクテット数(BE) |
| 4 | HopLimit     | 1 | 送出時既定 **0x08**。転送前に必ず 1 以上減算。**減算後 0 なら他フォワーダへ転送しない**(下記規則) |
| 5 | Reserved     | 1 | **0x00**(必須) |
| 6 | Flags        | 1 | **0x00**(定義済フラグ無し) |
| 7 | HeaderLength | 1 | **0x08**(固定ヘッダのみ、hop-by-hop 無し) |

**HopLimit 規則(RFC 8569 §2.4.1、全閉。ここは条文どおりに実装すること):**

| # | 条件 | 動作 |
|---|---|---|
| H1 | 全 Interest は HopLimit を必ず持つ | 送出時 0x08 を載せる |
| H2 | **リモートから受信した Interest の HopLimit が 0** | **破棄する**(または Interest Return を返す)。転送も PIT 登録もしない |
| H3 | ローカルアプリ由来の Interest は HopLimit=0 でもよい | ローカル発行元/ローカル CS のみに限定される |
| H4 | 転送前に **1 以上減算**しなければならない | `hop_limit -= 1` |
| H5 | **減算後の値が 0** | **他フォワーダへ転送してはならない**。ただし **ローカル発行元アプリへの配送**と **ローカル CS からの応答**は可 |

> 条文: "A forwarder MUST decrement the HopLimit of an Interest by at least 1 before it is forwarded" /
> "If the decremented HopLimit equals 0, the Interest MUST NOT be forwarded to another forwarder;
> it MAY be sent to a local publisher application or serviced from a local Content Store."(RFC 8569 §2.4.1)
>
> **旧記述「0 なら転送しない」は誤りである。** それは「減算前に 0 を見る」= 条文より 1 ホップ緩い判定で、
> HopLimit=1 の Interest をもう 1 ホップ転送してしまう。判定は必ず**減算後の値**に対して行う。
> 本デモは全ピア 1 ホップのフルメッシュのため既定 0x08 では H5 に到達しないが、
> 契約としては上表が正であり、C/Go 双方がこれに従う。

### 1.2 Content Object 固定ヘッダ(PacketType = 0x01)
```
 オフセット  0        1        2        3
           +--------+--------+--------+--------+
      0    |Version |  PT    |    PacketLength |
           +--------+--------+--------+--------+
      4    |    Reserved     |  Flags |HdrLen  |
           +--------+--------+--------+--------+
```
| オフセット | フィールド | 幅 | v1 の値 |
|---|---|---|---|
| 0 | Version      | 1 | **0x01** |
| 1 | PacketType   | 1 | **0x01** = PT_CONTENT |
| 2 | PacketLength | 2 | パケット総オクテット数(BE) |
| 4 | Reserved     | 2 | **0x0000** |
| 6 | Flags        | 1 | **0x00** |
| 7 | HeaderLength | 1 | **0x08** |

### 1.3 Interest Return 固定ヘッダ(PacketType = 0x02)
Interest と同一レイアウト。相違は PacketType=0x02 とオフセット5(Interest では Reserved)に
**ReturnCode** を置く点のみ(RFC 8609 §3.2.3 "Interest Return Fixed Header")。
PacketLength は元 Interest と不変(**バイト長を変えない**。ヘッダ 2 バイトの書き換えのみで返す)。

#### 1.3.1 ReturnCode 一覧(RFC 8609 §3.2.3.3 Table 2 / IANA レジストリ §4.2)

条文の全 9 コード。値は RFC 8609 本文から転記した確定値であり、**推測で埋めてはならない**。

| 値 | 定数名(RFC 8609) | 意味 | v1 実装 |
|---|---|---|---|
| 0x01 | `T_RETURN_NO_ROUTE` | No Route — 返送側フォワーダが当該 Interest 名への経路を持たない | **生成する** |
| 0x02 | `T_RETURN_LIMIT_EXCEEDED` | Hop Limit Exceeded — HopLimit が 0 に達し、なお転送が必要だった | 生成しない |
| 0x03 | `T_RETURN_NO_RESOURCES` | No Resources — 資源不足で処理できない | 生成しない |
| 0x04 | `T_RETURN_PATH_ERROR` | Path Error — 経路上の誤り | 生成しない |
| 0x05 | `T_RETURN_PROHIBITED` | Prohibited — 管理ポリシにより禁止 | 生成しない |
| 0x06 | `T_RETURN_CONGESTED` | Congested — 輻輳により処理できない | 生成しない |
| 0x07 | `T_RETURN_MTU_TOO_LARGE` | Interest MTU too large — Interest が出力リンク MTU を超える | 生成しない |
| 0x08 | `T_RETURN_UNSUPPORTED_HASH_RESTRICTION` | ContentObjectHashRestriction のハッシュ方式が未対応 | 生成しない(§8 参照) |
| 0x09 | `T_RETURN_MALFORMED_INTEREST` | Malformed Interest — 文法違反の Interest | 生成しない |

**実装状況(契約):**
- **codec 層は 0x01〜0x09 の全コードを透過的に encode / decode できること。**
  encode はメッセージ構造体の ReturnCode をそのままオフセット5 に載せ、
  decode はオフセット5 の値を検査せずそのまま取り出す(未知値でも decode 失敗にしない)。
  → 実 cefnetd など他実装から届く Return を落とさないため。
- **フォワーダが自発的に生成するのは `0x01 = T_RETURN_NO_ROUTE` のみ。**
  それ以外のコードは v1 では生成経路を持たない(受信側として解釈できればよい)。
- 0x00 および 0x0A 以上は本仕様で意味を割り当てない。生成してはならない。

---

## 2. CCNx Message TLV

固定ヘッダ(+空 hop-by-hop)の直後に 1 個だけ置く。
```
+--------+--------+--------+--------+
|      Type       |     Length      |   Type: 0x0001=Interest / 0x0002=ContentObject
+--------+--------+--------+--------+   Length: 以降 Value のオクテット数
|                Value              |   Value = Name TLV [+ Payload TLV]
+-----------------------------------+
```
| 定数 | 値 |
|---|---|
| T_INTEREST | 0x0001 |
| T_OBJECT   | 0x0002 |

Value の並び(この順序固定):
1. **Name TLV**(必須, 1 個)
2. **Payload TLV**(任意, 0〜1 個)。Interest では選抜バックオフ等の小データ、ContentObject では本体。

---

## 3. Name TLV(RFC 8609 §3.6)

```
+--------+--------+--------+--------+
|   T_NAME=0x0000 |     Length      |   Length: 全 NameSegment の合計オクテット数
+--------+--------+--------+--------+
|         NameSegment ...           |
+-----------------------------------+
```
`/a/b/c` は Name TLV の Value に NameSegment を左から順に連結して表す。

### 3.1 NameSegment(RFC 8609 §3.6.1)
```
+--------+--------+--------+--------+========+
| T_NAMESEGMENT=0x0001 |  Length   | Value  |   Value = セグメント文字列の生バイト(UTF-8/ASCII、NUL 終端なし)
+--------+--------+--------+--------+========+
```
| 定数 | 値 |
|---|---|
| T_NAME        | 0x0000 |
| T_NAMESEGMENT | 0x0001 |
| T_IPID        | 0x0002 |
| T_APP:00〜T_APP:4095 | 0x1000〜0x1FFF |

**セグメントは「型 + 値」の対である。名前の同一性判定は型と値の両方を突き合わせる。**
本デモが生成する名前は全て T_NAMESEGMENT(0x0001)だが、**decode では全ての型を保持する**。
T_IPID や T_APP は未知型ではなく §3.6.1 の定義済み型であり、これを捨てて値だけ残すと
`/0x1000=app` と `/app` が同一名に潰れて FIB / PIT / CS の鍵が壊れる(監査指摘)。
未知型のセグメントも同様に型ごと保持する(捨てない)。

**例外 — T_PAD(0x0FFE):** RFC 8609 §3.3.1 の Pad TLV は **Name の中に置いてはならない**。
Name 内に T_PAD 型のセグメントを含む名前は **encode / decode とも拒否**する
(§7.2 E8 / §7.3 D14。第3次是正で追加)。

**URI ↔ Name 変換規則(全閉。C `ccnx_name_from_uri`/`ccnx_name_to_uri` と Go `NameFromURI`/`URI()` が同一):**
- 入力 URI は `/seg0/seg1/...` 形式。先頭 `/` は必須、末尾 `/` は付けない。
- 各セグメントは空にできない(`//` 禁止)。セグメントの生バイトをそのまま Value にする。
- **型が T_NAMESEGMENT(0x0001)のセグメント**: 前置なしでそのまま。例 `/area/A-2`
- **それ以外の型のセグメント**: `0xHHHH=` を前置(H は大文字16進 4 桁)。例 `/0x1000=app` `/0x0002=ipid`
  - 解析規則: セグメント長 ≥ 7 かつ `s[0]=='0' && s[1]=='x' && s[6]=='='` かつ `s[2..5]` が 16 進のときのみ
    型付きとみなす。条件を満たさなければ**生の値**として扱う(例 `/0xZZZZ=x` は値そのもの)。
  - 型付きで値が空(`/0x1000=`)は不正(URI 解析エラー)。
- 本デモの名前は ASCII 英数・`-`・数字のみを用いる(パーセントエンコード不要)。
  したがって生の値が `0xHHHH=` の形になることはなく、上記規則に曖昧さは生じない。
- 空名前 `/`(seg_count=0)の扱いは **§3.2 を正とする**。

### 3.2 空名前 `/`(seg_count = 0)の扱い — **FIB / プレフィックス登録専用**

**空名前は Interest / ContentObject の Name として encode してはならない。**

RFC 8609 §3.6.1 の文法は **CCNx Message TLV の Name において最初の NameSegment が長さ 0 になることを許さない**
("the message grammar does not allow the first name segment to have zero length in a CCNx Message TLV Name")。
したがってワイヤ上の Interest / CO に空名前を載せることは **8609 非適合**である。

一方、**FIB のデフォルトルート**としての `ccnx:/` はワイヤに載らない**ローカルなテーブル表現**であり、
RFC 8569 §3.1 の最長プレフィックス一致(セグメント単位)において、
0 セグメントのプレフィックスが全ての名前に一致する = デフォルトルートになる。本デモの
「全ピアへ `/` を張るフルメッシュ FIB」はこの**ローカル表現**の用法であって、ワイヤ形式ではない。

| 用途 | 空名前 `/` | 根拠 |
|---|---|---|
| FIB エントリのプレフィックス(デフォルトルート) | **可**(ローカル表現。ワイヤに出ない) | RFC 8569 §3.1 |
| アプリのプレフィックス登録(portal の register) | **可**(ローカル表現。ワイヤに出ない) | 同上 |
| Interest の Name として encode | **不可**(encode 拒否。§7 参照) | RFC 8609 §3.6.1 |
| ContentObject の Name として encode | **不可**(encode 拒否。§7.2 E2 を CO にも適用) | RFC 8609 §3.6.1 |

> **削除された旧記述:**「空名前は Name TLV Length=0 として符号化(RFC の "ccnx:/" 相当)」。
> これは「ccnx:/ 相当だからワイヤに載せても適法」という誤った適法主張だった。
> RFC 8609 §3.6.1 は "a T_NAME with zero length corresponds to ccnx:/" を **FIB/デフォルトルートの文脈**で
> 述べたうえで、**メッセージ文法としては禁じている**。両者を混同してはならない。
>
> **decode 側は現状維持。** 空 Name TLV を含む datagram を受けた場合の扱いは変更しない
> (相手実装との受理集合を非対称にしないため)。禁止するのは **生成(encode)側**である。

---

## 4. Payload TLV(RFC 8609 §3.6.2)

```
+--------+--------+--------+--------+========+
| T_PAYLOAD=0x0001 |    Length     | Value  |   Value = 生ペイロードバイト
+--------+--------+--------+--------+========+
```
| 定数 | 値 |
|---|---|
| T_PAYLOAD | 0x0001 |

本デモのペイロードは PROTOCOL.md のコンパクト JSON(例 `{"id":3,"d":4.2}`)を UTF-8 で入れる。
無ペイロード時は Payload TLV 自体を出さない(空 TLV を出さない)。

---

## 5. エンコード手順(両実装で同一の算法)

1. Name TLV を組む: 各セグメントを `00 01 | len(2) | bytes` で連結 → `00 00 | Σlen(2) | segs`。
2. Payload があれば `00 01 | plen(2) | payload` を作る。
3. Message Value = NameTLV [+ PayloadTLV]。Message TLV = `T(2) | len(Value)(2) | Value`
   (T=0x0001 Interest / 0x0002 Object)。
4. PacketLength = 8 + len(Message TLV)。HeaderLength = 8。
5. 固定ヘッダ(§1)を先頭に置き、Message TLV を続ける。
6. Length 溢れ(>0xFFFF)や UDP 上限超過は encode 失敗(負値)。

デコードは逆順。各 TLV は `Type(2)|Length(2)` を読み、Length ぶんを Value として消費。
未知 Type の TLV は**スキップ**(Length ぶん読み飛ばす)して前方互換を保つ。ただし
トップレベルでは **最初の TLV が CCNx Message TLV でなければならず**(RFC 8609 §3.1)、
スキップ対象は Message より後ろの TLV に限る(§7.3 D12)。
Length が残バイトを超える/PacketLength と不整合なら decode 失敗。

> **境界値と拒否条件の完全な一覧は §7 を正とする。** 本節は算法の説明であり、
> 数値・拒否条件が §7 と食い違う場合は §7 が勝つ。

---

## 6. テストベクタ(既知名 → 期待16進。C/Go 双方がこれに一致すること)

各バイト列は「1 UDP datagram のペイロード全体」。空白は可読性のためで、実バイトには含まれない。

### ベクタ 1 — Interest `/area/A-2`(HopLimit=8, ペイロード無)
全 **31 (0x1F)** octet:
```
01 00 00 1F 08 00 00 08  00 01 00 13  00 00 00 0F
00 01 00 04 61 72 65 61  00 01 00 03 41 2D 32
```
内訳:
- `01 00 00 1F 08 00 00 08` 固定ヘッダ(PT=Interest, PacketLength=0x1F=31, HopLimit=8, HdrLen=8)
- `00 01 00 13` Message TLV(T_INTEREST, Value 長 0x13=19)
- `00 00 00 0F` Name TLV(Value 長 0x0F=15)
- `00 01 00 04 61 72 65 61` NameSegment "area"
- `00 01 00 03 41 2D 32` NameSegment "A-2"

### ベクタ 2 — Interest `/task/rescue/zone/A-2/carriers/2`(HopLimit=8, ペイロード無)
全 **66 (0x42)** octet:
```
01 00 00 42 08 00 00 08  00 01 00 36  00 00 00 32
00 01 00 04 74 61 73 6B  00 01 00 06 72 65 73 63 75 65
00 01 00 04 7A 6F 6E 65  00 01 00 03 41 2D 32
00 01 00 08 63 61 72 72 69 65 72 73  00 01 00 01 32
```
- Message TLV Value 長 0x36=54、Name TLV Value 長 0x32=50。
- セグメント: task(74 61 73 6B) / rescue(72 65 73 63 75 65) / zone(7A 6F 6E 65) /
  A-2(41 2D 32) / carriers(63 61 72 72 69 65 72 73) / 2(32)。

### ベクタ 3 — ContentObject `/star/7/claim` ペイロード `robot-3`(ASCII 7 octet)
全 **49 (0x31)** octet:
```
01 01 00 31 00 00 00 08  00 02 00 25  00 00 00 16
00 01 00 04 73 74 61 72  00 01 00 01 37  00 01 00 05 63 6C 61 69 6D
00 01 00 07 72 6F 62 6F 74 2D 33
```
内訳:
- `01 01 00 31 00 00 00 08` 固定ヘッダ(PT=ContentObject, PacketLength=0x31=49, HdrLen=8)
- `00 02 00 25` Message TLV(T_OBJECT, Value 長 0x25=37)
- `00 00 00 16` Name TLV(Value 長 0x16=22): star(73 74 61 72)/7(37)/claim(63 6C 61 69 6D)
- `00 01 00 07 72 6F 62 6F 74 2D 33` Payload TLV "robot-3"

これら 3 本は INTEROP.md の「バイト一致」判定の基準ベクタである。
C の `ccnx_test`、Go の `TestVectors` は同一の期待16進を内蔵し、両者が一致すれば相互運用の第一段合格。

---

## 7. 受理・生成境界(適合契約)

**本節は C 版 `libccnx.a` と Go 版 `package ccnx` の両方が従う契約である。**
監査 W-2「C と Go の受理集合が非対称」への対処として、**両実装の受理集合(decode で成功する
バイト列の集合)と生成集合(encode で出力しうるバイト列の集合)を本節で完全に一致させる**。
一方だけが受け取れる/一方だけが出せるバイト列が存在してはならない。

### 7.1 固定境界値(両実装で同一の定数)

| 項目 | 上限 | 超過時 | 備考 |
|---|---|---|---|
| Name のセグメント数 | **16** | encode / decode とも失敗 | `MAX_NAME_SEGS` |
| 1 セグメントの Value バイト長 | **256** | encode / decode とも失敗 | `MAX_SEG_LEN`。TLV ヘッダ 4 octet は含まない |
| Payload の Value バイト長 | **1400** | encode / decode とも失敗 | `MAX_PAYLOAD`。TLV ヘッダ 4 octet は含まない |
| UDP datagram 全長 | **1500** | encode / decode とも失敗 | `MAX_DATAGRAM` = PacketLength の上限 |
| PacketLength / 各 TLV Length | 0xFFFF | encode 失敗 | 2 octet 幅の物理上限(実効上限は上の 1500) |

これらは **RFC 8609 が定める値ではなく、本デモが相互運用のために固定した実装プロファイル**である。
値を変えると C/Go の受理集合が非対称になるため、**片方だけを変更してはならない**。
1500 は Ethernet MTU に合わせた「1 メッセージ = 1 UDP datagram、フラグメントなし」(§0)の帰結。

### 7.2 encode 拒否条件(以下のいずれかに該当したら出力せず失敗を返す)

| # | 条件 | 根拠 |
|---|---|---|
| E1 | PacketType が `0x00` / `0x01` / `0x02` 以外 | §1。RFC 8609 §3.2 の定義済 PacketType のみ |
| E2 | **セグメント数 0(空名前)。全 PacketType に適用** | RFC 8609 §3.6.1(§3.2)。空名前は FIB 登録専用 |
| E3 | セグメント数 > 16 | §7.1 |
| E4 | いずれかのセグメント長 > 256、または **セグメント長 0**(`//` 禁止) | §7.1 / §3 |
| E5 | Payload 長 > 1400 | §7.1 |
| E6 | 算出した PacketLength > 1500(または > 0xFFFF) | §7.1 |
| E7 | 出力バッファ容量が PacketLength に満たない | 実装保護 |
| E8 | **Name 内に T_PAD(0x0FFE)型のセグメント** | RFC 8609 §3.3.1(Pad は Name に置けない)。§3.1 |

> **ContentObject(PT=0x01)の空名前について(第3次是正で E2 に統合):**
> RFC 8609 §3.6.1 上 Interest 同様に不正であり、**E2 は ContentObject にも適用する**(encode 拒否)。
> 長さ 0 の T_NAME を持つ CO は「Name を省略した nameless CO」**ではない**
> (nameless は Name TLV 自体を出さない。§8-10 のとおり v1 は生成しない)。
> 旧記述「v1 は空名前 CO の生成経路を持たないため明示チェックを追加しない」は、
> 生成経路(encode API 直叩き)が実在し文書とテストが矛盾していたため撤回した。
> decode 側は §7.4 A5 のとおり現状維持で受理する(相手実装との受理集合を狭めない)。

### 7.3 decode 拒否条件(以下のいずれかに該当したら失敗を返す)

| # | 条件 | 根拠 |
|---|---|---|
| D1 | Version != 0x01 | §1 |
| D2 | PacketType が `0x00` / `0x01` / `0x02` 以外 | §1 |
| D3 | datagram 長 < 8(固定ヘッダ未満) | §1 |
| D4 | datagram 長 > 1500 | §7.1 |
| D5 | `HeaderLength < 8` または `HeaderLength > PacketLength` | §1 |
| D6 | `PacketLength < HeaderLength` または `PacketLength > 受信バイト数` | §5 |
| D7 | ある TLV の `Length` が、その TLV が属する領域の**残バイトを超える** | §5 |
| D8 | **末尾余剰バイト**(`PacketLength < 受信 datagram 長`)= PacketLength 不整合 | 1 datagram = 1 メッセージ(§0) |
| D9 | CCNx Message TLV(0x0001 / 0x0002)が存在しない | §2 |
| D10 | Message Value 内に **Name TLV(0x0000)が存在しない** | §2 |
| D11 | セグメント数 > 16 / セグメント長 > 256 / Payload 長 > 1400 | §7.1 |
| D12 | **最初のトップレベル TLV が CCNx Message TLV でない**(Message より前に Validation 等の他 TLV がある並び) | RFC 8609 §3.1(PacketPayload は CCNx Message から始まる)。正当な Validation は Message の**後ろ**に来る(A1 でスキップ) |
| D13 | **固定ヘッダ PacketType と Message TLV 型の不整合**(PT_INTEREST / PT_RETURN ⇔ T_INTEREST(0x0001)、PT_CONTENT ⇔ T_OBJECT(0x0002)) | RFC 8609 §3.2.3(Interest Return は元 Interest の PacketType / ReturnCode のみ書換え = 本文は T_INTEREST) |
| D14 | **Name 内に T_PAD(0x0FFE)型の NameSegment** | RFC 8609 §3.3.1。§3.1 / §7.2 E8 と対 |

### 7.4 decode が許容するもの(前方互換方針。**厳格化してはならない**)

| # | 事象 | 扱い | 理由 |
|---|---|---|---|
| A1 | **未知 Type の TLV**(Message Value 内、および**トップレベルでは Message TLV より後**。Validation 等) | `Length` ぶん読み飛ばして**スキップ**し、処理を続行 | RFC 8609 は将来型の追加を前提とする。実 cefnetd 等との相互運用に必須。ただし Message より**前**のトップレベル TLV は D12 のとおり拒否(RFC 8609 §3.1) |
| A2 | hop-by-hop ヘッダ域が非空(`HeaderLength > 8`) | Message TLV の開始位置を `HeaderLength` から取る(A1 と同様に内容は読み飛ばす) | 生成はしない(§0)が、受信はできる |
| A3 | Message Value 内の **Name / Payload の出現順序** | **現状どおり寛容**(順序を強制しない) | 既存の受理集合を狭めないため。**変更禁止** |
| A4 | Name / Payload の **多重度**(2 個以上出現する等) | **現状どおり寛容** | 同上。**変更禁止** |
| A5 | **空 Name TLV**(Length=0)を含む datagram | **現状維持**(encode では禁ずるが decode の挙動は変えない) | §3.2。相手実装との受理集合を非対称にしないため |
| A6 | Interest Return の ReturnCode が 0x01 以外 / 未割当値 | 値を検査せずそのまま取り出す | §1.3.1 |

**原則:** 生成側は狭く(§7.2)、受理側は広く(§7.4)。ただし §7.1 の境界値と §7.3 の拒否条件は
**C と Go で 1 条件も違わないこと**。差分が出たら本節を正として実装側を直す。

---

## 8. 意図的に使っていない RFC 8609 機能(v1 スコープ外)

以下は **RFC 8609 上 optional であり、使わなくても 8609 適合**である。v1 は
「両実装が最小同一バイトを出す」ことを最優先し、意図的に不使用とする。
将来使う場合も §7.4 A1(未知 TLV スキップ)により、既存実装が壊れることはない。

| # | 機能 | 型/位置 | 不使用でよい条文根拠 | v1 の扱い |
|---|---|---|---|---|
| 8-1 | **Validation(署名)域** | ValidationAlgorithm / ValidationPayload(RFC 8609 §3.6.4) | §3.6 が "Both Interests and Content Objects have the **option** to include information about how to validate the CCNx Message" と明示。パケット構造上も末尾の任意領域 | **出さない**。PacketLength は Message TLV 終端に一致(§0)。受信時は A1 でスキップ |
| 8-2 | **フラグメンテーション** | hop-by-hop ヘッダ(RFC 8609 §3.4 が "additional hop-by-hop headers are defined in higher level specifications such as the **fragmentation specification**" として 8609 本体の外に置く) | 8609 本体に規定が無い = 実装必須ではない | **使わない**。1 メッセージ = 1 UDP datagram、上限 1500(§7.1) |
| 8-3 | **hop-by-hop ヘッダ全般** | RFC 8609 §3.4。図 2 で "**Optional** hop-by-hop header TLVs" | 領域ごと省略可能(HeaderLength=8) | **常に空**。HeaderLength = 8 固定(§0) |
| 8-4 | └ **Interest Lifetime** | `T_INTLIFE` = 0x0001(RFC 8609 §3.4.1) | 8-3 の optional 領域内にあるため省略可 | ワイヤに載せず、受信側 PIT の**ローカル固定タイマ(4s)**で管理(§0) |
| 8-5 | └ **Recommended Cache Time** | `T_CACHETIME` = 0x0002(RFC 8609 §3.4.2) | 同上 | 使わない。CS は表示用の単純キャッシュ |
| 8-6 | └ Message Hash | `T_MSGHASH` = 0x0003(RFC 8609 §3.4.3) | 同上 | 使わない |
| 8-7 | **KeyIdRestriction** | `T_KEYIDRESTR` = 0x0002(RFC 8609 §3.6.2.1.1)。Interest の optional restriction | Interest の任意フィールド。無ければ「鍵による絞り込み無し」 | 出さない。受信時は A1 でスキップ |
| 8-8 | **ContentObjectHashRestriction** | `T_OBJHASHRESTR` = 0x0003(RFC 8609 §3.6.2.1.2)。Interest の optional restriction | 同上 | 出さない。したがって ReturnCode `0x08`(未対応ハッシュ)を生成する経路も無い(§1.3.1) |
| 8-9 | **ExpiryTime** | `T_EXPIRY` = 0x0006(RFC 8609 §3.6.2.2.2)。§3.6.2.2 が "**Optional** ExpiryTime TLV" と明示 | 明示的に optional | 出さない。CO の寿命はデモ時間内で管理しない |
| 8-10 | **nameless ContentObject** | RFC 8609 §3.6 "The first enclosed TLV of a CCNx Message TLV is always the Name TLV, **if present**" | Name 省略は許されるが、その CO は ContentObjectHashRestriction 付き Interest としか一致できない。8-8 を使わない以上、使う意味が無い | **生成しない**。CO は必ず Name TLV を持つ(§7.2 / §7.3 D10) |
| 8-11 | **汎用 NameSegment 以外のセグメント型** | `T_IPID` = 0x0002、アプリ定義 `T_APP:00`〜`T_APP:4096` = 0x1000〜0x1FFF(RFC 8609 §3.6.1) | いずれも用途特化の任意型。汎用 `T_NAMESEGMENT` = 0x0001 のみで名前は完全に表現できる | **`T_NAMESEGMENT`(0x0001)のみ**を生成(§3.1) |

**この一覧の意味:** 上表の機能が出てこないことは「未実装の欠落」ではなく **v1 の明示的なスコープ決定**である。
INTEROP のバイト一致判定(§6)は、この最小集合の上で成立している。
将来これらを足す場合は、**§7.1 の境界値と §7.3 の拒否条件を先に本書で改定**してから、
C/Go を同時に追随させること(片側だけの追加は受理集合の非対称=監査 W-2 の再発になる)。
