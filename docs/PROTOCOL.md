# B-283 PROTOCOL.md — 名前空間 / 状態JSON / WS / IPC 厳密仕様(単一の真実)

本書は DESIGN.md を親、WIRE.md を下位ワイヤ規定とする。JSON は全て **UTF-8, 1 行(改行区切り)**、
キーは以下に列挙したもの以外を出さない。数値の型・単位・座標系を含め実装者が独自解釈しない。

座標系: 平面 **40m(x) × 30m(y)**、原点 (0,0) = 左上、x 右・y 下が正。単位はメートル(浮動小数)。
時間: `t` は Unix epoch ミリ秒(整数)。デモ時間倍率 ×3(実時間トグルあり)は B層内部の話で JSON には出さない。

---

## 1. 名前空間(実装と一致。Interest 4系統 + 非請求 CO announce)

| 用途 | CCNx 名(URI) | 種別 | 送出元 | 補足 |
|------|--------------|------|--------|------|
| ロボ担当登録 | `/area/{zone}` | Interest 宛先プレフィックス | (受け皿) | 例 `/area/A-2`。ロボが prefix 登録。v1 は登録のみで応答しない |
| 救助タスク | `/task/rescue/robot/{casualtyId}/carriers/{n}` | Interest | controller / casualty / carrier | 例 `/task/rescue/robot/3/carriers/2`。患者=止まった故障ロボ。**位置(x,y)は Payload で運ぶ**(改修1)。controller 送出のほか、casualty 自身が周期「助けて」ビーコンとして、相方喪失時は carrier が `carriers/1`(補充)として同名前空間を再利用する |
| 帰還指令 | `/command/home` | Interest | controller | 「おうちに戻れ」= 全機を ControlCenter へ。救助従事中/救助対象の機は無視して任務継続 |
| エリア集中指令 | `/command/focus/{zone}` | Interest | controller | 「dur 秒間このエリアの星だけ取れ」。矩形 `x0,y0,x1,y1` と `dur` は Payload。C 側はゾーン地図を持たず Payload を正とする |
| 到着通知 | `/task/rescue/robot/{casualtyId}/carriers/{n}/arrived/{robot_id}` | ContentObject(announce) | carrier 候補 | casualty に着いた機が撒く。先着機が到着順を集めて相方を決める |
| ペア確定 | `/task/rescue/robot/{casualtyId}/carriers/{n}/pair` | ContentObject(announce) | 先着機 | `{a,b}` の 2 機を指名。b は自分が選ばれたと知り、他機は撤退 |
| 搬送完了 通知 | `/task/rescue/robot/{casualtyId}/carriers/{n}/done` | ContentObject(announce) | carrier | 搬送完了を系に告知 |
| 復活通知 | `/robot/{casualtyId}/repaired` | ContentObject(announce) | carrier | 病院搬送完了で carrier が publish。casualty が傍受で修理→復活(改修1) |
| 姿勢共有 | `/robot/{robot_id}/pose` | ContentObject(announce) | 各ロボ | **周期 pose announce**。平常時は低頻度(6 sim秒)で同チーム分離用、協力搬送中は高頻度(0.5 sim秒)で隊形維持用。傍受した機が相方/僚機の位置を更新する。※後述の注記 |
| 星 claim(自律) | `/star/{star_id}/claim` | ContentObject(announce) | 各ロボ | 傍受した機が占有を記録し二重取りを避ける(TTL 付き) |
| 星 回収報告 | `/star/{star_id}/collected` | ContentObject(announce) | 各ロボ | 拾った瞬間の報告。傍受した機が「回収済」を伝播し、UI も星を消す |
| 星 投函報告 | `/star/{star_id}/deposited` | ContentObject(announce) | 各ロボ | 箱に置いた瞬間の報告(可視化用) |
| 箱 運搬着手宣言 | `/bin/{team}/claim` | ContentObject(announce) | 各ロボ | 「この箱はいま私が運びます」。僚機は傍受でクールダウンし二重運搬を防ぐ(競合時は小さい id が勝つ) |
| 箱 新位置告知 | `/bin/{team}/moved` | ContentObject(announce) | ロボ **または controller** | 可動ゴミ箱の新位置。ロボが置いた時に送出。**人の介入(UI ドラッグ&ドロップ)時は controller が同じ名前で送出し `by=-1` を人の印とする**。同チーム機は傍受で投函先を更新 |

