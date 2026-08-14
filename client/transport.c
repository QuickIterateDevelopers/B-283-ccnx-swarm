/*
 * transport.c - UDP フェイス実装
 *
 * B-283 A層。sendto/recvfrom + poll。名前もフォワーダも知らない(DESIGN §3)。
 */
#include "transport.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void make_addr(struct sockaddr_in *sa, const char *ip, uint16_t port) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    inet_pton(AF_INET, ip ? ip : "127.0.0.1", &sa->sin_addr);
}

int ccnx_face_open(ccnx_face_t *f, const char *bind_addr,
                   uint16_t listen_port, uint16_t tap_port) {
    if (!f) return -1;
    memset(f, 0, sizeof(*f));
    f->fd = -1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    make_addr(&sa, bind_addr ? bind_addr : "127.0.0.1", listen_port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }

    f->fd = fd;
    f->listen_port = listen_port;
    /* listen_port==0 は OS 任せの ephemeral 割り当て。実際に bind されたポートを
     * getsockname で解決して保持する(Go の Open が LocalAddr で実ポートを拾うのと
     * 対称。これが無いと ccnx_portal_port が 0 を返し、相手が送り先を確定できない)。 */
    if (listen_port == 0) {
        struct sockaddr_in bound;
        socklen_t bl = sizeof(bound);
        if (getsockname(fd, (struct sockaddr *)&bound, &bl) == 0)
            f->listen_port = ntohs(bound.sin_port);
    }
    f->tap_port = tap_port;
    return 0;
}

void ccnx_face_close(ccnx_face_t *f) {
    if (!f) return;
    if (f->fd >= 0) close(f->fd);
    f->fd = -1;
}

static int send_to_port(int fd, uint16_t port, const uint8_t *buf, size_t len) {
    struct sockaddr_in dst;
    make_addr(&dst, "127.0.0.1", port);
    ssize_t n = sendto(fd, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst));
    return (n < 0) ? -1 : (int)n;
}

int ccnx_face_send(ccnx_face_t *f, uint16_t dst_port,
                   const uint8_t *buf, size_t len) {
    if (!f || f->fd < 0 || !buf || len == 0) return -1;
    int n = send_to_port(f->fd, dst_port, buf, len);
    if (n < 0) return -1;
    /* 送出 datagram の複製を tap へ(PROTOCOL §5.1。C->Go ライブ相互運用点)。
     * tap 自身宛の送出は複製しない(二重ループ防止)。 */
    if (f->tap_port > 0 && dst_port != f->tap_port)
        (void)send_to_port(f->fd, f->tap_port, buf, len);
    return n;
}

int ccnx_face_recv(ccnx_face_t *f, uint32_t timeout_ms,
                   uint8_t *buf, size_t cap, uint16_t *src_port) {
    if (!f || f->fd < 0 || !buf || cap == 0) return -1;

    struct pollfd pfd;
    pfd.fd = f->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pr = poll(&pfd, 1, (int)timeout_ms);
    if (pr < 0) {
        if (errno == EINTR) return 0;   /* SIGTERM 等で中断: データ無扱い */
        return -1;
    }
    if (pr == 0) return 0;              /* タイムアウト */
    if (!(pfd.revents & POLLIN)) return 0;

    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(f->fd, buf, cap, 0,
                         (struct sockaddr *)&from, &fromlen);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (src_port) *src_port = ntohs(from.sin_port);
    return (int)n;
}
