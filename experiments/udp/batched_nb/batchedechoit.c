#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_BATCH     64
#define MSG_SIZE  2048

static int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int set_blocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
}

int main(int argc, const char *argv[]) {
    if (argc != 3) {
        printf("usage: (port) (batch_size) [max=%d]\n", MAX_BATCH);
        exit(-1);
    }
    uint16_t port = atoi(argv[1]);
    int batch = atoi(argv[2]);
    if (batch < 1 || batch >= MAX_BATCH) {
        printf("batch size must be between 1 and %d!\n", MAX_BATCH);
        exit(-1);
    }
    int efd = epoll_create1(EPOLL_CLOEXEC);

    int rcvbuf = 4 * 1024 * 1024;
    int sndbuf = 4 * 1024 * 1024;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(s, (struct sockaddr *)&sa, sizeof sa);
    set_nonblocking(s);
    epoll_ctl(efd, EPOLL_CTL_ADD, s,
              &(struct epoll_event){
                  .events = EPOLLIN | EPOLLET,
                  .data.fd = s,
              });

    struct mmsghdr msgs[MAX_BATCH];
    struct iovec   iovecs[MAX_BATCH];
    char           buffers[MAX_BATCH][MSG_SIZE];
    struct sockaddr_in peer_addrs[MAX_BATCH];

    memset(msgs, 0, sizeof msgs);
    for (int i = 0; i < batch; i++) {
        iovecs[i].iov_base = buffers[i];
        iovecs[i].iov_len  = MSG_SIZE;
        msgs[i].msg_hdr.msg_iov     = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen  = 1;
        msgs[i].msg_hdr.msg_name    = &peer_addrs[i];
        msgs[i].msg_hdr.msg_namelen = sizeof peer_addrs[i];
    }

    struct epoll_event evs[16];
    for (;;) {
        int n = epoll_wait(efd, evs, 16, -1);
        for (int i = 0; i < n; i++) {
            int s = evs[i].data.fd;
            for (;;) {
                int r = recvmmsg(s, msgs, batch, MSG_DONTWAIT, NULL);
                if (r <= 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    break;
                }
                for (int j = 0; j < r; j++)
                    iovecs[j].iov_len = msgs[j].msg_len;

                int sent = 0;
                while (sent < r) {
                    int k = sendmmsg(s, &msgs[sent], r - sent, MSG_DONTWAIT);
                    if (k < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }
                    sent += k;
                }

                // if we fail to non-blocking send, just switch back to blocking mode to
                // get the work done.
                if (sent < r) {
                    set_blocking(s);
                    while (sent < r) {
                        int k = sendmmsg(s, &msgs[sent], r - sent, 0);
                        if (k < 0) {
                            if (errno == EINTR) continue;
                            break;
                        }
                        sent += k;
                    }
                    set_nonblocking(s);
                }

                for (int j = 0; j < r; j++)
                    iovecs[j].iov_len = MSG_SIZE;
            }
        }
    }
    return 0;
}

