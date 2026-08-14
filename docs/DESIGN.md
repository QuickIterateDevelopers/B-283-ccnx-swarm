# B-283 DESIGN — CCNx セマンティック群ロボ通信デモ アーキテクチャ確定書

本書は単一の真実(契約)である。以降の全実装者はこれに厳密に従う。曖昧が生じたら
本書 → WIRE.md → PROTOCOL.md → INTEROP.md の順を正とし、実装者が独自解釈しない。

想定読者: ICN/CCNx 実装の専門家(cefnetd 等のフォワーダ実装者を想定)。
証明対象: 自作プロトコルが RFC 8569/8609 準拠で「実際に」分散動作し、**独立2実装(C/Go)が
バイト一致で相互運用する**こと。演出ではなく物証(Wireshark に映る純 8609 UDP + 公開 C コード
+ C/Go 相互デコード成功)で示す。

---

## 1. 層分離(最重要・A層とB層を混ぜるな)

| 層 | 責務 | ドメイン依存 | C の実体 | Go の実体 |
|----|------|------|------|------|
| **A層** | 8609 ワイヤコーデック / フォワーダ(PIT・FIB・CS, Interest 集約, HopLimit, Interest Return, CO マッチング, キャッシュ) / フェイス(UDP I/O) / Portal API | **無**(ロボも救助も知らない) | `libccnx.a` | `package ccnx` |
| **B層** | 挙動。A層 API を呼ぶだけでワイヤ形式は知らない | 有 | `robot.c`(ロボ挙動) | controller / observer(救助 Interest 組立・送出、ワイヤ観測) |

**A層 API(Portal, `client/ccnx.h`)= Cefore の portal/face API に寄せた粒度:**
`ccnx_portal_open` / `ccnx_register_prefix` / `ccnx_fib_add_route` /
`ccnx_express_interest` / `on_interest` / `ccnx_publish` / `on_content` / `ccnx_tick` / `ccnx_run`。
Go `package ccnx` は同義の関数群を Go 慣用で提供する(§6)。

★**A層の正しさの証明 = C版 libccnx.a と Go版 package ccnx がワイヤ互換(バイト一致)であること。**
両者は WIRE.md の 1 文書に従う。相互運用が成立すれば「独自実装が RFC に忠実」の物証になる
(INTEROP.md)。この一点のために Go も 8609 を**実装する**(旧版の「Go は 8609 を実装しない」は破棄)。

各ロボは「フォワーダ内蔵の自律 CCNx ノード」= libccnx が PIT/FIB/CS を持つフォワーダまで内包し
robot.c がその上のアプリ。実装範囲は RFC 8569/8609 の限定サブセット(§9 参照)。
**Cefore 差し替え点:** ロボ 1 体だけ A層を実 cefnetd 裏の portal API 実装に差し替え可能な継ぎ目にする。
そのため 8609 チャネルは素の UDP + RFC8609 準拠バイトに限定し独自ヘッダを禁止する(WIRE §0)。

---

## 2. プロセス構成(確定)

- **Go 管理プロセス `swarmd`(シングルバイナリ・シングルプロセス, 内部 goroutine):**
  群れの一員ではなく管理ノード1個なので goroutine で可。内訳:
  - `net/http`: 単一ページ UI を `:8080` で配信(Go embed で内包)。
  - WS ハブ: browser と双方向(PROTOCOL §3)。
  - スーパーバイザ: `os/exec` で Cロボ ×N(既定 N=8)を起動。SIGTERM で故障注入。stdout の状態 JSON を集約。
  - **コントローラ(B層, `package ccnx` を使用):** 救助 Interest を組み立て・送出。
  - **ワイヤ観測(B層):** LAN の 8609 を tap ポートで受け `package ccnx` で decode し packet-event を browser へ。
- **Cロボ ×N(プロセス, 既定 N=8):** `libccnx.a`(A層) + `robot.c`(B層)をリンク。
  各自 UDP ソケットで LAN に実 8609 を流す(Wireshark 可視)。各自 状態 JSON を stdout に emit → Go が中継。

```
                    ┌───────────────────────────────────────────────┐
                    │  Browser (受像機に徹する / 3ペイン)             │
                    │  (1)指示コンソール (2)応答表示 (3)Canvas2D動作画面│
                    └──────────────▲───────────────┬────────────────┘
        state/topology/            │ WebSocket     │ task-command
        packet-event/log-event     │               │ fault-inject / scenario
                    ┌──────────────┴───────────────▼────────────────┐
                    │  Go swarmd (シングルプロセス / goroutine 群)    │
                    │  http :8080 │ WS hub │ supervisor              │
                    │  controller(package ccnx A層) │ wire-observer  │
                    └───▲──────────────▲──────────────┬─────────────┘
   stdout 改行JSON       │              │ stdout       │ SIGTERM(故障)
   (状態チャネル)         │              │ 改行JSON      │
              ┌──────────┴──┐     ┌─────┴────┐    ┌────▼──────────────┐
              │ robot 0      │ ... │ robot N-1 │   │(controller は Go 内 │
              │ libccnx.a A層│     │           │   │ goroutine。C 別proc │
              │ robot.c   B層│     │           │   │ ではない)           │
              │ UDP :9200+i  │     │ UDP:9200+i│   └────────────────────┘
              └──────┬───────┘     └────┬──────┘
                     │  実 8609 Interest / ContentObject(UDP)      │
                     ├─────────── LAN(127.0.0.1 マルチポート)──────┤
                     └─── 各 datagram の複製 → tap :9300 → observer ┘
                          (Wireshark: lo0 udp portrange 9200-9300)
```

