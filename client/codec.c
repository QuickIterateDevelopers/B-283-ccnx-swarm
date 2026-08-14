/*
 * codec.c - CCNx (RFC 8609) TLV wire-format encoder/decoder
 *
 * B-283 A層(libccnx.a)。docs/WIRE.md のオクテット配置に厳密一致する。
 * ドメイン(ロボ/救助/星)を一切知らない。ネットワークバイトオーダ(BE)厳守、
 * 全 TLV は Type(2)|Length(2)|Value。境界チェックを全経路で行う。
 *
 * 参照: RFC 8609 / docs/WIRE.md(単一の真実)
 */
#include "codec.h"
#include <string.h>

/* --- big-endian 2 octet helpers (WIRE §0: 全整数は BE) --- */
static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}
static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}


/* =====================================================================
 * 名前操作
 * ===================================================================== */
void ccnx_name_clear(ccnx_name_t *n) {
    if (!n) return;
    n->seg_count = 0;
}

int ccnx_name_append(ccnx_name_t *n, const char *seg) {
    if (!n || !seg) return -1;
    size_t len = strlen(seg);
    if (len == 0) return -1;                 /* 空セグメント禁止(WIRE §3) */
    if (len > CCNX_MAX_SEG_LEN) return -1;
    if (n->seg_count >= CCNX_MAX_NAME_SEGS) return -1;
    uint16_t i = n->seg_count;
    memcpy(n->seg[i], seg, len);
    n->seg_len[i] = (uint16_t)len;
    n->seg_type[i] = CCNX_T_NAMESEG;
    n->seg_count++;
    return 0;
}

/* URI の 1 セグメント "0xHHHH=value" / "value" を型と値に分解する。
 * 戻り値: 値の開始位置。型は *type へ(前置が無ければ CCNX_T_NAMESEG)。
 * 不正な 0x 前置(桁数不足・非16進・'=' 無し)は -1 相当として NULL を返す。 */
static const char *seg_split_type(const char *s, size_t len, uint16_t *type) {
    *type = CCNX_T_NAMESEG;
    /* "0xHHHH=" は 7 文字。それより短ければ前置ではありえない */
    if (len < 7) return s;
    if (s[0] != '0' || s[1] != 'x' || s[6] != '=') return s;
    uint16_t v = 0;
    for (int i = 2; i < 6; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return s;                  /* 16進でない → 前置ではなく生の値 */
        v = (uint16_t)(v * 16 + d);
    }
    *type = v;
    return s + 7;
}

/* "/seg0/seg1/..." -> segs。先頭 '/' 必須・末尾 '/' 禁止・空セグメント禁止。
 * "/" 単独は空名前(seg_count=0)。WIRE §3。 */
int ccnx_name_from_uri(ccnx_name_t *n, const char *uri) {
    if (!n || !uri) return -1;
    ccnx_name_clear(n);
    if (uri[0] != '/') return -1;
    const char *p = uri + 1;
    if (*p == '\0') return 0;                 /* 空名前 "/" */
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t seglen = (size_t)(p - start);
        if (seglen == 0) return -1;           /* "//" 空セグメント */
        /* "0xHHHH=" 前置があれば型付きセグメント(codec.h の URI 表記規約) */
        uint16_t stype;
        const char *val = seg_split_type(start, seglen, &stype);
        seglen -= (size_t)(val - start);
        if (seglen == 0) return -1;           /* 型付きで値が空 */
        if (seglen > CCNX_MAX_SEG_LEN) return -1;
        if (n->seg_count >= CCNX_MAX_NAME_SEGS) return -1;
        memcpy(n->seg[n->seg_count], val, seglen);
        n->seg_len[n->seg_count] = (uint16_t)seglen;
        n->seg_type[n->seg_count] = stype;
        n->seg_count++;
        if (*p == '/') {
            p++;
            if (*p == '\0') return -1;         /* 末尾 '/' 禁止 */
        }
    }
    return 0;
}

int ccnx_name_to_uri(const ccnx_name_t *n, char *out, size_t cap) {
    if (!n || !out || cap == 0) return -1;
    if (n->seg_count == 0) {                   /* 空名前 -> "/" */
        if (cap < 2) return -1;
        out[0] = '/';
        out[1] = '\0';
        return 1;
    }
    static const char HEX[] = "0123456789ABCDEF";
    size_t off = 0;
    for (uint16_t i = 0; i < n->seg_count; i++) {
        uint16_t st = n->seg_type[i];
        size_t pre = (st == CCNX_T_NAMESEG) ? 0 : 7;   /* "0xHHHH=" */
        /* '/' + [前置] + seg + 終端 NUL 分の余裕 */
        if (off + 1 + pre + (size_t)n->seg_len[i] + 1 > cap) return -1;
        out[off++] = '/';
        if (pre) {
            out[off++] = '0'; out[off++] = 'x';
            out[off++] = HEX[(st >> 12) & 0xF];
            out[off++] = HEX[(st >> 8)  & 0xF];
            out[off++] = HEX[(st >> 4)  & 0xF];
            out[off++] = HEX[st & 0xF];
            out[off++] = '=';
        }
        memcpy(&out[off], n->seg[i], n->seg_len[i]);
        off += n->seg_len[i];
    }
    out[off] = '\0';
    return (int)off;
}

