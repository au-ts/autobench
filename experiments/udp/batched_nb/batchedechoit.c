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

#define BATCH     64
#define MSG_SIZE  2048

static int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        // printf("usage: (port) (nthreads)\n");
        printf("usage: (port)\n");
        exit(-1);
    }
    uint16_t port = atoi(argv[1]);
    // int nthreads  = atoi(argv[2]);
    // printf("starting on port %u with %d threads\n", port, nthreads);

    int efd = epoll_create1(EPOLL_CLOEXEC);

    // // One socket per thread
    // int *socks = calloc(nthreads, sizeof *socks);
    // for (int i = 0; i < nthreads; i++) {
    //     int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    //     int one = 1;
    //     setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    //     int rcvbuf = 4 * 1024 * 1024;
    //     setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    //     struct sockaddr_in sa = {
    //         .sin_family = AF_INET,
    //         .sin_port = htons(port),
    //         .sin_addr.s_addr = INADDR_ANY,
    //     };
    //     bind(s, (struct sockaddr *)&sa, sizeof sa);
    //     set_nonblocking(s);
    //     epoll_ctl(efd, EPOLL_CTL_ADD, s,
    //               &(struct epoll_event){
    //                   .events = EPOLLIN | EPOLLET,
    //                   .data.fd = s,
    //               });
    //     socks[i] = s;
    // }
    int rcvbuf = 4 * 1024 * 1024;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
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

    // preallocate batch buffers
    struct mmsghdr msgs[BATCH];
    struct iovec   iovecs[BATCH];
    char           buffers[BATCH][MSG_SIZE];
    struct sockaddr_in peer_addrs[BATCH];

    memset(msgs, 0, sizeof msgs);
    for (int i = 0; i < BATCH; i++) {
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
                int r = recvmmsg(s, msgs, BATCH, MSG_DONTWAIT, NULL);
                if (r <= 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    break;
                }
                for (int j = 0; j < r; j++)
                    iovecs[j].iov_len = msgs[j].msg_len;
                sendmmsg(s, msgs, r, 0);
                // reset iovec
                for (int j = 0; j < r; j++)
                    iovecs[j].iov_len = MSG_SIZE;
            }
        }
    }
    return 0;
}