2 チャネルは物理分離する:
- **状態チャネル**: C stdout → Go(改行 JSON)。制御 Go → C(SIGTERM)。ここに 8609 は流れない。
- **8609 チャネル**: ロボ間・コントローラ→ロボ の UDP。ここには 8609 TLV バイトしか流れない。
  Wireshark をこのポート域に張れば純 8609 だけが観測される(状態 JSON の混入なし)。

**ライブ相互運用(デモ実行そのものが INTEROP の連続実演):**
- Go controller が `package ccnx` で **encode** した Interest → Cロボが `libccnx` で **decode**(Go→C)。
- Cロボが `libccnx` で **encode** した Interest/CO → tap :9300 → Go observer が `package ccnx` で **decode**(C→Go)。
両方向が実行中ずっと成立する。バイト一致でなければ decode は失敗するので、動くデモ=相互運用の証明。

---

## 3. ディレクトリレイアウト(確定)

```
B-283-ccnx-swarm/
├── docs/
│   ├── DESIGN.md          ← 本書(アーキ確定)
│   ├── WIRE.md            ← 8609 バイト配置 オクテット厳密 + テストベクタ
│   ├── PROTOCOL.md        ← 名前空間 / 状態JSON / WS / IPC の厳密仕様
│   └── INTEROP.md         ← C↔Go バイト一致・相互decode 試験手順と合否
├── client/                ← C: 納品対象 CCNx 実装(A層 libccnx + B層 robot)
│   ├── ccnx.h             ← A層 単一公開ヘッダ(Portal API + codec.h 取込)【確定】
│   ├── codec.h            ← 8609 コーデック契約(ccnx_name_t/ccnx_msg_t)【確定】
│   ├── codec.c            ← codec.h の実装(WIRE.md 準拠, ベクタ自己証明)
│   ├── ccnx_tables.h/.c   ← PIT/FIB/CS の型と操作(ccnx_name_t 鍵。UDP を知らない)
│   ├── transport.h/.c     ← UDP 送受信 + tap 複製(名前の意味を知らない)
│   ├── ccnx.c             ← Portal core(codec+tables+transport を束ね ccnx.h を実装)
│   ├── robot.c            ← B層: ロボ挙動 main → 実行ファイル swarm_robot
│   ├── ccnx_test.c        ← WIRE ベクタ一致 + round-trip 自己証明 → codec_test
│   └── Makefile
├── server/                ← Go: 操縦席 + 第2実装(A層 package ccnx)
│   ├── ccnx/              ← package ccnx(A層。C libccnx と同一ワイヤ)
│   │   ├── codec.go       ← 8609 encode/decode(WIRE.md 準拠)
│   │   ├── portal.go      ← Portal API(register/express/publish/tick 相当)
│   │   ├── tables.go      ← PIT/FIB/CS
│   │   └── codec_test.go  ← TestVectors(WIRE と同一期待16進)+ round-trip
│   ├── main.go            ← http + WS ハブ + supervisor 起動
│   ├── supervisor.go      ← os/exec による Cロボ群の起動/監視/故障注入
│   ├── hub.go             ← WS ハブ + 状態集約 + browser 中継
│   ├── controller.go      ← B層: 救助 Interest 組立(package ccnx 使用)
│   ├── observer.go        ← B層: tap :9300 で 8609 を decode し packet-event 化
│   ├── static/index.html  ← 単一ページ UI(Canvas2D + 3ペイン)。embed 配信
│   └── go.mod
└── README.md
```

C 側ヘッダ分割(ccnx.h / codec.h / ccnx_tables.h / transport.h)は責務境界の確定。
実装者はこの境界を越えて機能を混ぜない:
- codec.* = 8609 バイト ⇄ 構造体のみ(テーブルもソケットも知らない)。
- ccnx_tables.* = ccnx_name_t を鍵にした PIT/FIB/CS のみ(UDP を知らない)。
- transport.* = UDP バイト送受信 + tap 複製のみ(名前の意味を知らない)。
- ccnx.c(Portal) = 上記3者を束ね ccnx.h の API を実装(ドメイン非依存)。
- robot.c = B層。ccnx.h の関数だけを呼ぶ(codec/tables/transport を直接触らない)。

Go 側も同型: `package ccnx` が A層(codec/portal/tables)、`controller.go`/`observer.go` が B層。
`main.go`/`supervisor.go`/`hub.go` は操縦席インフラ(A層でも B層でもない)。

---

