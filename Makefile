# B-283 CCNx セマンティック群ロボ通信デモ — ルート Makefile
# 担当: ビルド/実行/相互運用ハーネス/ドキュメントの接着
#
# 契約: docs/DESIGN.md §5(ビルド生成物表)を正とする。
#   libccnx.a   = A層静的ライブラリ  (codec.c ccnx_tables.c transport.c ccnx.c)
#   swarm_robot = ロボプロセス        (robot.c [+ B層補助] + libccnx.a)
#   codec_test  = WIRE ベクタ自己証明 (ccnx_test.c + libccnx.a)
#   swarmd      = Go 操縦席           (server/*.go + server/ccnx + embed static)
#
# 生成物は全て client/ 直下(DESIGN §5「C 生成物は client/ 直下に出力」)。
#
# ソース分解は C 担当の実装に追随できるよう wildcard で解決する:
#   - A層(libccnx)= client/*.c から「エントリ点(main を持つ)」と「B層補助」を除いた全て。
#     → DESIGN の {codec,ccnx_tables,transport,ccnx}.c でも、
#       別分解 {codec,forwarder,face,ccnx}.c でも同一の結果になる。
#   - エントリ点 = robot.c(swarm_robot) と ccnx_test.c(codec_test)。
#   - B層補助(あれば)= behavior.c / movement.c / state_emit.c → swarm_robot にのみリンク。

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
CPPFLAGS += -Iclient
# Linux glibc では -std=c11 が POSIX 拡張(clock_gettime の CLOCK_* 等)を隠すため明示する。
# macOS では無害(既定で可視のため no-op)。
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDLIBS  += -lm
AR      ?= ar
ARFLAGS := rcs

CLIENT  := client
SERVER  := server

# --- エントリ点(main を持つ翻訳単位) ---
ROBOT_MAIN     := $(CLIENT)/robot.c
# codec_test の main: 契約 = client/ccnx_test.c。旧名 test/test_codec.c があれば従。
CODEC_TEST_SRC := $(firstword $(wildcard $(CLIENT)/ccnx_test.c test/test_codec.c))

# --- B層 robot 補助(あれば swarm_robot 側にのみリンク。libccnx には入れない) ---
ROBOT_EXTRA := $(wildcard $(CLIENT)/behavior.c $(CLIENT)/movement.c $(CLIENT)/state_emit.c)

# --- A層 libccnx ソース = client/*.c − エントリ点 − B層補助 ---
ALL_CLIENT_C := $(wildcard $(CLIENT)/*.c)
LIB_SRCS := $(filter-out $(ROBOT_MAIN) $(CLIENT)/ccnx_test.c $(ROBOT_EXTRA),$(ALL_CLIENT_C))
LIB_OBJS := $(LIB_SRCS:.c=.o)

LIB       := $(CLIENT)/libccnx.a
ROBOT_BIN := $(CLIENT)/swarm_robot
TEST_BIN  := $(CLIENT)/codec_test

CLIENT_HDRS := $(wildcard $(CLIENT)/*.h)

.PHONY: all lib robot codec_test test server swarmd interop clean help
.DEFAULT_GOAL := all

## all: libccnx.a + swarm_robot + codec_test を作る
all: $(LIB) $(ROBOT_BIN) $(TEST_BIN)

## lib: A層静的ライブラリ libccnx.a
lib: $(LIB)

## robot: ロボ実行ファイル swarm_robot
robot: $(ROBOT_BIN)

## codec_test: WIRE ベクタ自己証明バイナリ
codec_test: $(TEST_BIN)

# --- 実際のビルド規則 ---
$(CLIENT)/%.o: $(CLIENT)/%.c $(CLIENT_HDRS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJS)
	@if [ -z "$(LIB_OBJS)" ]; then \
	  echo "ERROR: client/ に A層ソース(.c)が無い。C 担当の codec.c/ccnx_tables.c/transport.c/ccnx.c を待つ。" >&2; \
	  exit 1; fi
	$(AR) $(ARFLAGS) $@ $(LIB_OBJS)

$(ROBOT_BIN): $(ROBOT_MAIN) $(ROBOT_EXTRA) $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(ROBOT_MAIN) $(ROBOT_EXTRA) $(LIB) -o $@ $(LDLIBS)

$(TEST_BIN): $(CODEC_TEST_SRC) $(LIB)
	@if [ -z "$(CODEC_TEST_SRC)" ]; then \
	  echo "ERROR: client/ccnx_test.c(または test/test_codec.c)が無い。" >&2; exit 1; fi
	$(CC) $(CFLAGS) $(CPPFLAGS) $(CODEC_TEST_SRC) $(LIB) -o $@ $(LDLIBS)

## test: codec_test をビルドして selftest を実行(WIRE ベクタ一致 + round-trip)
test: $(TEST_BIN)
	$(TEST_BIN) selftest

## server (別名 swarmd): Go 操縦席をビルド
server swarmd:
	cd $(SERVER) && go build -o swarmd .

## interop: C↔Go バイト一致 + 相互 decode ハーネスを実行(INTEROP.md 命題 A〜D)
interop: all
	bash interop/run.sh

## clean: 生成物を削除
clean:
	rm -f $(CLIENT)/*.o $(LIB) $(ROBOT_BIN) $(TEST_BIN) $(SERVER)/swarmd $(SERVER)/ccnxcli
	rm -f /tmp/b283_c_vectors.txt /tmp/b283_go_vectors.txt

## help: ターゲット一覧
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  /'