`{zone}` は `A-0`〜`A-9` 等の英数。`{robot_id}`/`{casualtyId}` は `0`〜`N-1` の十進。
`{star_id}` は `0`〜 の十進。`{team}` は箱(=チーム)番号 `0`〜`2` の十進。`{n}` は必要搬送機数(≥1)。
URI 規則は WIRE §3(先頭 `/` 必須・空セグメント禁止・ASCII 英数と `-`)。

> **姿勢共有の実体(用語の明確化)**
> `/robot/{id}/pose` は **非請求 ContentObject の announce**(publish して他機が傍受)であり、
> Interest ⇄ ContentObject の双方向交換ではない。リポジトリ全体で `/robot/{id}/pose` への
> Interest 送出は 1 件も無い。したがって本デモの隊形維持は
> **「周期 pose announce(publish + 傍受)による隊形維持」** と呼ぶ。
> `draft-irtf-icnrg-reflexive-forwarding` の機構(RNP・PIT 拡張)は本実装に一切無く、別物である。
> なお C ロボは `/robot/{id}/pose` に **Interest を受けたら pose CO を返す応答経路**
> (`on_pose_interest`)を登録済みだが、要求側が存在しないため現行シナリオでは呼ばれない。
> 非請求 CO の announce 自体は RFC 8569 §2.4.5(Content Object Pipeline: CO は PIT の
> 保留 Interest に照合される)の範囲を超えるアプリ層拡張であり、RFC の転送規定を
> 置き換えるものではない。

> **旧版からの差分**: `/task/rescue/.../accept/{robot_id}`(引受表明 CO)は現行実装には存在しない。
> 距離比例バックオフ後の accept publish ではなく、`arrived`(到着順)+ `pair`(先着機による指名)
> の 2 名前で選抜が成立する。`event` の `accept_sent` / `selected` も現行 B層は emit しない(§2.2)。

### 1.1 名前ごとのペイロード JSON(8609 Payload TLV の中身、コンパクト)
| 名前 | payload JSON | フィールド |
|------|------|------|
| 救助 Interest(controller 発) | `{"src":"ctl","dest":"hospital-1","robot":3,"x":12.5,"y":7.0,"t":1690000000000}` | src=発行元, dest=搬送先id, robot=casualty id, x/y=casualty 現在位置(m), t=発行時刻 |
| 救助 Interest(casualty の「助けて」ビーコン) | `{"robot":3,"x":12.5,"y":7.0,"dest":"hospital-1"}` | casualty が自位置を周期送出。取りこぼしても次周期で拾われる |
| 救助 Interest(`carriers/1` 補充) | `{"src":"5","dest":"hospital-1","robot":3,"x":12.5,"y":7.0,"t":1690000000000}` | src=補充を求める carrier の id(文字列), x/y=casualty 位置 |
| `/command/home` Interest | `{"cmd":"home"}` | 内容は使わない(名前だけで指令が成立) |
| `/command/focus/{zone}` Interest | `{"zone":"A-2","x0":7.0,"y0":5.0,"x1":13.0,"y1":11.0,"dur":10}` | 矩形(m)は左上-右下。dur=有効時間(実秒)。欠けると既定 10 秒 |
| arrived CO | `{"id":3,"t":1690000000000}` | id=到着した機, t=到着時刻(全機共通の到着順=選抜順の根拠) |
| pair CO | `{"a":3,"b":5}` | a=先着機(発行者), b=指名された相方。担架の頭側は 2 機のうち id の小さい方 |
| done CO | `{"casualty":7,"by":[3,5]}` | casualty=救助対象 id, by=搬送に成功した機 id 配列 |
| repaired CO | `{"id":7,"by":3,"t":1690000000000}` | id=復活する casualty, by=告知した carrier |
| pose CO | `{"id":3,"x":12.5,"y":7.0,"hd":1.57,"v":0.8}` | hd=進行方位rad, v=速度m/s |
| star claim / collected / deposited CO | `{"id":3,"star":7,"t":1690000000000}` | id=当該ロボ, star=星id(3 名前とも同形式) |
| bin claim CO | `{"team":0,"by":3,"t":1690000000000}` | team=箱(チーム)番号, by=運搬に着手した機 |
| bin moved CO | `{"team":0,"x":2.0,"y":28.0,"by":3,"t":1690000000000}` | x/y=箱の新位置(m), by=置いた機 id。**`by:-1` は人の介入(UI ドラッグ&ドロップ → controller 発)** |