int ccnx_name_equal(const ccnx_name_t *a, const ccnx_name_t *b) {
    if (!a || !b) return 0;
    if (a->seg_count != b->seg_count) return 0;
    for (uint16_t i = 0; i < a->seg_count; i++) {
        if (a->seg_type[i] != b->seg_type[i]) return 0;   /* 型も同一性の一部 */
        if (a->seg_len[i] != b->seg_len[i]) return 0;
        if (memcmp(a->seg[i], b->seg[i], a->seg_len[i]) != 0) return 0;
    }
    return 1;
}

int ccnx_name_is_prefix(const ccnx_name_t *p, const ccnx_name_t *n) {
    if (!p || !n) return 0;
    if (p->seg_count == 0) return 1;           /* デフォルトルート(全名前一致) */
    if (p->seg_count > n->seg_count) return 0;
    for (uint16_t i = 0; i < p->seg_count; i++) {
        if (p->seg_type[i] != n->seg_type[i]) return 0;   /* 型も同一性の一部 */
        if (p->seg_len[i] != n->seg_len[i]) return 0;
        if (memcmp(p->seg[i], n->seg[i], p->seg_len[i]) != 0) return 0;
    }
    return 1;
}

int ccnx_name_get_seg(const ccnx_name_t *n, uint16_t i, char *out, size_t cap) {
    if (!n || !out) return -1;
    if (i >= n->seg_count) return -1;
    uint16_t l = n->seg_len[i];
    if ((size_t)l + 1 > cap) return -1;
    memcpy(out, n->seg[i], l);
    out[l] = '\0';
    return (int)l;
}

/* =====================================================================
 * encode: msg -> wire (WIRE §5 の算法に厳密一致)
 * ===================================================================== */
