/*
 * ccnx_test.c - codec_test CLI(INTEROP.md §0 の C 側試験接点)
 *
 * B-283 A層の自己証明 + C↔Go 相互運用の判定 CLI。
 * サブコマンド(INTEROP §0):
 *   codec_test vectors                     WIRE §6 の 3 ベクタを encode(name<TAB>HEX)
 *   codec_test encode <uri> <I|C> [phex]   指定名を Interest/ContentObject で encode(HEX)
 *   codec_test encode <uri> R <rc> [phex]  Interest Return で encode。rc は 2 桁 HEX
 *                                          (RFC 8609 §3.2.3.3 ReturnCode)
 *   codec_test decode <HEX>                decode(kind<TAB>uri<TAB>payloadHex)
 *                                          InterestReturn のときのみ末尾に
 *                                          <TAB>returnCode(2 桁大文字 HEX)が付く
 *   codec_test selftest                    内蔵ベクタ一致 + ランダム1000件 round-trip
 *
 * HEX 規約: 大文字, 1 バイト 2 桁, 区切り無し(INTEROP §0)。
 * kind 語彙: Interest / ContentObject / InterestReturn。
 * Interest / ContentObject の decode 出力書式は既存 interop 契約。絶対に変えない。
 */
#include "ccnx.h"          /* codec.h + Portal 定数(CCNX_DEFAULT_HOP_LIMIT/CCNX_URI_MAX) */
#include "ccnx_tables.h"   /* PIT/FIB/CS = RFC 8569 フォワーダ三表(fwdtest 用) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>   /* porttest: 実 UDP + tap で C portal の送出を観測する */
#include <netinet/in.h>
#include <sys/time.h>
#include <unistd.h>

/* WIRE §6 の期待16進(隣接文字列リテラルはコンパイラが連結) */
static const char *V1_URI = "/area/A-2";
static const char *V1_HEX =
    "0100001F" "08000008"          /* 固定ヘッダ(Interest, PacketLen=0x1F, Hop=8) */
    "00010013"                     /* Message TLV(T_INTEREST, len 0x13) */
    "0000000F"                     /* Name TLV(len 0x0F) */
    "00010004" "61726561"          /* area */
    "00010003" "412D32";           /* A-2 */

static const char *V2_URI = "/task/rescue/zone/A-2/carriers/2";
static const char *V2_HEX =
    "01000042" "08000008"
    "00010036"
    "00000032"
    "00010004" "7461736B"          /* task */
    "00010006" "726573637565"      /* rescue */
    "00010004" "7A6F6E65"          /* zone */
    "00010003" "412D32"            /* A-2 */
    "00010008" "6361727269657273"  /* carriers */
    "00010001" "32";               /* 2 */

static const char *V3_URI = "/star/7/claim";
static const char *V3_PAYLOAD = "robot-3";
static const char *V3_HEX =
    "01010031" "00000008"          /* 固定ヘッダ(ContentObject, PacketLen=0x31) */
    "00020025"                     /* Message TLV(T_OBJECT, len 0x25) */
    "00000016"                     /* Name TLV(len 0x16) */
    "00010004" "73746172"          /* star */
    "00010001" "37"                /* 7 */
    "00010005" "636C61696D"        /* claim */
    "00010007" "726F626F742D33";   /* Payload "robot-3" */

/* =====================================================================
 * HEX ヘルパ
 * ===================================================================== */
static void bytes_to_hex(const uint8_t *buf, size_t len, char *out) {
    static const char *H = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = H[(buf[i] >> 4) & 0xF];
        out[i * 2 + 1] = H[buf[i] & 0xF];
    }
    out[len * 2] = '\0';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* HEX 文字列 -> バイト。返り値=バイト数 / 負値=不正。out は len/2 以上。 */
static int hex_to_bytes(const char *hex, uint8_t *out, size_t cap) {
    size_t n = strlen(hex);
    if (n % 2 != 0) return -1;
    size_t nb = n / 2;
    if (nb > cap) return -1;
    for (size_t i = 0; i < nb; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)nb;
}

static const char *kind_str(uint8_t pt) {
    switch (pt) {
        case CCNX_PT_INTEREST:       return "Interest";
        case CCNX_PT_CONTENT_OBJECT: return "ContentObject";
        case CCNX_PT_RETURN:         return "InterestReturn";
        default:                     return "Unknown";
    }
}

/* =====================================================================
 * サブコマンド
 * ===================================================================== */
static int build_msg(ccnx_msg_t *m, const char *uri, uint8_t pt, uint8_t rc,
                     const uint8_t *payload, uint16_t plen, uint8_t hop) {
    memset(m, 0, sizeof(*m));
    m->packet_type = pt;
    m->hop_limit = (pt == CCNX_PT_CONTENT_OBJECT) ? 0 : hop;
    m->return_code = (pt == CCNX_PT_RETURN) ? rc : 0;
    if (ccnx_name_from_uri(&m->name, uri) != 0) return -1;
    if (payload && plen > 0) {
        if (plen > CCNX_MAX_PAYLOAD) return -1;
        memcpy(m->payload, payload, plen);
        m->payload_len = plen;
    }
    return 0;
}

static int emit_encoded(const char *uri, uint8_t pt, uint8_t rc,
                        const uint8_t *payload, uint16_t plen,
                        const char *prefix_name) {
    ccnx_msg_t m;
    if (build_msg(&m, uri, pt, rc, payload, plen, CCNX_DEFAULT_HOP_LIMIT) != 0) {
        fprintf(stderr, "encode: bad uri/payload\n");
        return 1;
    }
    uint8_t buf[CCNX_MAX_PAYLOAD + 512];
    int len = ccnx_encode(&m, buf, sizeof(buf));
    if (len < 0) {
        fprintf(stderr, "encode: failed\n");
        return 1;
    }
    char hex[(CCNX_MAX_PAYLOAD + 512) * 2 + 1];
    bytes_to_hex(buf, (size_t)len, hex);
    if (prefix_name) printf("%s\t%s\n", prefix_name, hex);
    else printf("%s\n", hex);
    return 0;
}

static int cmd_vectors(void) {
    uint8_t p3[CCNX_MAX_PAYLOAD];
    int p3len = (int)strlen(V3_PAYLOAD);
    memcpy(p3, V3_PAYLOAD, (size_t)p3len);
    int rc = 0;
    rc |= emit_encoded(V1_URI, CCNX_PT_INTEREST, 0, NULL, 0, V1_URI);
    rc |= emit_encoded(V2_URI, CCNX_PT_INTEREST, 0, NULL, 0, V2_URI);
    rc |= emit_encoded(V3_URI, CCNX_PT_CONTENT_OBJECT, 0, p3, (uint16_t)p3len, V3_URI);
    return rc;
}