現行実装の Interest は上表のとおり全て Payload TLV を持つ。Payload を持たない Interest も
WIRE §4 上は合法で、その場合 Payload TLV を出さない。

---

## 2. 状態機械(B層 robot.c。全 state を列挙。JSON の `state` はこの綴りをそのまま用いる)

列挙順は `client/behavior.h` の `robot_state_t` と `STATE_NAMES[]` に一致する(全 22 状態)。

```
IDLE              初期状態。次反復で PATROL へ
PATROL            星探索徘徊(公称1.5m/s)
STAR_CLAIMING     近傍(半径1.0m)星発見 → /star/{id}/claim を publish
STAR_ENROUTE      claim 成立、星へ移動
STAR_LOADING      積載(1s 固定)。拾った報告 /star/{id}/collected を publish
STAR_CARRYING     自チーム回収箱(3個)へ搬送。箱が動けば /bin/{team}/moved 傍受で追従
STAR_UNLOADING    降載(1s 固定)。/star/{id}/deposited を publish
BIDDING           救助 Interest 受信 → 距離比例バックオフ(0-2000ms)後、casualty へ向かう
                  (事前選抜はしない。選抜は到着順で決まる)
SELECTED          予約状態。到着順ベースの現行選抜では遷移せず、入った場合は WITHDRAWN へ落ちる
PAIRING           予約状態。同上(SELECTED と同じ扱い)
ENROUTE_PICKUP    casualty 位置(Interest Payload の x,y)へ移動
SYNC_WAIT         到着 → /task/.../arrived を publish し settle 窓(0.4s)で到着順を固める。
                  先頭 n 機が carrier。先着機が /task/.../pair を撒いて確定、圏外機は WITHDRAWN
COOP_LOADING      2機で積載(3s 固定)
COOP_CARRYING     隊形保持で病院へ搬送(0.8m/s)。周期 pose announce(/robot/{id}/pose の CO を
                  0.5 sim秒ごとに publish)を相方が傍受して担架隊形を保つ
COOP_UNLOADING    降載(3s 固定)。現行フローでは未使用(病院到着で即開放するため)
RECRUITING        搬送中に相方の pose 途絶(2s)を検知 → carriers/1 で 1機分 再選抜(待ち続ける)
WITHDRAWN         選抜から降りた(到着順の先頭 n 圏外 / 他所のペア確定を傍受)。次反復で PATROL
FAILED            回復不能。現行 B層では到達しない(保険。自己 FAILED は撤去済み)
NEEDS_RESCUE      救助対象化(停止・生存)。SIGUSR1 または自動故障で遷移し「助けて」を周期送出。
                  carrier に病院へ運ばれ /robot/{id}/repaired 受信で REPAIRING へ(改修1)
REPAIRING         病院に置き去り → 修理待ち(15 sim秒)。完了で PATROL へ復活
GOING_HOME        /command/home 受信 → ControlCenter へ帰還。着いたら PATROL
BIN_CARRYING      自チームのゴミ箱を星の密集地へお引っ越し中(0.8m/s)。着手時に /bin/{team}/claim、
                  設置時に /bin/{team}/moved を publish。運搬中は pose を高頻度化して僚機に追従させる
```

