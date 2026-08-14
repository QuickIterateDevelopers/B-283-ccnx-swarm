/*
 * codec.h - CCNx (RFC 8609) TLV wire-format encoder/decoder
 *
 * B-283 CCNx セマンティック群ロボ通信デモ
 * 8609 のワイヤ形式をバイト厳密に組み立て/分解する。可視化・エージェント・
 * Cefore 相互接続は全てこの層の上に乗る。まず round-trip テスト
 * (bytes -> struct -> bytes が完全一致) で仕様忠実性を自己証明する。
 *
 * 参照: RFC 8609 "CCNx Messages in TLV Format" (~/Downloads/B-283-CCNx/rfc8609.txt)
 *       RFC 8569 "CCNx Semantics"                (~/Downloads/B-283-CCNx/rfc8569.txt)
 */
#ifndef CCNX_CODEC_H
#define CCNX_CODEC_H

#include <stdint.h>
#include <stddef.h>

/* --- Fixed header PacketType (RFC 8609 §3.2 / §4.1 Packet Type Registry) --- */
#define CCNX_PT_INTEREST        0x00
#define CCNX_PT_CONTENT_OBJECT  0x01
#define CCNX_PT_RETURN          0x02

/* --- Interest Return ReturnCode (RFC 8609 §3.2.3.3, Table 2) --- */
#define CCNX_T_RETURN_NO_ROUTE                     0x01
#define CCNX_T_RETURN_LIMIT_EXCEEDED               0x02
#define CCNX_T_RETURN_NO_RESOURCES                 0x03
#define CCNX_T_RETURN_PATH_ERROR                   0x04
#define CCNX_T_RETURN_PROHIBITED                   0x05
#define CCNX_T_RETURN_CONGESTED                    0x06
#define CCNX_T_RETURN_MTU_TOO_LARGE                0x07
#define CCNX_T_RETURN_UNSUPPORTED_HASH_RESTRICTION 0x08
#define CCNX_T_RETURN_MALFORMED_INTEREST           0x09

/* --- Top-level message TLV types (RFC 8609 §3.3, §3.4) --- */
#define CCNX_T_INTEREST         0x0001
#define CCNX_T_OBJECT           0x0002

/* --- Message-body TLV types --- */
#define CCNX_T_NAME             0x0000
#define CCNX_T_PAYLOAD          0x0001

/* --- Name segment TLV types (RFC 8609 §3.6.1) --- */
#define CCNX_T_NAMESEG          0x0001  /* generic NameSegment */
#define CCNX_T_IPID             0x0002  /* Interest Payload ID */
#define CCNX_T_APP_FIRST        0x1000  /* T_APP:00 */
#define CCNX_T_APP_LAST         0x1FFF  /* T_APP:4095 */

/* Pad TLV (RFC 8609 §3.3.1)。Name の中に置いてはならない型であり、
 * encode / decode とも Name 内の T_PAD セグメントを拒否する(WIRE §7.2 E8 / §7.3 D14) */
#define CCNX_T_PAD              0x0FFE

/* --- 受理/生成境界(C と Go で厳密に同一。INTEROP 契約なので勝手に変えない) --- */
#define CCNX_MAX_NAME_SEGS      16
#define CCNX_MAX_SEG_LEN        256
#define CCNX_MAX_PAYLOAD        1400    /* 1 UDP datagram に収める前提 */
#define CCNX_MAX_DATAGRAM       1500    /* datagram 全長上限(Go: MaxDatagram) */
#define CCNX_FIXED_HDR_LEN      8

/* 1 本の名前 (例: /area/A-2/task/rescue)
 *
 * ★セグメントは「型 + 値」の対である(RFC 8609 §3.6.1)。名前の同一性判定は
 *   型と値の両方を突き合わせる。汎用でない型(T_IPID / T_APP:00-4095 等)を
 *   捨てて値だけ残すと、別名を同一名と誤認して経路・PIT・CS の鍵が壊れる。
 *
 * URI 表記(C/Go 共通・INTEROP 契約):
 *   - 型が CCNX_T_NAMESEG(0x0001) のセグメント : そのまま  例 /area/A-2
 *   - それ以外の型                             : 0xHHHH= を前置
 *                                                例 /0x1000=app  /0x0002=ipid
 */
typedef struct {
    uint16_t seg_count;
    uint16_t seg_type[CCNX_MAX_NAME_SEGS];   /* RFC 8609 §3.6.1 の NameSegment 型 */
    uint16_t seg_len[CCNX_MAX_NAME_SEGS];
    uint8_t  seg[CCNX_MAX_NAME_SEGS][CCNX_MAX_SEG_LEN];
} ccnx_name_t;

typedef struct {
    uint8_t     packet_type;    /* CCNX_PT_* */
    uint8_t     hop_limit;      /* Interest / Interest Return のみ意味を持つ */
    /* ReturnCode (RFC 8609 §3.2.3.3)。CCNX_PT_RETURN のときのみ意味を持ち、
     * それ以外の PacketType では encode/decode ともに必ず 0 になる。
     * ワイヤ上は固定ヘッダ octet 5 (Interest の Reserved と同位置)。 */
    uint8_t     return_code;
    ccnx_name_t name;
    uint16_t    payload_len;
    uint8_t     payload[CCNX_MAX_PAYLOAD];
} ccnx_msg_t;

/* 名前操作 */
void ccnx_name_clear(ccnx_name_t *n);
int  ccnx_name_append(ccnx_name_t *n, const char *seg);          /* 文字列セグメント追加 */
int  ccnx_name_from_uri(ccnx_name_t *n, const char *uri);        /* "/a/b/c" -> segs */
int  ccnx_name_to_uri(const ccnx_name_t *n, char *out, size_t cap);
int  ccnx_name_equal(const ccnx_name_t *a, const ccnx_name_t *b);

/* prefix 判定: p の全セグメントが n の先頭に一致するか。
 * FIB 最長一致の唯一の基準。一致=1(p は n の prefix)/不一致=0。
 * p.seg_count == 0 は全名前に一致(デフォルトルート)。
 * p.seg_count > n.seg_count のときは 0。 */
int  ccnx_name_is_prefix(const ccnx_name_t *p, const ccnx_name_t *n);

/* n の i 番目セグメントを NUL 終端文字列として out へ取り出す。
 * 書き込んだ長さ(NUL 除く)を返す。範囲外/溢れは負値。
 * seg[] は生バイトで NUL 終端を保証しないため、値の読み出しは必ず本関数を使う。 */
int  ccnx_name_get_seg(const ccnx_name_t *n, uint16_t i, char *out, size_t cap);

/* encode: msg -> wire。書き込んだバイト数を返す。失敗は負値 */
int ccnx_encode(const ccnx_msg_t *msg, uint8_t *buf, size_t cap);

/* decode: wire -> msg。消費バイト数を返す。失敗は負値 */
int ccnx_decode(const uint8_t *buf, size_t len, ccnx_msg_t *msg);

#endif /* CCNX_CODEC_H */