static int cmd_encode(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
            "usage: codec_test encode <uri> <I|C> [payloadHex]\n"
            "       codec_test encode <uri> R <returnCodeHex2> [payloadHex]\n");
        return 1;
    }
    const char *uri = argv[2];
    uint8_t pt;
    if (strcmp(argv[3], "I") == 0)      pt = CCNX_PT_INTEREST;
    else if (strcmp(argv[3], "C") == 0) pt = CCNX_PT_CONTENT_OBJECT;
    else if (strcmp(argv[3], "R") == 0) pt = CCNX_PT_RETURN;
    else { fprintf(stderr, "encode: type must be I, C or R\n"); return 1; }

    /* R は ReturnCode を必須引数として取る(RFC 8609 §3.2.3.3)。
     * payload の位置引数が 1 つ後ろにずれる。I/C の書式は不変。 */
    int payload_argi = 4;
    uint8_t rc = 0;
    if (pt == CCNX_PT_RETURN) {
        if (argc < 5) {
            fprintf(stderr,
                "usage: codec_test encode <uri> R <returnCodeHex2> [payloadHex]\n");
            return 1;
        }
        uint8_t rcb[1];
        if (hex_to_bytes(argv[4], rcb, sizeof(rcb)) != 1) {
            fprintf(stderr, "encode: returnCode must be exactly 2 hex digits\n");
            return 1;
        }
        rc = rcb[0];
        payload_argi = 5;
    }

    uint8_t payload[CCNX_MAX_PAYLOAD];
    uint16_t plen = 0;
    if (argc > payload_argi && argv[payload_argi][0] != '\0') {
        int n = hex_to_bytes(argv[payload_argi], payload, sizeof(payload));
        if (n < 0) { fprintf(stderr, "encode: bad payload hex\n"); return 1; }
        plen = (uint16_t)n;
    }
    return emit_encoded(uri, pt, rc, plen ? payload : NULL, plen, NULL);
}

static int cmd_decode(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: codec_test decode <HEX>\n");
        return 1;
    }
    size_t hexlen = strlen(argv[2]);
    uint8_t *buf = malloc(hexlen / 2 + 1);
    if (!buf) return 1;
    int n = hex_to_bytes(argv[2], buf, hexlen / 2 + 1);
    if (n < 0) { fprintf(stderr, "decode: bad hex\n"); free(buf); return 1; }

    ccnx_msg_t m;
    int dl = ccnx_decode(buf, (size_t)n, &m);
    free(buf);
    if (dl < 0) { fprintf(stderr, "decode: not valid 8609\n"); return 1; }

    char uri[CCNX_URI_MAX];
    if (ccnx_name_to_uri(&m.name, uri, sizeof(uri)) < 0) {
        fprintf(stderr, "decode: uri overflow\n");
        return 1;
    }
    char phex[CCNX_MAX_PAYLOAD * 2 + 1];
    if (m.payload_len > 0) bytes_to_hex(m.payload, m.payload_len, phex);
    else phex[0] = '\0';
    /* Interest / ContentObject は 3 フィールドのまま(既存 interop 契約)。
     * InterestReturn のときだけ ReturnCode を 4 フィールド目に足す。 */
    if (m.packet_type == CCNX_PT_RETURN)
        printf("%s\t%s\t%s\t%02X\n", kind_str(m.packet_type), uri, phex,
               m.return_code);
    else
        printf("%s\t%s\t%s\n", kind_str(m.packet_type), uri, phex);
    return 0;
}

/* =====================================================================
 * selftest
 * ===================================================================== */
static int encode_hex(const ccnx_msg_t *m, char *hexout, size_t cap) {
    uint8_t buf[CCNX_MAX_PAYLOAD + 512];
    int len = ccnx_encode(m, buf, sizeof(buf));
    if (len < 0) return -1;
    if ((size_t)(len * 2 + 1) > cap) return -1;
    bytes_to_hex(buf, (size_t)len, hexout);
    return len;
}

static int check_vector(const char *uri, uint8_t pt,
                        const char *payload, const char *expect_hex,
                        char *reason, size_t rcap) {
    ccnx_msg_t m;
    uint16_t plen = payload ? (uint16_t)strlen(payload) : 0;
    if (build_msg(&m, uri, pt, 0, (const uint8_t *)payload, plen,
                  CCNX_DEFAULT_HOP_LIMIT) != 0) {
        snprintf(reason, rcap, "build_msg failed for %s", uri);
        return -1;
    }
    char hex[(CCNX_MAX_PAYLOAD + 512) * 2 + 1];
    if (encode_hex(&m, hex, sizeof(hex)) < 0) {
        snprintf(reason, rcap, "encode failed for %s", uri);
        return -1;
    }
    if (strcmp(hex, expect_hex) != 0) {
        snprintf(reason, rcap, "vector %s mismatch\n  got : %s\n  want: %s",
                 uri, hex, expect_hex);
        return -1;
    }
    return 0;
}

/* msg 同値判定(name/種別/payload)。1=一致 */
static int msg_eq(const ccnx_msg_t *a, const ccnx_msg_t *b) {
    if (a->packet_type != b->packet_type) return 0;
    if (a->return_code != b->return_code) return 0;
    if (!ccnx_name_equal(&a->name, &b->name)) return 0;
    if (a->payload_len != b->payload_len) return 0;
    if (a->payload_len && memcmp(a->payload, b->payload, a->payload_len) != 0) return 0;
    return 1;
}

/* encode -> decode -> re-encode がバイト一致 & 復元 msg が同値 */
static int roundtrip(const ccnx_msg_t *m, char *reason, size_t rcap) {
    uint8_t b1[CCNX_MAX_PAYLOAD + 512];
    int l1 = ccnx_encode(m, b1, sizeof(b1));
    if (l1 < 0) { snprintf(reason, rcap, "encode1 failed"); return -1; }

    ccnx_msg_t m2;
    int dl = ccnx_decode(b1, (size_t)l1, &m2);
    if (dl < 0) { snprintf(reason, rcap, "decode failed"); return -1; }
    if (dl != l1) { snprintf(reason, rcap, "decode consumed %d != %d", dl, l1); return -1; }
    if (!msg_eq(m, &m2)) { snprintf(reason, rcap, "decoded msg differs"); return -1; }

    uint8_t b2[CCNX_MAX_PAYLOAD + 512];
    int l2 = ccnx_encode(&m2, b2, sizeof(b2));
    if (l2 < 0) { snprintf(reason, rcap, "encode2 failed"); return -1; }
    if (l2 != l1 || memcmp(b1, b2, (size_t)l1) != 0) {
        snprintf(reason, rcap, "re-encode byte mismatch");
        return -1;
    }
    return 0;
}