## 4. コンポーネント責務(確定)

### 4.1 A層(C: libccnx.a / Go: package ccnx)
- ドメイン非依存。Portal ハンドル = face(UDP) + PIT/FIB/CS + コールバック表を内包した
  「フォワーダ内蔵の自律 CCNx ノード」1個(実装範囲は限定サブセット)。
- Interest 受信: PIT 登録 → FIB 最長一致で nexthop へ転送、または登録プレフィックス一致なら
  `on_interest` 起動。傍受した CO は CS に載せ `on_content(is_overheard=1)` を上げる。
- CO 受信: CS 格納 → PIT 逆引きで要求元へ返送 → `on_content`。
- HopLimit 減算、Interest 集約(同名 PIT の統合)、CS 期限切れ掃除、Interest Return(NO_ROUTE)を担う。
- C/Go とも WIRE.md に**厳密**に従う。両者のベクタテスト(INTEROP.md)がバイト一致で合格すること。

### 4.2 B層 — C: swarm_robot(×N)
- 起動引数で自 id / listen ポート(9200+i)/ peer ポート一覧 / tap ポート(9300)/ 初期座標を受ける(PROTOCOL §5)。
- `ccnx_portal_open` → `ccnx_register_prefix("/area/{zone}", ...)` と近傍 FIB ルートを設定。
- メインループ: `ccnx_tick(100ms)` → 状態機械更新(PROTOCOL §1)→ 位置更新 → 状態 JSON を stdout emit
  (on-change + 500ms ハートビート)。星自律 claim / 距離バックオフ選抜 / 協力搬送 / RF 姿勢交換を実装。
- SIGTERM で即終了(故障=プロセス消滅。ハンドラで小細工しない)。

### 4.3 B層 — Go: controller(goroutine)
- browser の task-command / scenario を受ける唯一の救助 Interest 送出元。
- `ccnx.Portal.ExpressInterest("/task/rescue/zone/{zone}/carriers/{n}", ...)` を全ロボ port へ送出。
- 送出のたび controller イベント JSON → hub → browser の packet-event/log-event。
- **ロボ以外で 8609 を送る唯一の主体。** hub/supervisor は 8609 を作らない。

### 4.4 B層 — Go: observer(goroutine)
- tap :9300 で 8609 datagram を受け `ccnx.Decode` → 名前/種別を packet-event 化して browser へ。
- ここでの decode 成功が「C→Go ライブ相互運用」の実演。decode 失敗はエラーログ(バグの物証)。

### 4.5 Browser(受像機)
- 3ペイン(PROTOCOL §3.0)。C が決めた座標をそのまま描く(位置を発明しない。rAF は 2 状態間補間のみ)。
- PIT/FIB/CS カウントは robot JSON の値をそのまま表示。外部 CDN 依存禁止(CSS/JS インライン)。

---

## 5. ビルド生成物(確定)

