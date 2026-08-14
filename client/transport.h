/*
 * transport.h - UDP フェイス(RFC 8569 の face 相当)
 *
 * B-283 A層。UDP バイトの送受信 + tap 複製のみ。名前の意味を知らない(DESIGN §3)。
 * bind は常に 127.0.0.1(PROTOCOL §5.2)。送出時 tap_port>0 なら複製を tap へも打つ。
 */
#ifndef CCNX_TRANSPORT_H
#define CCNX_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int      fd;           /* UDP ソケット */
    uint16_t listen_port;  /* 自ノード listen(9200+i) */
    uint16_t tap_port;     /* 観測 tap(9300)。0 なら複製しない */
} ccnx_face_t;

/* listen_port に UDP バインド。bind_addr=NULL は "127.0.0.1"。0/負値。 */
int  ccnx_face_open(ccnx_face_t *f, const char *bind_addr,
                    uint16_t listen_port, uint16_t tap_port);
void ccnx_face_close(ccnx_face_t *f);

/* dst_port(127.0.0.1)へ datagram を送出。tap_port>0 かつ dst!=tap なら tap へも複製。
 * 返り値: 送出バイト数 / 負値=失敗。 */
int  ccnx_face_send(ccnx_face_t *f, uint16_t dst_port,
                    const uint8_t *buf, size_t len);

/* datagram を最大 timeout_ms 待って1個受信。src_port に送信元ポートを返す。
 * 返り値: 受信バイト数(>0) / 0=タイムアウト(データ無) / 負値=失敗。 */
int  ccnx_face_recv(ccnx_face_t *f, uint32_t timeout_ms,
                    uint8_t *buf, size_t cap, uint16_t *src_port);

#endif /* CCNX_TRANSPORT_H */