/* 自前 PRNG(POSIX rand_r 非依存で -std=c11 でも確実に使える。決定論的) */
static uint32_t prng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static void rand_msg(ccnx_msg_t *m, uint32_t *seed) {
    memset(m, 0, sizeof(*m));
    switch (prng(seed) % 3) {
        case 0:  m->packet_type = CCNX_PT_INTEREST;       break;
        case 1:  m->packet_type = CCNX_PT_CONTENT_OBJECT; break;
        default: m->packet_type = CCNX_PT_RETURN;         break;
    }
    m->hop_limit = (m->packet_type == CCNX_PT_CONTENT_OBJECT)
                       ? 0 : (uint8_t)(prng(seed) & 0xFF);
    /* ReturnCode は Interest Return のみ 0x01..0x09(RFC 8609 §3.2.3.3) */
    m->return_code = (m->packet_type == CCNX_PT_RETURN)
                         ? (uint8_t)(1 + prng(seed) % 9) : 0;
    /* 全 PacketType で名前必須(WIRE §7.2 E2)。空名前は FIB/prefix 登録専用。 */
    const int minseg = 1;
    int nseg = minseg + (int)(prng(seed) % (CCNX_MAX_NAME_SEGS + 1 - minseg));
    m->name.seg_count = (uint16_t)nseg;
    for (int i = 0; i < nseg; i++) {
        int sl = 1 + (int)(prng(seed) % 12);                /* 1..12 */
        m->name.seg_len[i] = (uint16_t)sl;
        for (int j = 0; j < sl; j++)
            m->name.seg[i][j] = (uint8_t)(prng(seed) & 0xFF);
    }
    int has_p = (int)(prng(seed) & 1);
    if (has_p) {
        int pl = (int)(prng(seed) % 64);
        m->payload_len = (uint16_t)pl;
        for (int j = 0; j < pl; j++)
            m->payload[j] = (uint8_t)(prng(seed) & 0xFF);
    }
}

/* encode が必ず失敗すべきケース。0=期待どおり拒否 / -1=誤って通った */
static int expect_encode_reject(const ccnx_msg_t *m) {
    uint8_t buf[CCNX_MAX_PAYLOAD + 512];
    return (ccnx_encode(m, buf, sizeof(buf)) < 0) ? 0 : -1;
}

/* フォワーダ(RFC 8569)三表の適合テスト。ccnx_tables.c を直接叩き、Go 側
 * portal_test.go(TestPITExpiredEntryAbsent / TestForwarderPITSymmetry)と
 * 同一の不変条件を機械判定する。集約可否の決定点は ccnx_pit_insert が返す
 * ccnx_pit_insert_res_t であり、ccnx.c の handle_interest はこの res に従って
 * 転送/抑制するだけなので、ここを固定すれば C↔Go のフォワーダ等価性が担保される。
 * 成功で 0、失敗で reason を書いて非 0。 */
static int fwd_conformance(char *reason, size_t cap) {
    ccnx_tables_t t;
    ccnx_pit_insert_res_t res;
    ccnx_name_t nx, ny, ne;

    if (ccnx_name_from_uri(&nx, "/x/1") != 0 ||
        ccnx_name_from_uri(&ny, "/y/1") != 0 ||
        ccnx_name_from_uri(&ne, "/exp/1") != 0) {
        snprintf(reason, cap, "fwd: name_from_uri failed"); return 1;
    }

    /* (1) 期限切れ PIT は不在扱い(RFC 8569 §2.4.2, 監査#2) */
    ccnx_tables_init(&t);
    /* now=1000 に対し expire=500 の失効エントリを作る */
    if (ccnx_pit_insert(&t, &ne, 42, /*expire*/500, /*now*/1000, 8, &res) < 0) {
        snprintf(reason, cap, "fwd(1): insert expired failed"); return 1;
    }
    if (ccnx_pit_find(&t, &ne, /*now*/1000) != -1) {
        snprintf(reason, cap, "fwd(1): expired PIT returned as live"); return 1;
    }
    if (ccnx_pit_expire(&t, 1000) != 1 || ccnx_tbl_pit_count(&t) != 0) {
        snprintf(reason, cap, "fwd(1): expired PIT not swept (count=%d)",
                 ccnx_tbl_pit_count(&t)); return 1;
    }
    /* 未失効(expire=2000)は find で見つかる */
    if (ccnx_pit_insert(&t, &ne, 42, 2000, 1000, 8, &res) < 0 ||
        ccnx_pit_find(&t, &ne, 1000) < 0) {
        snprintf(reason, cap, "fwd(1): live PIT not found"); return 1;
    }

    /* (2) ローカル express が §2.4.2 の高水位を記録し、同値 HopLimit の
     *     リモート Interest は集約(larger でない)、大 HopLimit は MUST forward
     *     (RFC 8569 §2.4.2, 監査#4 maxHopLimit) */
    ccnx_tables_init(&t);
    if (ccnx_pit_insert(&t, &nx, CCNX_LOCAL_REQUESTER, 5000, 1000, 8, &res) < 0 ||
        res.aggregated != 0 || res.larger_hop_limit != 0) {
        snprintf(reason, cap, "fwd(2): local express must be new (agg=%d larger=%d)",
                 res.aggregated, res.larger_hop_limit); return 1;
    }
    /* 同値 HopLimit=8 のリモート → 集約, larger でない(高水位=8 が効く) */
    if (ccnx_pit_insert(&t, &nx, 45001, 5000, 1000, 8, &res) < 0 ||
        res.aggregated != 1 || res.larger_hop_limit != 0) {
        snprintf(reason, cap,
                 "fwd(2): equal-HopLimit must aggregate w/o larger (agg=%d larger=%d)",
                 res.aggregated, res.larger_hop_limit); return 1;
    }
    /* 大 HopLimit=9 → larger_hop_limit=1(MUST forward) */
    if (ccnx_pit_insert(&t, &nx, 45002, 5000, 1000, 9, &res) < 0 ||
        res.aggregated != 1 || res.larger_hop_limit != 1) {
        snprintf(reason, cap,
                 "fwd(2): larger HopLimit must set larger_hop_limit (agg=%d larger=%d)",
                 res.aggregated, res.larger_hop_limit); return 1;
    }

    /* (3) 同一前ホップからの再送は retransmit=1(§2.4.2 MUST forward)。
     *     45001 は (2) で既に requester。高水位は 9 なので larger は立たない。 */
    if (ccnx_pit_insert(&t, &nx, 45001, 5000, 1000, 8, &res) < 0 ||
        res.retransmit != 1 || res.larger_hop_limit != 0) {
        snprintf(reason, cap, "fwd(3): retransmit must set retransmit only (rtx=%d larger=%d)",
                 res.retransmit, res.larger_hop_limit); return 1;
    }

    /* (4) 転送可能集合 = FIB 最長一致 nexthop − 到来面。唯一の nexthop が到来面の
     *     ときは「経路なし」で NO_ROUTE(RFC 8569 §10.3.1, 監査#4 has_route) */
    ccnx_tables_init(&t);
    if (ccnx_fib_add(&t, &ny, 45001) != 0) {
        snprintf(reason, cap, "fwd(4): fib_add failed"); return 1;
    }
    if (ccnx_fib_has_route(&t, &ny, /*exclude=arrival*/45001) != 0) {
        snprintf(reason, cap, "fwd(4): only nexthop==arrival must be NO_ROUTE"); return 1;
    }
    if (ccnx_fib_has_route(&t, &ny, /*exclude*/CCNX_LOCAL_REQUESTER) != 1) {
        snprintf(reason, cap, "fwd(4): route must exist when not excluding it"); return 1;
    }
    /* 2 本目の nexthop を足すと到来面を除いても転送先が残る */
    if (ccnx_fib_add(&t, &ny, 45002) != 0 ||
        ccnx_fib_has_route(&t, &ny, 45001) != 1) {
        snprintf(reason, cap, "fwd(4): second nexthop must survive split-horizon"); return 1;
    }

    /* (5) 前ホップ検査の材料: FIB に nexthop として在るポートだけ is_nexthop=1
     *     (RFC 8569 §2.4.5(1)。handle_content の CO 到来面検証に使う) */
    if (ccnx_fib_is_nexthop(&t, 45002) != 1 ||
        ccnx_fib_is_nexthop(&t, 9999) != 0) {
        snprintf(reason, cap, "fwd(5): is_nexthop wrong"); return 1;
    }

    return 0;
}

