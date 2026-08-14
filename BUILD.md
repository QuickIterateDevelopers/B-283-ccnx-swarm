# BUILD.md — ビルド・実行手順書

このリポジトリを clone してから、デモ UI をブラウザで開くまでの手順。
設計・仕様の正本は `docs/` の4文書（DESIGN → WIRE → PROTOCOL → INTEROP）。

## 1. 必要なもの

| ツール | 要件 | 備考 |
|---|---|---|
| C コンパイラ | C11 対応（gcc / clang） | `cc` として呼べること |
| make | GNU make | |
| Go | 1.22 以降 | `server/`（操縦席＋第2実装）のビルドに使用 |
| OS | Linux / macOS | 下記の検証済み環境を参照 |

外部ライブラリ依存はない（C は libm のみ、Go は標準ライブラリのみ、UI は全インライン）。

## 2. ビルド

```bash
git clone <このリポジトリ>
cd B-283-ccnx-swarm
./run.sh --build-only
```

`run.sh --build-only` は次の2つを実行するだけなので、個別にやってもよい:

```bash
make all                          # C: client/libccnx.a, swarm_robot, codec_test
( cd server && go build -o swarmd . )   # Go: swarmd（操縦席 + package ccnx）
```

生成物:

| パス | 内容 |
|---|---|
| `client/libccnx.a` | A層 CCNx スタック（RFC8569/8609・ドメイン非依存） |
| `client/swarm_robot` | Cロボット（B層。swarmd が9体 exec する） |
| `client/codec_test` | C 単体の codec 試験 |
| `server/swarmd` | Go 操縦席（HTTP/WS UI・スーパーバイザ・第2実装内蔵） |

## 3. 相互運用の機械判定（推奨: 起動前に一度）

```bash
bash interop/run.sh
```

INTEROP.md の命題 A〜F を機械判定する。最後に `PASS=35  FAIL=0` と
`INTEROP GREEN` が出れば、C/Go 独立2実装が同一ワイヤに収束している。
1件でも FAIL なら WIRE.md を正として裁定する（INTEROP.md 参照）。

`./run.sh --check` でビルド→判定→起動を一括実行することもできる（全緑でなければ起動中止）。

## 4. デモの起動とブラウザ表示

```bash
./run.sh
```

- swarmd が Cロボット×9体を起動し、**http://localhost:8080/** で UI を配信する。
- **注意: 待受は `*:8080`（全インターフェース）**。同一 LAN からは
  `http://<マシンのIP>:8080/` で直接開ける。外部に出したくない場合は
  `server/supervisor.go` の `HTTPAddr()` を `127.0.0.1:%d` に変更するか、
  ファイアウォールで閉じたうえで SSH トンネル
  （`ssh -L 8080:localhost:8080 <host>`）経由で開く。
- 停止は Ctrl-C（swarmd がロボ群も道連れに終了する）。

### ワイヤ観測（任意）

ロボ間の実 CCNx/UDP パケット（RFC8609 バイト）はループバックに流れる:

```
Wireshark / tcpdump:  interface lo (macOS: lo0),  filter: udp portrange 9200-9300
```

## 5. 検証済み環境

| OS | コンパイラ | Go | 結果 |
|---|---|---|---|
| macOS (Apple Silicon) | Apple clang | — | ビルド・デモ・interop 全緑 |
| Ubuntu 24.04 (x86_64, kernel 6.8) | gcc 13.3.0 | go1.26.5 | ビルド・デモ・**interop PASS=35/FAIL=0** |

## 6. トラブルシュート

| 症状 | 原因と対処 |
|---|---|
| `CLOCK_MONOTONIC undeclared`（Linux） | `-std=c11` が POSIX 拡張を隠すため。現行 Makefile は `-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE` を定義済み。古いチェックアウトでは `make all CPPFLAGS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"` |
| ポート 8080 が使用中 | 既存プロセスを止めるか、`server/supervisor.go` の `HTTPPort` を変更して再ビルド |
| interop が RED | `docs/WIRE.md` が単一の真実。テストベクタと突き合わせて裁定（INTEROP.md §5） |
| UI は出るがロボが動かない | swarmd の標準出力を確認（`robot N: stars 200 個ロード` が9行出るのが正常） |