自動故障: 全 N 台合計で平均およそ実時間 1台/分 が倒れる。倒れた機は **FAILED ではなく
NEEDS_RESCUE(casualty 化)** になり、復活後はクールダウン中は抽選対象外。
明示操作(fault-inject=SIGTERM / 救助対象化=SIGUSR1)も従来どおり。

★改修1: 患者はゾーン中心の抽象患者ではなく「止まった故障ロボ(casualty)」。オペレータが
UI で 1 体選ぶ → SIGUSR1 で NEEDS_RESCUE に停止(プロセスは生存)→ carrier 2 体が
casualty 位置(Interest Payload の x,y)へ距離バックオフ選抜で集結 → 病院へ協力搬送 →
carrier が `/robot/{casualtyId}/repaired` を publish → casualty が傍受で復活し PATROL 再開。
自然発生的な故障(自己 FAILED)は撤去した。

優先度: 救助 > 星 > 待機。星は中断可能(救助 Interest で割込)。患者搬送中の機は入札不参加。
時定数: バックオフ窓(0-2s) << 移動(数s)。Interest Lifetime=4s / CS 保持=10s。
位置更新 20Hz。状態 emit = on-change + 500ms ハートビート。

### 2.1 状態 JSON スキーマ(robot → stdout、1 行。DESIGN §4.2)
```json
{"type":"state","id":3,"t":1690000000000,"pos":{"x":12.5,"y":7.0},"zone":"A-2",
 "state":"COOP_CARRYING","partner_id":5,"casualty_id":7,"pit_count":2,"fib_count":4,"cs_count":1,
 "carrying":true,"event":"pair_formed"}
```
`casualty_id`(int): carrier が搬送中の救助対象ロボ id。carrier 以外/非救助時は `-1`(改修1)。
ブラウザは COOP_ 中の carrier ペア中点に casualty ロボ本体を「担架の乗客」として描く。
| キー | 型 | 説明 |
|------|----|------|
| `type` | string | 固定 `"state"` |
| `id` | int | ロボ id(0..N-1) |
| `t` | int | emit 時刻(epoch ms) |
| `pos` | object | `{"x":float,"y":float}` m。★ブラウザはこの座標をそのまま描く(発明禁止) |
| `zone` | string | 担当ゾーン(例 `"A-2"`)。未割当は `""` |
| `state` | string | §2 の綴りのいずれか |
| `partner_id` | int | 協力搬送の相方 id。無ければ `-1` |
| `pit_count` | int | 自ノード PIT エントリ数(`ccnx_pit_count`) |
| `fib_count` | int | 自ノード FIB エントリ数(`ccnx_fib_count`) |
| `cs_count` | int | 自ノード CS エントリ数(`ccnx_cs_count`) |
| `carrying` | bool | 何か(星/患者)を運搬中か |
| `event` | string | on-change の契機。無ければ `""`。値集合を §2.2 に固定 |

### 2.2 `event` 値集合(固定・これ以外を出さない)
```
"" spawn patrol star_claim star_pickup star_dropoff
bid_start withdrawn pair_formed
pickup_arrive sync_wait coop_load coop_dropoff
partner_lost recruiting recruited
needs_rescue repaired going_home
```
予約(`behavior.c` に定数はあるが現行 B層は emit しない): `accept_sent` / `selected` / `failed`。
`accept_sent` / `selected` は accept CO ベースの旧選抜、`failed` は撤去済みの自己 FAILED に対応する。

---

## 3. WebSocket スキーマ(Go swarmd ⇄ Browser)

`ws://<host>:8080/ws`。全メッセージは JSON オブジェクトで先頭キー `type` により判別。