/* fwdtest サブコマンド: フォワーダ適合のみを単独実行(interop Section F 用)。 */
static int cmd_fwdtest(void) {
    char reason[512];
    if (fwd_conformance(reason, sizeof(reason)) != 0) {
        printf("FAIL: %s\n", reason); return 1;
    }
    printf("PASS\n");
    return 0;
}

/* ---- porttest: C portal(ccnx.c)を実 UDP + tap で駆動し「実際にワイヤへ出た
 *      datagram」を観測する統合テスト。表フラグ(fwdtest)ではなく Portal が
 *      その判定に従って転送/抑止/NO_ROUTE/前ホップ処理を行ったかを検査する。
 *      Go portal_test.go(TestPortalForwardSendBehavior / TestPortalPrevHopCO)と
 *      同一の観測点(loopback + tap)・同一の不変条件。 */

static int udp_open(uint16_t *port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    socklen_t sl = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &sl) < 0) { close(fd); return -1; }
    *port = ntohs(a.sin_port);
    struct timeval tv = { 0, 60000 }; /* 60ms recv timeout */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static void udp_send_to(int fd, uint16_t dport, const uint8_t *buf, size_t n) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(dport);
    (void)sendto(fd, buf, n, 0, (struct sockaddr *)&a, sizeof a);
}

