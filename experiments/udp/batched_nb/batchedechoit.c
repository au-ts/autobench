#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_BATCH     64
// largest possible UDP payload, so no datagram is ever truncated
#define MSG_SIZE      65536

static int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int main(int argc, const char *argv[]) {
    // extra arguments are accepted (and ignored) so ipbench's server_flags
    // can be passed through
    if (argc < 3) {
        printf("usage: (port) (batch_size) [max=%d]\n", MAX_BATCH);
        exit(-1);
    }
    uint16_t port = atoi(argv[1]);
    int batch = atoi(argv[2]);
    if (batch < 1 || batch > MAX_BATCH) {
        printf("batch size must be between 1 and %d!\n", MAX_BATCH);
        exit(-1);
    }
    int efd = epoll_create1(EPOLL_CLOEXEC);
    if (efd < 0) {
        perror("epoll_create1");
        exit(1);
    }

    int rcvbuf = 4 * 1024 * 1024;
    int sndbuf = 4 * 1024 * 1024;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        perror("socket");
        exit(1);
    }
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(s, (struct sockaddr *)&sa, sizeof sa)) {
        perror("bind");
        exit(1);
    }
    set_nonblocking(s);
    epoll_ctl(efd, EPOLL_CTL_ADD, s,
              &(struct epoll_event){
                  .events = EPOLLIN | EPOLLET,
                  .data.fd = s,
              });
    printf("batchedechoit: listening on port %d, batch size %d\n", port, batch);
    fflush(stdout);

    static struct mmsghdr     msgs[MAX_BATCH];
    static struct iovec       iovecs[MAX_BATCH];
    static char               buffers[MAX_BATCH][MSG_SIZE];
    static struct sockaddr_in peer_addrs[MAX_BATCH];

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
            // edge triggered: drain until EAGAIN
            for (;;) {
                int r = recvmmsg(s, msgs, batch, MSG_DONTWAIT, NULL);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (r == 0) break;

                for (int j = 0; j < r; j++)
                    iovecs[j].iov_len = msgs[j].msg_len;

                int sent = 0;
                while (sent < r) {
                    int k = sendmmsg(s, &msgs[sent], r - sent, MSG_DONTWAIT);
                    if (k < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // send buffer full: wait for room rather than
                            // dropping the rest of the batch
                            struct pollfd p = { .fd = s, .events = POLLOUT };
                            poll(&p, 1, -1);
                            continue;
                        }
                        // this datagram can't be sent; skip it, keep the rest
                        sent++;
                        continue;
                    }
                    sent += k;
                }

                for (int j = 0; j < r; j++) {
                    iovecs[j].iov_len = MSG_SIZE;
                    msgs[j].msg_hdr.msg_namelen = sizeof peer_addrs[j];
                }
            }
        }
    }
    return 0;
}