### 3.0 ブラウザ 3 ペイン(受信メッセージの割当)
- (1) 指示コンソール: 送信側(§3.2)。救助タスク投入 / 故障注入 / シナリオ制御。
- (2) 応答表示ウィンドウ: `log-event` を時系列テキスト追記。
- (3) ロボ動作表示画面(Canvas2D): `state`(座標・色分け)+ `topology`(ゾーン/病院/星/回収箱)+
  `packet-event`(線を走るパケットアニメ)+ PIT/FIB/CS ライブ(state の各 count)。

### 3.1 server → browser
| type | 目的 | スキーマ |
|------|------|---------|
| `state` | 各ロボ状態 | §2.1 の JSON を**そのまま**中継(Go は改変しない) |
| `topology` | 静的レイアウト(起動時 + reset 時) | 下記 |
| `packet-event` | 8609 が走る事実 | 下記 |
| `log-event` | 系の実況テキスト | 下記 |
| `bin-state` | 可動ゴミ箱の新位置(`/bin/{team}/moved` の観測 or 人の介入) | `{"type":"bin-state","team":0,"x":2.0,"y":28.0}` |
| `star-state` | 回収済み星 id の同期(リロード時の巻き戻し用) | `{"type":"star-state","collected":[0,3,7]}` |

`topology`:
```json
{"type":"topology","field":{"w":40,"h":30},"n_robots":8,
 "zones":[{"id":"A-2","x":10,"y":8,"r":6}],
 "hospitals":[{"id":"hospital-1","x":36,"y":4}],
 "bins":[{"id":"bin-0","x":2,"y":28}],
 "star":[{"id":0,"x":15,"y":20}],
 "ports":{"robot_base":9200,"tap":9300,"http":8080}}
```
`packet-event`(observer/controller が生成。位置は from/to のポート→ロボ/病院に対応させブラウザが線を引く):
```json
{"type":"packet-event","t":1690000000000,"kind":"Interest","name":"/task/rescue/robot/3/carriers/2",
 "from":9300,"to":9202,"bytes":66,"cls":"p2p"}
```
| キー | 型 | 説明 |
|------|----|------|
| `kind` | string | `"Interest"` / `"ContentObject"` / `"InterestReturn"` |
| `name` | string | 8609 から decode した URI(observer が復元。ブラウザは表示のみ) |
| `from`/`to` | int | UDP ポート(9200+i / 9300 / controller)。ブラウザがノードに対応付け |
| `bytes` | int | datagram 長(WIRE のベクタ長と一致し得る) |
| `cls` | string | 描画意味づけ(改修2): `"p2p"`=ロボ間の線 / `"announce"`=特定宛先なし(送信元パルス) / `"toctl"`=制御局宛の線 / `""`=既定。**observer が名前から論理宛先を推論**して `to`/`cls` を決める。プロトコル挙動は不変(tap ミラー観測のまま) |

`log-event`:
```json
{"type":"log-event","t":1690000000000,"level":"info","text":"robot 3 arrived /task/rescue/robot/7/carriers/2 (d=4.2m)"}
```
`level` ∈ `{"info","warn","error"}`。

### 3.2 browser → server
| type | 目的 | スキーマ |
|------|------|---------|
| `task-command` | 救助タスク投入 | `{"type":"task-command","robot":3,"x":12.5,"y":7.0,"carriers":2}` |
| `fault-inject` | 故障注入(kill) | `{"type":"fault-inject","robot_id":3}` |
| `recall` | 「おうちに戻れ」 | `{"type":"recall"}` → controller が `/command/home` Interest を送出 |
| `focus-command` | エリア集中指令 | `{"type":"focus-command","zone":"A-2"}` → controller が `/command/focus/{zone}` Interest を送出(dur=10 秒固定) |
| `bin-command` | 箱の手動移動(人の介入) | `{"type":"bin-command","team":0,"x":12.0,"y":20.0}` → controller が `/bin/{team}/moved`(`by:-1`)を publish し、UI へは `bin-state` で同期 |
| `scenario` | シナリオ制御 | `{"type":"scenario","action":"start"}` |

