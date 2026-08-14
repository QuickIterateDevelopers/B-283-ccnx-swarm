# B-283 client/Makefile — 薄い転送。実体はリポジトリ直下の Makefile(ビルド担当)。
# DESIGN.md §5 手順1「cd client && make」を維持するための橋渡し。
# 生成物(libccnx.a / swarm_robot / codec_test)は本ディレクトリ直下に出る。
MAKEFLAGS += --no-print-directory

.PHONY: all lib robot codec_test test clean help
all:
	$(MAKE) -C .. all
lib robot codec_test test clean help:
	$(MAKE) -C .. $@