static void udp_drain(int fd) {
    uint8_t b[2048];
    struct timeval tv = { 0, 3000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (recv(fd, b, sizeof b, 0) > 0) { /* discard */ }
    tv.tv_usec = 60000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* あるソケット(fd)に溜まった datagram を全部読み、PacketType 別に数える。
 * tap でも実 face(nexthop / 到来面)でも同じに使える。
 * n_int/n_co/n_ret に件数、first_hop に最初の Interest の HopLimit、
 * first_ret_code に最初の Return の ReturnCode を返す。総数を返す。 */
static int sock_tally(int fd, int *n_int, int *n_co, int *n_ret,
                      int *first_hop, int *first_ret_code) {
    *n_int = *n_co = *n_ret = 0; *first_hop = -1; *first_ret_code = -1;
    uint8_t b[2048];
    int total = 0;
    for (;;) {
        ssize_t r = recv(fd, b, sizeof b, 0);
        if (r <= 0) break;
        ccnx_msg_t m;
        if (ccnx_decode(b, (size_t)r, &m) < 0) continue;
        total++;
        if (m.packet_type == CCNX_PT_INTEREST) {
            if (*n_int == 0) *first_hop = m.hop_limit;
            (*n_int)++;
        } else if (m.packet_type == CCNX_PT_CONTENT_OBJECT) {
            (*n_co)++;
        } else if (m.packet_type == CCNX_PT_RETURN) {
            if (*n_ret == 0) *first_ret_code = m.return_code;
            (*n_ret)++;
        }
    }
    return total;
}

typedef struct { int n; char uris[8][CCNX_URI_MAX]; int overheard[8]; } co_events_t;
static void porttest_co_cb(ccnx_portal_t *p, const ccnx_name_t *name,
                           const uint8_t *pl, uint16_t pn, int oh, void *ctx) {
    (void)p; (void)pl; (void)pn;
    co_events_t *e = ctx;
    if (e->n < 8) {
        ccnx_name_to_uri(name, e->uris[e->n], CCNX_URI_MAX);
        e->overheard[e->n] = oh;
        e->n++;
    }
}

/* Interest を peer(fd)から portal の listen ポートへ送り、tick で処理させる。
 * 送信前に観測ソケット(sender 自身 / nexthop 面 / tap)の残留を掃除し、各ケースが
 * 自分の出力だけを観測するようにする。宛先検査は呼び出し側が sock_tally で行う。 */
static void port_send_interest(int fd, uint16_t pport, ccnx_portal_t *p,
                               const char *uri, uint8_t hop, int nh_fd, int catcher) {
    udp_drain(fd); udp_drain(nh_fd); udp_drain(catcher);
    ccnx_msg_t m;
    memset(&m, 0, sizeof m);
    m.packet_type = CCNX_PT_INTEREST;
    m.hop_limit = hop;
    ccnx_name_from_uri(&m.name, uri);
    uint8_t buf[CCNX_MAX_PAYLOAD + 512];
    int n = ccnx_encode(&m, buf, sizeof buf);
    if (n < 0) return;
    udp_send_to(fd, pport, buf, (size_t)n);
    for (int i = 0; i < 3; i++) (void)ccnx_tick(p, 20);
}

static void port_send_co(int fd, uint16_t pport, ccnx_portal_t *p,
                         const char *uri, const char *payload) {
    ccnx_msg_t m;
    memset(&m, 0, sizeof m);
    m.packet_type = CCNX_PT_CONTENT_OBJECT;
    ccnx_name_from_uri(&m.name, uri);
    m.payload_len = (uint16_t)strlen(payload);
    memcpy(m.payload, payload, m.payload_len);
    uint8_t buf[CCNX_MAX_PAYLOAD + 512];
    int n = ccnx_encode(&m, buf, sizeof buf);
    if (n < 0) return;
    udp_send_to(fd, pport, buf, (size_t)n);
    for (int i = 0; i < 3; i++) (void)ccnx_tick(p, 20);
}

static int port_conformance(char *reason, size_t cap) {
    uint16_t tap_port, nh_port, nh2_port, peer1_port, peer2_port;
    int catcher = udp_open(&tap_port);
    int nh      = udp_open(&nh_port);
    int nh2     = udp_open(&nh2_port);
    int peer1   = udp_open(&peer1_port);
    int peer2   = udp_open(&peer2_port);
    if (catcher < 0 || nh < 0 || nh2 < 0 || peer1 < 0 || peer2 < 0) {
        snprintf(reason, cap, "port: socket open failed"); return 1;
    }
    int rc = 1;
    /* nexthop 面 / 到来面 / tap をそれぞれ独立に読む。宛先の同一性まで検査するため
     * tap(本文のみ)ではなく実 face のソケットを直接読む。 */
    int a_ni, a_co, a_nr, a_fh, a_frc; /* nexthop 面(nh) */
    int b_ni, b_co, b_nr, b_fh, b_frc; /* 到来面(peer1) */
    int t_tot, d1, d2, d3, d4, d5;     /* tap 総数 + 捨て out 引数 */

    ccnx_portal_t *p = ccnx_portal_open("127.0.0.1", 0, tap_port);
    if (!p) { snprintf(reason, cap, "port: portal_open failed"); goto out; }
    uint16_t pport = ccnx_portal_port(p);
    if (ccnx_fib_add_route(p, "/x", nh_port) != 0) {
        snprintf(reason, cap, "port: fib_add_route failed"); goto out2;
    }

    /* (1) 新規 Interest → nexthop 面へ 1 回・HopLimit 7、到来面へは出さない
     *     (split-horizon)。tap 総数=1。 */
    port_send_interest(peer1, pport, p, "/x/1", 8, nh, catcher);
    sock_tally(nh, &a_ni, &a_co, &a_nr, &a_fh, &a_frc);
    sock_tally(peer1, &b_ni, &b_co, &b_nr, &b_fh, &b_frc);
    t_tot = sock_tally(catcher, &d1, &d2, &d3, &d4, &d5);
    if (a_ni != 1 || a_fh != 7) {
        snprintf(reason, cap, "port(1): nexthop must get 1 Interest at hop7 (int=%d hop=%d)", a_ni, a_fh); goto out2;
    }
    if (b_ni + b_co + b_nr != 0) {
        snprintf(reason, cap, "port(1): arrival face must NOT get the forward (got=%d)", b_ni + b_co + b_nr); goto out2;
    }
    if (t_tot != 1) {
        snprintf(reason, cap, "port(1): exactly 1 datagram on the wire (tap=%d)", t_tot); goto out2;
    }
    /* (2) 別前ホップからの同名同 HopLimit → 集約・抑止(nexthop 面も tap も 0) */
    port_send_interest(peer2, pport, p, "/x/1", 8, nh, catcher);
    sock_tally(nh, &a_ni, &a_co, &a_nr, &a_fh, &a_frc);
    t_tot = sock_tally(catcher, &d1, &d2, &d3, &d4, &d5);
    if (a_ni + a_co + a_nr != 0 || t_tot != 0) {
        snprintf(reason, cap, "port(2): aggregated Interest must be suppressed (nh=%d tap=%d)",
                 a_ni + a_co + a_nr, t_tot); goto out2;
    }
    /* (3) 同一前ホップ(peer1)からの再送 → nexthop 面へ MUST forward */
    port_send_interest(peer1, pport, p, "/x/1", 8, nh, catcher);
    sock_tally(nh, &a_ni, &a_co, &a_nr, &a_fh, &a_frc);
    if (a_ni != 1) {
        snprintf(reason, cap, "port(3): retransmit must re-forward to nexthop (int=%d)", a_ni); goto out2;
    }
    /* (4) 大 HopLimit=9 → nexthop 面へ MUST forward・HopLimit 8 */
    port_send_interest(peer2, pport, p, "/x/1", 9, nh, catcher);
    sock_tally(nh, &a_ni, &a_co, &a_nr, &a_fh, &a_frc);
    if (a_ni != 1 || a_fh != 8) {
        snprintf(reason, cap, "port(4): larger HopLimit must re-forward to nexthop at hop8 (int=%d hop=%d)", a_ni, a_fh); goto out2;
    }
    /* (5) HopLimit=1 (経路あり) → nexthop 面も到来面も tap も 0。加えて
     *     §10 行12: 転送できない Interest は PIT を作らない。送出有無だけでなく
     *     ccnx_pit_count() の増分 0 を検査する(portal の PIT 残置を機械判定)。 */
    int pc_before5 = ccnx_pit_count(p);
    port_send_interest(peer1, pport, p, "/x/hop1", 1, nh, catcher);
    sock_tally(nh, &a_ni, &a_co, &a_nr, &a_fh, &a_frc);
    sock_tally(peer1, &b_ni, &b_co, &b_nr, &b_fh, &b_frc);
    t_tot = sock_tally(catcher, &d1, &d2, &d3, &d4, &d5);
    if (a_ni + a_co + a_nr != 0 || b_ni + b_co + b_nr != 0 || t_tot != 0) {
        snprintf(reason, cap, "port(5): HopLimit=1 must be dropped silently (nh=%d arr=%d tap=%d)",
                 a_ni + a_co + a_nr, b_ni + b_co + b_nr, t_tot); goto out2;
    }
    if (ccnx_pit_count(p) != pc_before5) {
        snprintf(reason, cap, "port(5): HopLimit=1 must not create a PIT entry (行12。%d->%d)",
                 pc_before5, ccnx_pit_count(p)); goto out2;
    }
    /* (6) 唯一の nexthop == 到来面 → 到来面へ NO_ROUTE Return(code 0x01)、
     *     nexthop 面(/x の nh)へは出さない。tap 総数=1。加えて §10 行8:
     *     NO_ROUTE 返送経路は PIT を残さない → pit_count 増分 0 を検査する。 */
    if (ccnx_fib_add_route(p, "/y", peer1_port) != 0) {
        snprintf(reason, cap, "port(6): fib_add_route /y failed"); goto out2;
    }
    int pc_before6 = ccnx_pit_count(p);
    port_send_interest(peer1, pport, p, "/y/1", 8, nh, catcher);
    sock_tally(peer1, &b_ni, &b_co, &b_nr, &b_fh, &b_frc);
    sock_tally(nh, &a_ni, &a_co, &a_nr, &a_fh, &a_frc);
    t_tot = sock_tally(catcher, &d1, &d2, &d3, &d4, &d5);
    if (b_nr != 1 || b_ni != 0 || b_co != 0 || b_frc != CCNX_T_RETURN_NO_ROUTE) {
        snprintf(reason, cap, "port(6): arrival face must get 1 NO_ROUTE Return (ret=%d code=%d)", b_nr, b_frc); goto out2;
    }
    if (ccnx_pit_count(p) != pc_before6) {
        snprintf(reason, cap, "port(6): NO_ROUTE must not leave a PIT entry (行8。%d->%d)",
                 pc_before6, ccnx_pit_count(p)); goto out2;
    }
    if (a_ni + a_co + a_nr != 0 || t_tot != 1) {
        snprintf(reason, cap, "port(6): nothing to nexthop, 1 on wire (nh=%d tap=%d)",
                 a_ni + a_co + a_nr, t_tot); goto out2;
    }

    /* (7) tap 契約: 2 nexthop への転送は線上 2 datagram = tap 2 複製(宛先ごと)。
     *     C ccnx_face_send は送出ごとに tap するので N nexthop=N 複製。Go も
     *     宛先ごと tap に統一済み。1 複製に退行すれば trace が両実装で食い違う。 */
    {
        int c_ni, c_co, c_nr, c_fh, c_frc; /* nh2 面 */
        if (ccnx_fib_add_route(p, "/z", nh_port) != 0 ||
            ccnx_fib_add_route(p, "/z", nh2_port) != 0) {
            snprintf(reason, cap, "port(7): fib_add_route /z failed"); goto out2;
        }
        udp_drain(nh); udp_drain(nh2); udp_drain(catcher);
        port_send_interest(peer1, pport, p, "/z/1", 8, nh, catcher);
        sock_tally(nh,  &a_ni, &a_co, &a_nr, &a_fh, &a_frc);
        sock_tally(nh2, &c_ni, &c_co, &c_nr, &c_fh, &c_frc);
        t_tot = sock_tally(catcher, &d1, &d2, &d3, &d4, &d5);
        if (a_ni != 1 || c_ni != 1) {
            snprintf(reason, cap, "port(7): both nexthops must get the forward (nh=%d nh2=%d)", a_ni, c_ni); goto out2;
        }
        if (t_tot != 2) {
            snprintf(reason, cap, "port(7): 2 forwards = 2 tap copies (per-destination), tap=%d", t_tot); goto out2;
        }
    }

    /* (a)(b) 前ホップ CO 処理: trusted は CS 格納+PIT 充足(overheard=0)、
     *        untrusted は CS 非格納だが overheard=1 で配送(C/Go 対称)。 */
    {
        uint16_t trusted_port, stranger_port;
        int trusted = udp_open(&trusted_port);
        int stranger = udp_open(&stranger_port);
        if (trusted < 0 || stranger < 0) {
            snprintf(reason, cap, "port(ab): socket open failed"); goto out2;
        }
        ccnx_portal_t *q = ccnx_portal_open("127.0.0.1", 0, 0);
        if (!q) { close(trusted); close(stranger);
                  snprintf(reason, cap, "port(ab): portal q open failed"); goto out2; }
        uint16_t qport = ccnx_portal_port(q);
        co_events_t ev; ev.n = 0;
        ccnx_set_default_on_content(q, porttest_co_cb, &ev);
        ccnx_fib_add_route(q, "/svc", trusted_port);
        /* q が /svc/a を express(ローカル PIT を作る。Interest は trusted へ飛ぶ) */
        ccnx_express_interest(q, "/svc/a", NULL, 0, 8, 0, NULL, NULL);
        for (int i = 0; i < 3; i++) (void)ccnx_tick(q, 20);
        /* (a) trusted からの CO /svc/a → CS 格納 + overheard=0 */
        port_send_co(trusted, qport, q, "/svc/a", "answer");
        if (ccnx_cs_count(q) != 1) {
            snprintf(reason, cap, "port(a): trusted CO must be cached (cs=%d)", ccnx_cs_count(q));
            close(trusted); close(stranger); ccnx_portal_close(q); goto out2;
        }
        if (ev.n != 1 || strcmp(ev.uris[0], "/svc/a") != 0 || ev.overheard[0] != 0) {
            snprintf(reason, cap, "port(a): trusted CO must deliver overheard=0 (n=%d oh=%d)",
                     ev.n, ev.n ? ev.overheard[0] : -1);
            close(trusted); close(stranger); ccnx_portal_close(q); goto out2;
        }
        /* (b) stranger(非 nexthop)からの CO /star/9 → CS 非格納・overheard=1 */
        port_send_co(stranger, qport, q, "/star/9", "claim");
        if (ccnx_cs_count(q) != 1) {
            snprintf(reason, cap, "port(b): untrusted CO must NOT be cached (cs=%d)", ccnx_cs_count(q));
            close(trusted); close(stranger); ccnx_portal_close(q); goto out2;
        }
        if (ev.n != 2 || strcmp(ev.uris[1], "/star/9") != 0 || ev.overheard[1] != 1) {
            snprintf(reason, cap, "port(b): untrusted CO must deliver overheard=1 (n=%d oh=%d)",
                     ev.n, ev.n > 1 ? ev.overheard[1] : -1);
            close(trusted); close(stranger); ccnx_portal_close(q); goto out2;
        }
        close(trusted); close(stranger); ccnx_portal_close(q);
    }

    rc = 0;
out2:
    if (p) ccnx_portal_close(p);
out:
    if (catcher >= 0) close(catcher);
    if (nh >= 0) close(nh);
    if (nh2 >= 0) close(nh2);
    if (peer1 >= 0) close(peer1);
    if (peer2 >= 0) close(peer2);
    return rc;
}

/* porttest サブコマンド: C portal 統合(実 UDP + tap)を単独実行。 */
static int cmd_porttest(void) {
    char reason[512];
    if (port_conformance(reason, sizeof(reason)) != 0) {
        printf("FAIL: %s\n", reason); return 1;
    }
    printf("PASS\n");
    return 0;
}

static int cmd_selftest(void) {
    char reason[2048];

    /* 1) WIRE §6 の 3 ベクタ厳密一致 */
    if (check_vector(V1_URI, CCNX_PT_INTEREST, NULL, V1_HEX,
                     reason, sizeof(reason)) != 0) {
        printf("FAIL: %s\n", reason); return 1;
    }
    if (check_vector(V2_URI, CCNX_PT_INTEREST, NULL, V2_HEX,
                     reason, sizeof(reason)) != 0) {
        printf("FAIL: %s\n", reason); return 1;
    }
    if (check_vector(V3_URI, CCNX_PT_CONTENT_OBJECT, V3_PAYLOAD, V3_HEX,
                     reason, sizeof(reason)) != 0) {
        printf("FAIL: %s\n", reason); return 1;
    }

    /* 2) 境界: 空名前 / 最大セグメント長 / 最大ペイロード */
    ccnx_msg_t bm;

    /* 空名前 Interest(/)は encode 拒否
     * RFC 8569 §2.1 "An Interest MUST have a Name" /
     * RFC 8609 §3.6.1(先頭 name segment に零長は許されない) */
    memset(&bm, 0, sizeof(bm));
    bm.packet_type = CCNX_PT_INTEREST;
    bm.hop_limit = CCNX_DEFAULT_HOP_LIMIT;
    bm.name.seg_count = 0;
    if (expect_encode_reject(&bm) != 0) {
        printf("FAIL: empty-name Interest must be rejected by encode\n"); return 1;
    }
    /* 空名前 Interest Return も同様に拒否 */
    bm.packet_type = CCNX_PT_RETURN;
    bm.return_code = CCNX_T_RETURN_NO_ROUTE;
    if (expect_encode_reject(&bm) != 0) {
        printf("FAIL: empty-name InterestReturn must be rejected by encode\n"); return 1;
    }
    /* 空名前 ContentObject も encode 拒否(WIRE §7.2 E2)。長さ 0 の T_NAME を
     * 持つ CO は nameless CO ではない(nameless は Name TLV 自体を省く)。 */
    memset(&bm, 0, sizeof(bm));
    bm.packet_type = CCNX_PT_CONTENT_OBJECT;
    bm.name.seg_count = 0;
    if (expect_encode_reject(&bm) != 0) {
        printf("FAIL: empty-name ContentObject must be rejected by encode\n"); return 1;
    }
    /* decode 側の受理集合は不変(WIRE §7.4 A5): 空 Name TLV の CO は受理する */
    {
        uint8_t db[64];
        int dn = hex_to_bytes("01010010000000080002000400000000", db, sizeof(db));
        ccnx_msg_t dm;
        if (dn < 0 || ccnx_decode(db, (size_t)dn, &dm) < 0 || dm.name.seg_count != 0) {
            printf("FAIL: empty-name CO datagram must still decode\n"); return 1;
        }
    }

    /* Name 内の T_PAD(0x0FFE)は encode / decode とも拒否
     * (RFC 8609 §3.3.1 / WIRE §7.2 E8, §7.3 D14) */
    memset(&bm, 0, sizeof(bm));
    bm.packet_type = CCNX_PT_INTEREST;
    bm.hop_limit = CCNX_DEFAULT_HOP_LIMIT;
    bm.name.seg_count = 1;
    bm.name.seg_type[0] = CCNX_T_PAD;
    bm.name.seg_len[0] = 1;
    bm.name.seg[0][0] = 'x';
    if (expect_encode_reject(&bm) != 0) {
        printf("FAIL: T_PAD name segment must be rejected by encode\n"); return 1;
    }
    {
        uint8_t db[64];
        int dn = hex_to_bytes("010000150800000800010009000000050FFE000178", db, sizeof(db));
        ccnx_msg_t dm;
        if (dn < 0 || ccnx_decode(db, (size_t)dn, &dm) >= 0) {
            printf("FAIL: T_PAD name segment must be rejected by decode\n"); return 1;
        }
    }

    /* Message TLV より前のトップレベル TLV は文法違反(RFC 8609 §3.1 /
     * WIRE §7.3 D12)。同じ TLV でも Message の後ろならスキップして受理する。 */
    {
        uint8_t db[128];
        ccnx_msg_t dm;
        /* V1 の固定ヘッダ直後に 00030000(空 Validation 風)を挿入、PacketLen+4 */
        int dn = hex_to_bytes(
            "010000230800000800030000000100130000000F000100046172656100010003412D32",
            db, sizeof(db));
        if (dn < 0 || ccnx_decode(db, (size_t)dn, &dm) >= 0) {
            printf("FAIL: TLV before Message TLV must be rejected by decode\n"); return 1;
        }
        /* 同じ 00030000 を末尾に置いた場合は受理(前方互換, WIRE §7.4 A1) */
        dn = hex_to_bytes(
            "010000230800000800010013" "0000000F000100046172656100010003412D32" "00030000",
            db, sizeof(db));
        if (dn < 0 || ccnx_decode(db, (size_t)dn, &dm) < 0) {
            printf("FAIL: TLV after Message TLV must stay accepted by decode\n"); return 1;
        }
    }

    /* PacketType と Message TLV 型の不整合は拒否(WIRE §7.3 D13)。
     * PT_RETURN + T_OBJECT: Interest Return の本文は T_INTEREST でなければ
     * ならない(RFC 8609 §3.2.3)。 */
    {
        uint8_t db[128];
        int dn = hex_to_bytes(V3_HEX, db, sizeof(db));
        ccnx_msg_t dm;
        if (dn < 0) { printf("FAIL: V3 hex broken\n"); return 1; }
        db[1] = CCNX_PT_RETURN;                 /* PT_RETURN + T_OBJECT 本文 */
        if (ccnx_decode(db, (size_t)dn, &dm) >= 0) {
            printf("FAIL: PT_RETURN + T_OBJECT body must be rejected by decode\n");
            return 1;
        }
        db[1] = CCNX_PT_INTEREST;               /* PT_INTEREST + T_OBJECT 本文 */
        if (ccnx_decode(db, (size_t)dn, &dm) >= 0) {
            printf("FAIL: PT_INTEREST + T_OBJECT body must be rejected by decode\n");
            return 1;
        }
    }

    /* 未知 PacketType(0x7F)は encode 拒否(RFC 8609 §3.2 / §4.1) */
    memset(&bm, 0, sizeof(bm));
    bm.packet_type = 0x7F;
    bm.name.seg_count = 1;
    bm.name.seg_len[0] = 1;
    bm.name.seg[0][0] = 'x';
    if (expect_encode_reject(&bm) != 0) {
        printf("FAIL: packet_type 0x7F must be rejected by encode\n"); return 1;
    }

    /* 最大セグメント数(CCNX_MAX_NAME_SEGS)round-trip */
    memset(&bm, 0, sizeof(bm));
    bm.packet_type = CCNX_PT_INTEREST;
    bm.hop_limit = CCNX_DEFAULT_HOP_LIMIT;
    bm.name.seg_count = CCNX_MAX_NAME_SEGS;
    for (int i = 0; i < CCNX_MAX_NAME_SEGS; i++) {
        bm.name.seg_len[i] = 16;
        for (int j = 0; j < 16; j++)
            bm.name.seg[i][j] = (uint8_t)('a' + ((i + j) % 26));
    }
    if (roundtrip(&bm, reason, sizeof(reason)) != 0) {
        printf("FAIL: max-segs: %s\n", reason); return 1;
    }

    /* セグメント数超過(17)は encode 拒否 */
    bm.name.seg_count = CCNX_MAX_NAME_SEGS + 1;
    if (expect_encode_reject(&bm) != 0) {
        printf("FAIL: seg_count %d must be rejected by encode\n",
               CCNX_MAX_NAME_SEGS + 1); return 1;
    }

    /* datagram 上限超過(16 セグ x 256B = 4176B > CCNX_MAX_DATAGRAM)は encode 拒否 */
    memset(&bm, 0, sizeof(bm));
    bm.packet_type = CCNX_PT_INTEREST;
    bm.hop_limit = CCNX_DEFAULT_HOP_LIMIT;
    bm.name.seg_count = CCNX_MAX_NAME_SEGS;
    for (int i = 0; i < CCNX_MAX_NAME_SEGS; i++) {
        bm.name.seg_len[i] = CCNX_MAX_SEG_LEN;
        for (int j = 0; j < CCNX_MAX_SEG_LEN; j++)
            bm.name.seg[i][j] = (uint8_t)(j & 0xFF);
    }
    if (expect_encode_reject(&bm) != 0) {
        printf("FAIL: over-%d datagram must be rejected by encode\n",
               CCNX_MAX_DATAGRAM); return 1;
    }

    /* セグメント長超過(257)は encode 拒否 */
    memset(&bm, 0, sizeof(bm));
    bm.packet_type = CCNX_PT_INTEREST;
    bm.hop_limit = CCNX_DEFAULT_HOP_LIMIT;
    bm.name.seg_count = 1;
    bm.name.seg_len[0] = CCNX_MAX_SEG_LEN + 1;
    if (expect_encode_reject(&bm) != 0) {
        printf("FAIL: seg_len %d must be rejected by encode\n",
               CCNX_MAX_SEG_LEN + 1); return 1;
    }

    /* ペイロード長超過(1401)は encode 拒否 */
    memset(&bm, 0, sizeof(bm));
    bm.packet_type = CCNX_PT_CONTENT_OBJECT;
    bm.name.seg_count = 1;
    bm.name.seg_len[0] = 3;
    memcpy(bm.name.seg[0], "big", 3);
    bm.payload_len = CCNX_MAX_PAYLOAD + 1;
    if (expect_encode_reject(&bm) != 0) {
        printf("FAIL: payload_len %d must be rejected by encode\n",
               CCNX_MAX_PAYLOAD + 1); return 1;
    }

    /* 最大セグメント長(CCNX_MAX_SEG_LEN)1 セグメント */
    memset(&bm, 0, sizeof(bm));
    bm.packet_type = CCNX_PT_INTEREST;
    bm.hop_limit = CCNX_DEFAULT_HOP_LIMIT;
    bm.name.seg_count = 1;
    bm.name.seg_len[0] = CCNX_MAX_SEG_LEN;
    for (int j = 0; j < CCNX_MAX_SEG_LEN; j++) bm.name.seg[0][j] = (uint8_t)(j & 0xFF);
    if (roundtrip(&bm, reason, sizeof(reason)) != 0) {
        printf("FAIL: max-seg: %s\n", reason); return 1;
    }

    /* 最大ペイロード(CCNX_MAX_PAYLOAD)ContentObject */
    memset(&bm, 0, sizeof(bm));
    bm.packet_type = CCNX_PT_CONTENT_OBJECT;
    bm.name.seg_count = 1;
    bm.name.seg_len[0] = 3;
    memcpy(bm.name.seg[0], "max", 3);
    bm.payload_len = CCNX_MAX_PAYLOAD;
    for (int j = 0; j < CCNX_MAX_PAYLOAD; j++) bm.payload[j] = (uint8_t)(j & 0xFF);
    if (roundtrip(&bm, reason, sizeof(reason)) != 0) {
        printf("FAIL: max-payload: %s\n", reason); return 1;
    }

    /* 3) Interest Return: 全 ReturnCode(0x01..0x09, RFC 8609 §3.2.3.3 Table 2)
     *    round-trip。固定ヘッダ octet 5 に載り、decode で同値に戻ること。 */
    for (int code = CCNX_T_RETURN_NO_ROUTE;
         code <= CCNX_T_RETURN_MALFORMED_INTEREST; code++) {
        memset(&bm, 0, sizeof(bm));
        bm.packet_type = CCNX_PT_RETURN;
        bm.hop_limit = CCNX_DEFAULT_HOP_LIMIT;
        bm.return_code = (uint8_t)code;
        if (ccnx_name_from_uri(&bm.name, "/task/rescue/zone/A-2") != 0) {
            printf("FAIL: return-code 0x%02X: bad uri\n", code); return 1;
        }
        bm.payload_len = 4;
        memcpy(bm.payload, "\x01\x02\x03\x04", 4);
        if (roundtrip(&bm, reason, sizeof(reason)) != 0) {
            printf("FAIL: return-code 0x%02X: %s\n", code, reason); return 1;
        }
        /* ワイヤ上の位置と値を直接確認(既存 Return の 0x01 と互換であること) */
        uint8_t rb[CCNX_MAX_PAYLOAD + 512];
        int rl = ccnx_encode(&bm, rb, sizeof(rb));
        if (rl < 0 || rb[1] != CCNX_PT_RETURN || rb[5] != (uint8_t)code) {
            printf("FAIL: return-code 0x%02X: wire octet5 mismatch\n", code);
            return 1;
        }
        ccnx_msg_t rm;
        if (ccnx_decode(rb, (size_t)rl, &rm) < 0 || rm.return_code != (uint8_t)code) {
            printf("FAIL: return-code 0x%02X: decode lost ReturnCode\n", code);
            return 1;
        }
    }

    /* Interest / ContentObject では return_code が必ず 0 になること */
    memset(&bm, 0, sizeof(bm));
    bm.packet_type = CCNX_PT_INTEREST;
    bm.hop_limit = CCNX_DEFAULT_HOP_LIMIT;
    bm.return_code = 0x09;                 /* 立てても無視され、ワイヤは Reserved=0 */
    if (ccnx_name_from_uri(&bm.name, "/area/A-2") != 0) {
        printf("FAIL: return-code-zero: bad uri\n"); return 1;
    }
    {
        uint8_t rb[CCNX_MAX_PAYLOAD + 512];
        int rl = ccnx_encode(&bm, rb, sizeof(rb));
        if (rl < 0 || rb[5] != 0x00) {
            printf("FAIL: Interest octet5 must stay Reserved=0\n"); return 1;
        }
        ccnx_msg_t rm;
        if (ccnx_decode(rb, (size_t)rl, &rm) < 0 || rm.return_code != 0) {
            printf("FAIL: Interest return_code must decode as 0\n"); return 1;
        }
    }

    /* 4) ランダム 1000 件 round-trip */
    uint32_t seed = 0x2831C283u;
    for (int k = 0; k < 1000; k++) {
        ccnx_msg_t m;
        rand_msg(&m, &seed);
        if (roundtrip(&m, reason, sizeof(reason)) != 0) {
            printf("FAIL: random #%d: %s\n", k, reason);
            return 1;
        }
    }

    /* 5) フォワーダ(RFC 8569 三表)適合: 期限切れ PIT・maxHopLimit 高水位・
     *    larger/retransmit 例外・NO_ROUTE split-horizon・前ホップ検査。
     *    Go portal_test.go と同一不変条件(C↔Go フォワーダ等価性の機械判定)。 */
    if (fwd_conformance(reason, sizeof(reason)) != 0) {
        printf("FAIL: %s\n", reason); return 1;
    }

    printf("PASS\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: codec_test <vectors|encode|decode|selftest|fwdtest|porttest> ...\n");
        return 2;
    }
    if (strcmp(argv[1], "vectors") == 0)  return cmd_vectors();
    if (strcmp(argv[1], "encode") == 0)   return cmd_encode(argc, argv);
    if (strcmp(argv[1], "decode") == 0)   return cmd_decode(argc, argv);
    if (strcmp(argv[1], "selftest") == 0) return cmd_selftest();
    if (strcmp(argv[1], "fwdtest") == 0)  return cmd_fwdtest();
    if (strcmp(argv[1], "porttest") == 0) return cmd_porttest();
    fprintf(stderr, "unknown subcommand: %s\n", argv[1]);
    return 2;
}