`scenario.action` ∈ `{"start","reset","respawn"}`(reset=全体再配置, respawn=停止機の再起動)。
`task-command`: UI で救助対象(生存ロボ)を 1 体選ぶ。`robot`=casualty id、`x,y`=そのロボの
現在描画座標(コンソールが把握する C 決定値)、`carriers`(既定 2)。サーバは casualty を
SIGUSR1 で停止 → controller が `/task/rescue/robot/{robot}/carriers/{carriers}` を x,y 込みで送出。

---

## 4. コントローラ イベント JSON(controller goroutine → hub、内部)

controller は救助 Interest を送るたびに hub へ次を渡す(hub が packet-event / log-event に整形して browser へ):
```json
{"type":"ctl-event","t":1690000000000,"action":"express","name":"/task/rescue/robot/7/carriers/2",
 "to":[9200,9201,9202,9203,9204,9205,9206,9207],"bytes":66}
```
`action` ∈ `{"express","recruit","timeout"}`。`to` は送出先ポート配列。

---

## 5. C↔Go IPC 契約(確定)

### 5.1 チャネル分離
| チャネル | 方向 | 媒体 | 内容 |
|---------|------|------|------|
| 状態 | robot → Go | **子プロセス stdout, 改行区切り JSON** | §2.1 の state 行。1 行 1 JSON。Go は `bufio.Scanner` で読む |
| 制御(故障) | Go → robot | **SIGTERM** | 対象 robot を kill。robot はハンドラ無しで即死 |
| 制御(救助対象化) | Go → robot | **SIGUSR1** | 対象 robot を停止(NEEDS_RESCUE)。プロセスは生存し復活可能(改修1) |
| 制御(タスク) | browser → Go controller | WS(§3.2) | controller は Go 内 goroutine。stdin は使わない(旧版 C controller は廃止) |
| 8609 | robot ⇄ robot / controller → robot | **UDP** | WIRE.md のバイトのみ |
| 観測 | robot/controller → Go observer | **UDP tap :9300** | 送出 8609 の複製 |

robot は stdout に **state 行のみ** を出す(デバッグ文字列を混ぜない。混ぜると Go の JSON パースが壊れる)。
Go は robot の stderr を自身の stderr へ素通し(デバッグはそちら)。

### 5.2 UDP ポート割当(確定)
| 主体 | ポート | 役割 |
|------|--------|------|
| robot i(i=0..N-1) | **9200 + i** | 自ノード listen(Interest/CO 受信)。N=8 なら 9200-9207 |
| Go wire observer | **9300** | tap 受信(全 8609 datagram の複製がここへ来る) |
| Go controller | エフェメラル(送出専用ソケット) | 送出元。宛先は 9200+i(全ロボ)。送出複製を 9300 へも打つ |
| Go http/WS | **8080** | UI 配信 + WebSocket |

- robot は自身が送出する全 8609 datagram を「本来の宛先(peer の 9200+j)」に加え **9300(tap)** へも送る。
  これにより Go observer が C-encode を decode(C→Go ライブ相互運用, DESIGN §2)。
- controller の宛先は全ロボ port(ブロードキャスト相当)。各ロボは `/task/rescue` を prefix 登録して
  全救助 Interest を受け、ゾーンではなく **自機の状態と casualty までの距離** で参加可否を決める
  (従事中/入札中/救助対象なら不参加、それ以外は距離比例バックオフ後に BIDDING → 現場へ)。
- bind アドレスは全て `127.0.0.1`。Wireshark 対象は `lo0 udp portrange 9200-9300`。

### 5.3 robot 起動引数(supervisor → exec。順序固定)
```
swarm_robot --id <i> --port <9200+i> --tap <9300> \
            --peers <9200,9201,...,9207> \
            --zone <A-2> --x <float> --y <float> \
            --field 40x30 --n <N>
```
supervisor は各ロボの初期座標(x,y)と zone を割り当てて渡す(ブラウザは C が返す座標のみ描く)。
`--peers` は自分を含む全ロボ port の一覧(送出・傍受のため)。値は DESIGN §5 の解決パスで起動。
