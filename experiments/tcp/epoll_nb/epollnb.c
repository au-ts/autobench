#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_BATCH     64
#define MSG_SIZE      2048
#define MAX_EVENTS    1024

static int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void drop_connection(int efd, int fd) {
    epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

// Echo iov[0..cnt) as one gathered write. TCP is a byte stream, so a short
// write must be resumed from exactly where it stopped -- sendmmsg() would carry
// on with the next message after a short write and leave a hole in the stream.
// If the socket buffer is full we wait for it to drain rather than buffering.
static int send_all(int fd, struct iovec *iov, int cnt) {
    struct msghdr mh = { .msg_iov = iov, .msg_iovlen = cnt };
    while (mh.msg_iovlen > 0) {
        ssize_t k = sendmsg(fd, &mh, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd p = { .fd = fd, .events = POLLOUT };
                if (poll(&p, 1, -1) < 0 && errno != EINTR) return -1;
                continue;
            }
            return -1;
        }
        while (mh.msg_iovlen > 0 && (size_t)k >= mh.msg_iov->iov_len) {
            k -= mh.msg_iov->iov_len;
            mh.msg_iov++;
            mh.msg_iovlen--;
        }
        if (mh.msg_iovlen > 0) {
            mh.msg_iov->iov_base = (char *)mh.msg_iov->iov_base + k;
            mh.msg_iov->iov_len -= k;
        }
    }
    return 0;
}

int main(int argc, const char *argv[]) {
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

    signal(SIGPIPE, SIG_IGN);

    int efd = epoll_create1(EPOLL_CLOEXEC);
    if (efd < 0) {
        perror("epoll_create1");
        exit(1);
    }

    int rcvbuf = 4 * 1024 * 1024;
    int sndbuf = 4 * 1024 * 1024;
    int one = 1;

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) {
        perror("socket");
        exit(1);
    }
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(lfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    setsockopt(lfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    // an unchecked bind failure makes listen() silently pick a random port
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa)) {
        perror("bind");
        exit(1);
    }
    if (listen(lfd, SOMAXCONN)) {
        perror("listen");
        exit(1);
    }
    set_nonblocking(lfd);
    epoll_ctl(efd, EPOLL_CTL_ADD, lfd,
              &(struct epoll_event){
                  .events = EPOLLIN | EPOLLET,
                  .data.fd = lfd,
              });
    printf("epollnb: listening on port %d, batch size %d\n", port, batch);
    fflush(stdout);

    static struct mmsghdr msgs[MAX_BATCH];
    static struct iovec   iovecs[MAX_BATCH];
    static struct iovec   send_iov[MAX_BATCH];
    static char           buffers[MAX_BATCH][MSG_SIZE];

    for (int i = 0; i < MAX_BATCH; i++) {
        iovecs[i].iov_base = buffers[i];
        iovecs[i].iov_len  = MSG_SIZE;
        msgs[i].msg_hdr.msg_iov     = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen  = 1;
        msgs[i].msg_hdr.msg_name    = NULL;
        msgs[i].msg_hdr.msg_namelen = 0;
    }

    struct epoll_event evs[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(efd, evs, MAX_EVENTS, -1);
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;

            if (fd == lfd) {
                for (;;) {
                    int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR || errno == ECONNABORTED) continue;
                        perror("accept4");
                        break;
                    }
                    setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
                    setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
                    epoll_ctl(efd, EPOLL_CTL_ADD, cfd,
                              &(struct epoll_event){
                                  .events = EPOLLIN | EPOLLET | EPOLLRDHUP,
                                  .data.fd = cfd,
                              });
                }
                continue;
            }

            // edge triggered: drain until EAGAIN
            for (;;) {
                int r = recvmmsg(fd, msgs, batch, MSG_DONTWAIT, NULL);
                if (r < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    drop_connection(efd, fd);
                    break;
                }

                // On a stream socket recvmmsg() never returns 0 at EOF: each
                // 0-byte read is counted as a received message instead. The
                // first empty message therefore marks the end of the stream.
                int cnt = 0;
                while (cnt < r && msgs[cnt].msg_len > 0) {
                    send_iov[cnt].iov_base = buffers[cnt];
                    send_iov[cnt].iov_len  = msgs[cnt].msg_len;
                    cnt++;
                }
                int eof = (r == 0 || cnt < r);

                if (cnt > 0 && send_all(fd, send_iov, cnt) < 0) {
                    drop_connection(efd, fd);
                    break;
                }
                if (eof) {
                    drop_connection(efd, fd);
                    break;
                }
            }
        }
    }
    return 0;
}