| 生成物 | 言語 | ソース | 用途 |
|--------|------|--------|------|
| `libccnx.a`    | C  | codec.c ccnx_tables.c transport.c ccnx.c | A層 静的ライブラリ |
| `swarm_robot`  | C  | robot.c + libccnx.a | ロボプロセス。swarmd が ×N exec |
| `codec_test`   | C  | ccnx_test.c + libccnx.a | WIRE ベクタ一致 + round-trip 自己証明 |
| `swarmd`       | Go | server/*.go + server/ccnx + embed static | 操縦席。これだけ起動で全部立つ。package ccnx を内包 |
| (go test)      | Go | server/ccnx/codec_test.go | Go 側 ベクタ一致 + round-trip |

C 生成物は `client/` 直下に出力。swarmd は起動時に `swarm_robot` の絶対/相対パスを解決
(既定: swarmd と同階層 or `../client/`。PROTOCOL §5)。

**リンク構造:** robot.c は `libccnx.a` にのみリンクし codec/tables/transport の .o を直接触らない。
Go の controller/observer は `import "swarmd/ccnx"` で A層を使い、8609 バイトを自前で組まない。

**起動手順(デモ):**
1. `cd client && make`            → libccnx.a / swarm_robot / codec_test
2. `./client/codec_test`          → WIRE ベクタ一致 & round-trip PASS(C 側 物証)
3. `cd server && go test ./ccnx`  → Go 側 ベクタ一致 & round-trip PASS
4. `bash docs/interop_check.sh`(INTEROP.md)→ C-encode == Go-encode バイト一致 & 相互 decode PASS
5. `go build -o swarmd .` → `./swarmd` → `:8080` を開く。swarmd が Cロボ群を exec しデモ開始。
6. Wireshark を `lo0 udp portrange 9200-9300` に張る → 純 8609 を観測。

---

## 6. C↔Go A層 API 対応表(概念一致・言語慣用で表現)

| 概念 | C(ccnx.h) | Go(package ccnx) |
|------|-----------|------------------|
| ノード生成 | `ccnx_portal_open(addr,port,tap)` | `ccnx.Open(addr, port, tap)` |
| プレフィックス登録 | `ccnx_register_prefix(p,uri,cb,ctx)` | `p.Register(uri, OnInterest)` |
| FIB 経路追加 | `ccnx_fib_add_route(p,uri,port)` | `p.AddRoute(uri, port)` |
| Interest 送出 | `ccnx_express_interest(...)` | `p.ExpressInterest(uri, payload, hop, life, OnContent)` |
| CO 発行 | `ccnx_publish(p,uri,payload,len,fresh)` | `p.Publish(uri, payload, fresh)` |
| 1 反復実行 | `ccnx_tick(p,timeout)` | `p.Tick(timeout)` |
| 連続実行 | `ccnx_run(p)` / `ccnx_stop(p)` | `p.Run(ctx)` |
| テーブル計数 | `ccnx_pit_count/fib_count/cs_count` | `p.PitCount()/FibCount()/CsCount()` |
| encode/decode | `ccnx_encode/ccnx_decode` | `ccnx.Encode/ccnx.Decode` |

両実装で encode/decode が WIRE.md にバイト一致で従うことが唯一絶対の制約。API 名の綴りは言語慣用でよい。

---

## 7. 拡張点(初版から設計に織り込む)

- **N=2 協力搬送**: 初版は N=1 選抜が動けば可。バックオフ選抜は「傍受 accept 数 < n なら立候補継続、
  ≥ n なら降りる」の同一機構で N 一般化(PROTOCOL §1 の state で表現)。SYNC_WAIT バリア・RF 姿勢交換
  (`/robot/{id}/pose` 双方向)・故障時 RECRUITING 補充まで state 機械に含める。
- **実 Cefore 差し替え**: robot 1 体の A層を実 cefnetd 裏 portal 実装に差し替え可能にするため、
  8609 チャネルは RFC8609 準拠バイトに限定(独自ヘッダ禁止, WIRE §0)。その 1 体は状態 JSON を
  出さず Wireshark 観測のみで存在を示す。

---

## 8. RFC 外拡張: 非請求 Content Object(announce)パターン

本デモは **Interest を伴わない Content Object の push**(以下 announce)を用いる。
これは RFC 8569 の請求モデル(§2.4.4/§2.4.5 — CO は PIT に在る Interest を満たすために返る)の
**外側にあるアプリ層拡張**である。**知らずに違反しているのではなく、設計判断として逸脱し、
逸脱の範囲を以下に閉じている。**

### 8.1 announce を用いる名前(全列挙)

| 名前 | 意味 | 送出契機 |
|------|------|---------|
| `/star/{star_id}/claim` | 星の先取り宣言 | 近傍星の発見時 |
| `/star/{star_id}/collected` | 星を拾った報告 | 積載完了時 |
| `/star/{star_id}/deposited` | 星を回収箱へ入れた報告 | 降載完了時 |
| `/robot/{robot_id}/pose` | 自機の位置・方位・速度 | 協力搬送中の周期交換 |
| `/robot/{casualtyId}/repaired` | 復活通知 | 病院搬送完了時 |
| `/task/rescue/robot/{cid}/carriers/{n}/arrived/{rid}` | 現場到着通知 | casualty 位置に到着時 |
| `/task/rescue/robot/{cid}/carriers/{n}/pair` | ペア確定通知 | 先着機が相方を指名した時 |
| `/task/rescue/robot/{cid}/carriers/{n}/done` | 搬送完了通知 | 病院到着時 |
| `/bin/{team}/moved` | 回収箱の新位置告知 | 箱の移設完了時 |
| `/bin/{team}/claim` | 箱の運搬着手宣言 | 移設開始時 |

いずれも A層 API では `ccnx_publish` / `p.Publish` の 1 本で表現され、**B層はワイヤを知らない**
(§1 の層分離は維持されている)。

### 8.2 逸脱の範囲を閉じるための 3 つの規律

1. **ワイヤ形式は逸脱しない。** announce の datagram は RFC 8609 の **ContentObject そのもの**である。
   独自 PacketType・独自 TLV・独自ヘッダは 1 バイトも足していない(WIRE §0 の全閉決定)。
   したがって Cefore の decoder でもそのまま読める。逸脱しているのは「**いつ送るか**」だけである。
2. **非請求 CO を再転送しない。** 受信ノードは CS 格納とローカル配送で**終端**する。
   PIT に一致が無い CO を他ポートへ中継することは一切しない。この点は RFC 8569 §2.4 に準拠する。
   結果として announce の到達は**フルメッシュ前提の 1 ホップ限定**であり、
   マルチホップに拡散して系を汚すことはない。
3. **受信側の配送はローカル拡張に閉じる。** 受信ノードは `on_content(..., is_overheard=1)` /
   `OnContent(..., overheard=true)` でアプリへ渡す。これは **Portal API のローカル振る舞い**であって
   フォワーダ間のプロトコル挙動ではなく、ワイヤには一切現れない。`overheard` フラグにより
   B層は「請求して得た CO」と「傍受した CO」を常に区別できる。

### 8.3 なぜこの拡張を選んだか

群ロボの協調(先取り宣言・到着通知・姿勢交換)は本質的に **1 対多の非同期通知**である。
これを純粋な請求モデルで表すと、全機が全名前に対して常時 Interest を張り続ける必要があり、
PIT が N×名前数で膨張してデモの本質(協調挙動)が見えなくなる。
v1 は「ワイヤ形式の RFC 適合」と「協調挙動の可視化」を両立させるため、
**逸脱をアプリ層の送出契機のみに限定する**方針を採った。

実 cefnetd と相互接続する構成では announce は相手に届かない前提とし、請求形へ置き換える
(INTEROP §8 の制限事項 8)。

---

## 9. SHOULD / RECOMMENDED からの逸脱一覧と根拠

**MUST 適合性の主張範囲(第3次是正で訂正):** 「RFC の MUST に対する違反は無い」という
旧記述は**過大主張だったため撤回**する。正確な主張は次の二段である。

1. **本実装同士の閉域構成(v1 のデモ構成)では、既知の MUST 違反経路は無い**
   (第2次・第3次是正で検出された codec / フォワーダの MUST 違反は解消済み。
   検証範囲は §11 と INTEROP のとおりで、網羅証明ではない)。
2. **restriction / ExpiryTime 等を用いる他実装との相互接続では MUST に抵触しうる。**
   本表 7(KeyId / ContentObjectHash restriction を解釈しない)は、restriction 付き
   Interest に対し RFC 8569 §2.4.3 が要求する制約一致検査を行えず、
   本表 8(ExpiryTime を解釈しない)は、期限切れ CO で Interest に応答してはならない
   という RFC 8569 §4 の要求を保証できない。いずれも v1 スコープ外機能の**未解釈**に
   起因する制限であり、INTEROP §8 に相互接続時の危険として明記している。

以下は **SHOULD / RECOMMENDED からの意図的な逸脱**である。
各行に「なぜ逸脱したか」と「代わりに何で担保しているか(補償制御)」を明記する。

| # | 条文 | 規範語 | v1 の挙動 | 逸脱の根拠 | 補償制御 |
|---|------|-------|----------|-----------|---------|
| 1 | RFC 8569 §2.4.5(4)(Interest を満たした CO のみ CS に入れるべき) | SHOULD | **§2.4.5(1) の前ホップ検査を実装済み**(第3次是正で C/Go 対称)。CO は **FIB に nexthop として掲載された既知前ホップから来たときのみ** CS へ格納する(`handle_content`/`handleContent` の `from_fib_nexthop`/`isNexthop` ゲート)。残る SHOULD 逸脱は、前ホップさえ信頼できれば **PIT 不一致の傍受 CO も CS に入れる**点(§2.4.5(4) は「Interest を満たした CO のみ」)。傍受キャッシュが本デモの可視化対象そのもの(CS カウントが UI に出る)であるための意図的逸脱 | 前ホップ非信頼の CO は (a) CS に格納しない(汚染防止)、(b) PIT を満たさず下流へも転送しない(偽装応答注入防止)。ただしアプリ層の非請求 announce 径路(`on_content`/`OnContent` の overheard)へは渡す — 制御局(controller)はエフェメラルポートから `/bin/{team}/moved` を publish するため、完全破棄すると人の介入による箱移動が成立しないため(FIB 空ノードも「信頼できる前ホップ無し」として同様に扱う。C/Go 対称・Section F2 前ホップ試験で機械判定)。全ノードが同一運用者の閉域 127.0.0.1・無署名・外部到達なしで汚染攻撃面が無いこと、B層が CS を意思決定に使わない(星の重複回避は `star_claimed_by[]`、CS は表示専用)ことが残余リスクを閉じる |
| 2 | RFC 8569 §2.2(InterestLifetime) | — | **InterestLifetime をワイヤに載せない**(WIRE §0 で HeaderLength=8 固定)。PIT 期限は**ローカル固定 4 秒** | 独立2実装の**最小同一バイト**を保証するための全閉決定。可変ヘッダを許すと encode のバイト一致判定(INTEROP §1)が両実装の既定値差で壊れる | 4 秒はデモの時定数(バックオフ窓 0-2 秒 ≪ 移動数秒)に対して十分長く、B層の状態機械は PIT 満了に依存しない設計にしてある |
| 3 | RFC 8569 §2.2(consumer 既定 2 秒) | RECOMMENDED | ローカル既定を **4 秒**とし、RFC の consumer 既定 2 秒と相違する | 上記 2 と同じ理由でワイヤに出ないローカル値。デモの移動時間スケールに合わせた | 相互接続時に食い違う旨を INTEROP §8-2 に明記 |
| 4 | 受信 Interest の T_INTLIFE | — | **受信した T_INTLIFE を無視**し、自局固定 4 秒で PIT を保持する | v1 は hop-by-hop ヘッダ域を解釈しない(常に空を仮定)。本実装同士では常に空なので差は出ない | 相互接続時の制限事項として INTEROP §8-1 に明記。v2 で hop-by-hop パーサを追加する |
| 5 | RFC 8569 §2.4.4/§2.4.5(請求モデル) | — | **非請求 CO の push(announce)**を行う(§8) | §8.3 の通り、協調通知を請求モデルで表すと PIT が膨張しデモの本質が隠れる | 逸脱を「送出契機」のみに閉じる。ワイヤ形式は不変、非請求 CO は**再転送しない**、受信側配送は `overheard` 付きのローカル API に限定(§8.2) |
| 6 | 署名(Validation 域) | — | CO に Validation を**付けず**、受信した署名付き CO も**検証せずに受理**する | 閉域・単一運用者のデモ。鍵配布を持ち込むと納品範囲が発散する | WIRE §0 で「未署名」を全閉決定として明示。相互接続時の危険として INTEROP §8-4 に明記 |
| 7 | restriction 付き Interest(KeyId / ContentObjectHash) | — | **非対応**。制約を無視して名前一致だけで扱い、`08 T_RETURN_UNSUPPORTED_HASH_RESTRICTION` も返さない | v1 の名前空間(PROTOCOL §1)は restriction を使わない | 相互接続時に誤った CO を返し得る点を INTEROP §8-3 に明記 |
| 8 | ContentObject の ExpiryTime / RecommendedCacheTime | — | ワイヤに載せない。CS 保持は**ローカル固定 10 秒** | 上記 2 と同じくバイト一致のための全閉決定 | 期限切れ掃除は `ccnx_cs_expire` が毎 tick 実行し、無限保持はしない |

★この表の存在自体が主張である: v1 は RFC を**部分実装**しており、どこを実装しどこを実装していないかを
実装者が把握している。未把握の逸脱が見つかった場合は、直すか本表に行を足すかのどちらかを必ず行う。

---

## 10. C↔Go A層 挙動の統一(正 = C 方式)

§6 の API 対応表は「概念の対応」を定めたにすぎず、**フォワーダとしての挙動**までは定めていなかった。
先行監査で以下 8 点、第3次是正の実測監査でさらに 4 点(9〜12)の C↔Go 差分が特定されたため、
**C 方式を正として統一済み**である。

統一方向を C とした理由: (a) C 実装(`libccnx.a`)が納品対象そのものであること、
(b) 動作デモが C 側の挙動で検証済みであること、(c) 各項目とも C 側のほうが RFC 8569 §2.4 の
請求モデルに忠実であること(下表「根拠」列)。

| # | 論点 | **正(C 方式)の挙動** | 統一前の Go の挙動 | 根拠 | 状態 |
|---|------|--------------------|------------------|------|------|
| 1 | Interest 受信時のローカル配送と転送の関係 | **3 経路の排他選択**。(a) CS ヒット → 前ホップへ CO を返して終端。(b) 登録プレフィックス最長一致 → PIT へ載せ `on_interest` を起動して**終端**(nexthop へ転送しない)。(c) いずれでもない → 純フォワーダとして転送 | PIT 登録後、FIB nexthop があれば転送し、**加えて**ローカル cb も呼ぶ(配送と転送の二重実行) | RFC 8569 §2.4.4。プロデューサとして満たす Interest を再転送するとフルメッシュ既定ルート `/` の下で全ピアへ再拡散し、重複配送とループを生む | 統一済 |
| 2 | Interest 集約 | 同名 PIT が既に在れば**要求元ポートを追加するだけで再転送しない** | 集約判定なし。同名 Interest のたびに転送する | RFC 8569 §2.4(Interest 集約) | 統一済 |
| 3 | NO_ROUTE(Interest Return)の発生条件 | **CS ミス かつ 登録プレフィックス不一致 かつ FIB 一致経路なし**のときだけ生成する。受信 datagram を複製し PacketType=0x02 / ReturnCode=0x01 に**バイト長を変えずに**書き換えて前ホップへ返す | 「ローカル配送もされず転送も成功しなかった」条件で生成。送信エラーの有無に依存し、CS ヒット時の扱いも異なる | WIRE §1.3 / RFC 8609 §3.2.3。「経路が無い」以外の理由で NO_ROUTE を返さない | 統一済 |
| 4 | Interest Return 受信時の動作 | **上位へ通知せず破棄**する。PIT / pending は lifetime 満了で自然消滅させる | 一致するローカル PIT を**即削除**する | v1 は Interest Return 起因の再送・フェイルオーバ機構を持たない。1 つの nexthop からの NO_ROUTE で PIT を消すと、他 nexthop がまだ満たし得る要求を取り消してしまう | **未統一(意図的)**: Go は §10.3 の送出先照合を実装し、照合に合格した Return でのみ**ローカル要求元ぶんだけ**を取り消す(下流要求元は待たせたまま)。C は破棄のまま。Go の方が条文に沿っており、機能を落としてまで C に合わせない判断 |
| 5 | 経路なし `ExpressInterest` の PIT 状態 | FIB 一致が無ければ **PIT も pending も作らず**にエラー(`CCNX_ERR_NOMATCH`)を返す。**副作用なし** | PIT を先に作ってから `ErrNoRoute` を返すため、満たされないエントリが lifetime まで残り `pit_count` が実態と乖離する | 送出していない Interest の PIT を作らない(PIT は「未応答の要求」の表であり、UI にそのまま出る) | 統一済 |
| 6 | `cb=NULL` で送った Interest への CO 配送 | pending のコールバックが NULL なら **default_on_content(`overheard=0`)へフォールバック**して必ず配送する | `cb == nil` なら誰にも配送せず CO を捨てる | B層は `publish_help` / `express_recruit` を `cb=NULL` で送り、応答を既定ハンドラで受ける設計。捨てると救助フローが成立しない | 統一済 |
| 7 | `Publish` のファンアウト集合 | **「その名前に一致する PIT の要求元(ローカル要求元を除く)」∪「その名前に FIB 最長一致する nexthop」**を重複排除して送る | **FIB の全 nexthop**(名前一致を見ない)∪ PIT 要求元へ送る | 送出先は必ず**名前一致を経た**集合であること。FIB を名前と無関係に全展開するのは転送表の意味を壊す | 統一済 |
| 8 | NO_ROUTE 返送後の PIT 残置 | NO_ROUTE を返す経路では **PIT を作らない**(満たされない PIT を残さない) | PIT 挿入後に NO_ROUTE を返すため、満たされないエントリが lifetime まで残る | 上記 5 と同じ理由 | 統一済 |
| 9 | 期限切れ PIT の扱い | **失効エントリは不在扱い**。集約・延長・充足の対象にせず遅延削除する(`ccnx_pit_find` / `ccnx_pit_insert` の W-3) | `pit.find` が期限を見ず、失効エントリを延長して新規 Interest を集約(=握り潰し)していた | RFC 8569 §2.4.2(過去の Interest record は valid pending ではない。最初の受信は MUST forward) | 統一済(第3次) |
| 10 | ローカル express の PIT `maxHopLimit` | **送出 HopLimit を §2.4.2 の高水位として記録**する | 未設定(0)のまま残り、同値 HopLimit のリモート Interest を「larger」と誤判定して再転送 | RFC 8569 §2.4.2(larger HopLimit 例外の判定基準は pending の最大値) | 統一済(第3次) |
| 11 | NO_ROUTE 判定の集合 | **FIB 最長一致 nexthop から到来元を除いた転送可能集合が空**なら NO_ROUTE(`ccnx_fib_has_route`) | 到来元除外前のエントリ存在だけを見るため、唯一の nexthop が到来元のとき Return を返さず PIT を残して黒穴化 | RFC 8569 §10.3.1(E-3 と同根) | 統一済(第3次) |
| 12 | 受信 HopLimit=1 の純転送経路の PIT | 転送できない Interest は **PIT を作らず破棄**(上流に何も出していない=満たされる径路が無い) | HopLimit<2 でも PIT を作成し、C(PIT 0)と乖離 | RFC 8569 §2.4.1 + PIT の意味論(上記 5・8 と同根) | 統一済(第3次) |

**本表の位置づけ:** 本表は契約である。以後どちらかの実装が本表から外れた場合、
**その実装のバグ**として扱う(WIRE.md がワイヤの単一の真実であるのと同じ関係)。
統一が完了していない項目が生じた場合は、隠さずに当該行の「状態」を **未統一** に書き換え、
両者の挙動を差分として並記すること。

**検証状態(第3次是正で一部機械化。行ごとに状態が異なるので過大にまとめない):**
本表は 2 段の interop **Section F** で機械判定する。ただし**全 12 行を覆うわけではない**。

- **F1(表レベル)** — PIT/FIB が返す判定フラグ(集約 / retransmit / larger HopLimit /
  NO_ROUTE 集合 / is_nexthop / 期限切れ PIT 不在)を、C=`codec_test fwdtest`
  (`ccnx_tables.c` 直叩き)と Go=`portal_test.go` が同一不変条件で検査する。
- **F2(Portal レベル・実送信+宛先)** — Portal がフラグに従って実際にワイヤへ出した
  datagram を、**tap(本文・総数)に加え実 nexthop 面/到来面のソケットを直接読んで宛先まで**
  検査する。C=`codec_test porttest`(`ccnx.c` を実 UDP で駆動)、Go=`TestPortalForwardSendBehavior`
  / `TestPortalPrevHopCO`。宛先を読むので「本文は正しいが誤った face に送る」回帰も赤くなる
  (変異試験で確認済み: 集約抑止を外すと F2 のみ赤 / 転送先を到来元へ変えると nexthop 面が空で赤)。

**行別の被覆(正直な対応表):**

| §10 行 | 内容 | Section F での状態 |
|---|---|---|
| 2 | Interest 集約(抑止) | **機械判定(F1+F2。C/Go)** |
| 6 | `cb=NULL` の CO を既定ハンドラへ | **機械判定(F2 前ホップ(a)。C/Go)** |
| 8 | NO_ROUTE 返送後に PIT を残さない | **機械判定(C=porttest の pit_count 増分0 / Go=`TestForwarderPITSymmetry`(c))** |
| 9 | 期限切れ PIT 不在 | **機械判定(F1。C=fwdtest / Go=`TestPITExpiredEntryAbsent`)** |
| 10 | ローカル express の maxHopLimit 高水位 | **機械判定(F1+F2。C/Go)** |
| 11 | NO_ROUTE の split-horizon 判定 | **機械判定(F1+F2。C/Go)** |
| 12 | 受信 HopLimit=1 の無送出・無 PIT | **機械判定(C=porttest の送出0+pit_count 増分0 / Go=`TestForwarderPITSymmetry`(b)+F2(5))** |
| 3 | NO_ROUTE 生成条件 | **部分**(F2 は FIB 無経路枝で NO_ROUTE を出すことのみ検査。CS ヒット/登録プレフィックス一致時に NO_ROUTE を**出さない**枝は未検査=行1と同根) |
| 1 | CS ヒット/登録プレフィックス終端の枝 | **部分**(F2 は転送・NO_ROUTE 枝のみ。CS 応答/ローカル終端の実送信は未検査=レビュー保証) |
| 5 | 経路なし express で PIT を作らない | **レビュー保証**(F 未検査) |
| 7 | `Publish` のファンアウト集合 | **レビュー保証**(F 未検査) |
| 4 | Interest Return 受信時の動作 | **対象外**(意図的に Go≠C。統一しないので等価検査もしない) |

残る未機械化: 行 1・行 3 の「NO_ROUTE を出さない/CS・登録終端の実送信」枝、行 5・行 7、および
**同一ランダム trace を両スタックで再生し datagram 列を全一致比較する differential(元 interop E
構想)**。Section F は本表で「機械判定」と記した行(2/6/8/9/10/11/12)の列挙不変条件を C/Go 両側で
固定するが、網羅的 fuzzing 差分ではない。将来作業として残す(INTEROP §6.3)。

---

## 11. 適合性検証の範囲(何が証明済みで、何が未証明か)

| 対象 | 証明手段 | 状態 |
|------|---------|------|
| RFC 8609 ワイヤ表現(encode/decode) | INTEROP §1〜§4 の A〜D(独立2実装のバイト一致 + 相互 decode + 交差 round-trip) | **機械判定済み** |
| Interest Return のワイヤ表現(全 ReturnCode) | INTEROP §0.1 の CLI 契約 + 追加ベクタ | **機械判定済み** |
| 受理境界(空名前・最大セグメント・最大ペイロード) | INTEROP §3.1 の境界ベクタ | **機械判定済み** |
| MUST/MUST-NOT の負検査(T_PAD in Name・Validation 前置・PT/型不整合・空名前生成・ReturnCode 0) | interop **Section E**(C/Go 両側が同一の不正ワイヤを decode/encode 拒否) | **機械判定済み** |
| RFC 8569 フォワーダ判定フラグの C↔Go 等価性 | interop **Section F1**(C=`codec_test fwdtest` が `ccnx_tables.c` を、Go=`portal_test.go` が同一不変条件を検査。期限切れ PIT 不在・maxHopLimit 高水位・larger/retransmit の MUST forward 例外・NO_ROUTE split-horizon・is_nexthop) | **機械判定済み**(第3次是正) |
| RFC 8569 フォワーダ **実送信挙動+宛先** の C↔Go 等価性(§10 行 2/6/8/9/10/11/12) | interop **Section F2**(C=`codec_test porttest`・Go=`TestPortalForwardSendBehavior`/`TestPortalPrevHopCO`。tap に加え**実 nexthop 面/到来面のソケットを直接読み宛先まで**検査: 転送先=nexthop・HopLimit-1 / 集約抑止 / retransmit・larger の再転送 / HopLimit=1 無送出+無 PIT / NO_ROUTE Return は到来面へ+無 PIT / 前ホップ CO=trusted は CS格納+充足overheard=0・untrusted は CS非格納だが overheard=1 配送) | **機械判定済み**(第3次是正。§10 の一部行のみ。行 1・3 は部分、行 5・7 はレビュー保証、行 4 は非対象=§10 被覆表参照) |
| フォワーダ全挙動の網羅的 differential(ランダム trace 全一致) | 未実装(Section F は列挙不変条件のみ) | **未検証**(将来。INTEROP §6.3) |
| 実 cefnetd との相互接続 | 未実施 | **未検証**。既知の制限は INTEROP §8 |
| 署名検証・restriction・フラグメント | 未実装 | **範囲外**(§9 の 6・7、INTEROP §8) |

★専門家読者向けの要点: 本プロジェクトは「RFC 8569/8609 を完全実装した」とは主張しない。
**RFC 8609 のワイヤ表現については独立2実装のバイト一致で証明済み**、
**RFC 8569 のセマンティクスについては §9 の逸脱一覧の範囲で部分実装**、
という二段の主張である。逸脱は全て本書 §8/§9 と INTEROP §6〜§8 に列挙されており、
未列挙の逸脱が発見された場合は仕様バグとして本書に追記する。