int ccnx_encode(const ccnx_msg_t *msg, uint8_t *buf, size_t cap) {
    if (!msg || !buf) return -1;

    /* 0. PacketType 検証(RFC 8609 §3.2 / §4.1)。未知値を Interest レイアウトで
     *    送出しない。Go 実装(codec.go Encode の default 節)と受理集合を一致させる。 */
    if (msg->packet_type != CCNX_PT_INTEREST &&
        msg->packet_type != CCNX_PT_CONTENT_OBJECT &&
        msg->packet_type != CCNX_PT_RETURN)
        return -1;

    /* 0b. 全 PacketType で名前必須(空名前 encode 禁止)。
     *     Interest / Interest Return: RFC 8569 §2.1 "An Interest MUST have a Name"。
     *     ContentObject: RFC 8609 §3.6.1 は CCNx Message TLV の Name で先頭
     *     NameSegment の零長を許さない。長さ 0 の T_NAME を持つ CO は「Name を
     *     省略した nameless CO」ではない(nameless は Name TLV 自体を出さない。
     *     WIRE §8-10 のとおり v1 は生成しない)。空名前は FIB/prefix 登録専用の
     *     ローカル表現であり、ワイヤに載せてはならない(WIRE §3.2)。 */
    if (msg->name.seg_count == 0) return -1;

    /* 0c. ReturnCode 0 の生成禁止。
     *     RFC 8609 §3.2.3.3 "A return code of '0' MUST NOT be used, as it
     *     indicates that the returning system did not modify the Return Code
     *     field."(0 は「未設定」を意味する予約値なので送出してはならない) */
    if (msg->packet_type == CCNX_PT_RETURN && msg->return_code == 0) return -1;

    if (msg->name.seg_count > CCNX_MAX_NAME_SEGS) return -1;

    /* 1. Name TLV Value 長 = Σ(4 + seg_len) */
    size_t name_val = 0;
    for (uint16_t i = 0; i < msg->name.seg_count; i++) {
        if (msg->name.seg_len[i] > CCNX_MAX_SEG_LEN) return -1;
        /* RFC 8609 §3.6.1: 先頭 NameSegment の長さ 0 は文法が許さない */
        if (i == 0 && msg->name.seg_len[i] == 0) return -1;
        /* RFC 8609 §3.3.1: Pad TLV は Name の中に置けない(WIRE §7.2 E8) */
        if (msg->name.seg_type[i] == CCNX_T_PAD) return -1;
        name_val += 4u + (size_t)msg->name.seg_len[i];
    }
    if (name_val > 0xFFFF) return -1;
    size_t name_tlv = 4u + name_val;

    /* 2. Payload TLV(あれば) */
    int has_payload = (msg->payload_len > 0);
    size_t payload_tlv = 0;
    if (has_payload) {
        if (msg->payload_len > CCNX_MAX_PAYLOAD) return -1;
        payload_tlv = 4u + (size_t)msg->payload_len;
    }

    /* 3. Message Value / Message TLV */
    size_t msg_val = name_tlv + payload_tlv;
    if (msg_val > 0xFFFF) return -1;
    size_t msg_tlv = 4u + msg_val;

    /* 4. PacketLength = 8 + Message TLV。HeaderLength = 8 */
    size_t pkt_len = CCNX_FIXED_HDR_LEN + msg_tlv;
    if (pkt_len > 0xFFFF) return -1;           /* WIRE §5 Length 溢れ */
    if (pkt_len > CCNX_MAX_DATAGRAM) return -1; /* 1 UDP datagram 上限(Go と同値) */
    if (pkt_len > cap) return -1;

    /* 5. 固定ヘッダ(WIRE §1) */
    buf[0] = 0x01;                              /* Version(必須) */
    buf[1] = msg->packet_type;                  /* PacketType */
    put_u16(&buf[2], (uint16_t)pkt_len);        /* PacketLength BE */
    if (msg->packet_type == CCNX_PT_CONTENT_OBJECT) {
        buf[4] = 0x00;                          /* Reserved(2 octet) */
        buf[5] = 0x00;
    } else if (msg->packet_type == CCNX_PT_RETURN) {
        buf[4] = msg->hop_limit;                /* HopLimit(Interest と同一) */
        buf[5] = msg->return_code;              /* ReturnCode(RFC 8609 §3.2.3.3) */
    } else {                                    /* Interest */
        buf[4] = msg->hop_limit;                /* HopLimit */
        buf[5] = 0x00;                          /* Reserved */
    }
    buf[6] = 0x00;                              /* Flags */
    buf[7] = 0x08;                              /* HeaderLength(固定ヘッダのみ) */

    size_t off = CCNX_FIXED_HDR_LEN;

    /* Message TLV */
    uint16_t mtype = (msg->packet_type == CCNX_PT_CONTENT_OBJECT)
                         ? CCNX_T_OBJECT : CCNX_T_INTEREST;
    put_u16(&buf[off], mtype);          off += 2;
    put_u16(&buf[off], (uint16_t)msg_val); off += 2;

    /* Name TLV */
    put_u16(&buf[off], CCNX_T_NAME);        off += 2;
    put_u16(&buf[off], (uint16_t)name_val); off += 2;
    for (uint16_t i = 0; i < msg->name.seg_count; i++) {
        put_u16(&buf[off], msg->name.seg_type[i]); off += 2;   /* 型を保存(§3.6.1) */
        put_u16(&buf[off], msg->name.seg_len[i]); off += 2;
        memcpy(&buf[off], msg->name.seg[i], msg->name.seg_len[i]);
        off += msg->name.seg_len[i];
    }

    /* Payload TLV */
    if (has_payload) {
        put_u16(&buf[off], CCNX_T_PAYLOAD);   off += 2;
        put_u16(&buf[off], msg->payload_len); off += 2;
        memcpy(&buf[off], msg->payload, msg->payload_len);
        off += msg->payload_len;
    }

    return (int)off;                            /* == pkt_len */
}

/* =====================================================================
 * decode: wire -> msg (WIRE §5 逆順。未知 Type はスキップ)
 * ===================================================================== */
int ccnx_decode(const uint8_t *buf, size_t len, ccnx_msg_t *msg) {
    if (!buf || !msg) return -1;
    if (len < CCNX_FIXED_HDR_LEN) return -1;

    if (buf[0] != 0x01) return -1;              /* Version 必須 */
    uint8_t pt = buf[1];
    if (pt != CCNX_PT_INTEREST && pt != CCNX_PT_CONTENT_OBJECT &&
        pt != CCNX_PT_RETURN)
        return -1;

    size_t pkt_len = get_u16(&buf[2]);
    if (pkt_len < CCNX_FIXED_HDR_LEN || pkt_len != len) return -1;  /* WIRE D8: 末尾余剰バイト
                                     = PacketLength 不整合。1 datagram = 1 メッセージ(WIRE §0) */
    if (pkt_len > CCNX_MAX_DATAGRAM) return -1;  /* datagram 上限(Go と同値) */

    size_t hdr_len = buf[7];
    if (hdr_len < CCNX_FIXED_HDR_LEN || hdr_len > pkt_len) return -1;

    msg->packet_type = pt;
    if (pt == CCNX_PT_INTEREST || pt == CCNX_PT_RETURN)
        msg->hop_limit = buf[4];
    else
        msg->hop_limit = 0;
    /* ReturnCode は Interest Return のみ。他種別では必ず 0(RFC 8609 §3.2.3.3) */
    msg->return_code = (pt == CCNX_PT_RETURN) ? buf[5] : 0;
    ccnx_name_clear(&msg->name);
    msg->payload_len = 0;

    /* Message TLV は HeaderLength 直後(v1 は hop-by-hop 域が空なので 8)。
     * RFC 8609 §3.1: PacketPayload は CCNx Message TLV から始まる。したがって
     * 最初のトップレベル TLV は必ず T_INTEREST / T_OBJECT であり、Message より
     * 前に他 TLV(Validation を含む)が来る並びは文法違反として拒否する(WIRE
     * §7.3 D12)。正当な Validation は Message の後ろに来るので、後続 TLV の
     * スキップ(下の while)で署名付き CO は落ちない。 */
    size_t off = hdr_len;
    if (off + 4 > pkt_len) return -1;
    uint16_t mtype = get_u16(&buf[off]);
    uint16_t mlen = get_u16(&buf[off + 2]);
    if (mtype != CCNX_T_INTEREST && mtype != CCNX_T_OBJECT) return -1;
    /* PacketType と Message TLV 型の整合(WIRE §7.3 D13)。Interest Return は
     * 元 Interest の PacketType / ReturnCode だけを書き換えたものなので本文は
     * T_INTEREST でなければならない(RFC 8609 §3.2.3)。CO は T_OBJECT。 */
    if (mtype != ((pt == CCNX_PT_CONTENT_OBJECT) ? CCNX_T_OBJECT
                                                 : CCNX_T_INTEREST))
        return -1;
    off += 4;
    if (off + (size_t)mlen > pkt_len) return -1;
    size_t mend = off + mlen;

    int got_name = 0;
    while (off + 4 <= mend) {
        uint16_t t = get_u16(&buf[off]);
        uint16_t l = get_u16(&buf[off + 2]);
        off += 4;
        if (off + (size_t)l > mend) return -1;

        if (t == CCNX_T_NAME) {
            size_t noff = off;
            size_t nend = off + l;
            ccnx_name_clear(&msg->name);
            while (noff + 4 <= nend) {
                uint16_t st = get_u16(&buf[noff]);
                uint16_t sl = get_u16(&buf[noff + 2]);
                noff += 4;
                if (noff + (size_t)sl > nend) return -1;
                /* ★全てのセグメント型を型ごと保持する(RFC 8609 §3.6.1)。
                 *   T_IPID(0x0002)や T_APP:00-4095(0x1000-0x1FFF)は定義済み型であり、
                 *   捨てると別名が同一名に潰れて経路・PIT・CS の鍵が壊れる。 */
                if (msg->name.seg_count >= CCNX_MAX_NAME_SEGS) return -1;
                if (sl > CCNX_MAX_SEG_LEN) return -1;
                /* 先頭セグメントの長さ 0 は文法が許さない(§3.6.1) */
                if (msg->name.seg_count == 0 && sl == 0) return -1;
                /* Pad TLV は Name 内に置けない(RFC 8609 §3.3.1 / WIRE §7.3 D14) */
                if (st == CCNX_T_PAD) return -1;
                uint16_t idx = msg->name.seg_count;
                msg->name.seg_type[idx] = st;
                msg->name.seg_len[idx] = sl;
                memcpy(msg->name.seg[idx], &buf[noff], sl);
                msg->name.seg_count++;
                noff += sl;
            }
            if (noff != nend) return -1;         /* Name TLV 内の余剰バイト */
            got_name = 1;
        } else if (t == CCNX_T_PAYLOAD) {
            if (l > CCNX_MAX_PAYLOAD) return -1;
            msg->payload_len = l;
            memcpy(msg->payload, &buf[off], l);
        }
        /* 未知メッセージ体 TLV はスキップ */
        off += l;
    }
    if (off != mend) return -1;                  /* Message Value の余剰バイト */
    if (!got_name) return -1;                    /* Name は必須 */

    /* Message TLV の後ろに 2 個目の CCNx Message があれば文法違反として拒否。
     * (Validation 等の他 TLV はスキップして構わない) */
    while (off + 4 <= pkt_len) {
        uint16_t t = get_u16(&buf[off]);
        uint16_t l = get_u16(&buf[off + 2]);
        if (off + 4 + (size_t)l > pkt_len) return -1;
        if (t == CCNX_T_INTEREST || t == CCNX_T_OBJECT) return -1;
        off += 4 + (size_t)l;
    }
    /* TLV 境界に載らない端数(1〜3 バイト)を残して終わってはならない。
     * これを見逃すと Go(codec.go の off != len(body) 検査)と受理集合が食い違う。 */
    if (off != pkt_len) return -1;

    return (int)pkt_len;                         /* 消費バイト数 */
}
