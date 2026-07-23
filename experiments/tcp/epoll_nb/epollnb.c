#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
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
    if (batch < 1 || batch > MAX_BATCH) {
        printf("batch size must be between 1 and %d!\n", MAX_BATCH);
        exit(-1);
    }

    signal(SIGPIPE, SIG_IGN);

    int efd = epoll_create1(EPOLL_CLOEXEC);

    int rcvbuf = 4 * 1024 * 1024;
    int sndbuf = 4 * 1024 * 1024;
    int one = 1;

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(lfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    setsockopt(lfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(lfd, (struct sockaddr *)&sa, sizeof sa);
    listen(lfd, SOMAXCONN);
    set_nonblocking(lfd);
    epoll_ctl(efd, EPOLL_CTL_ADD, lfd,
              &(struct epoll_event){
                  .events = EPOLLIN | EPOLLET,
                  .data.fd = lfd,
              });

    struct mmsghdr msgs[MAX_BATCH];
    struct iovec   iovecs[MAX_BATCH];
    char           buffers[MAX_BATCH][MSG_SIZE];

    memset(msgs, 0, sizeof msgs);
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
                        if (errno == EINTR) continue;
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

            for (;;) {
                int r = recvmmsg(fd, msgs, batch, MSG_DONTWAIT, NULL);
                if (r < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    break;
                }
                if (r == 0) {
                    epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    break;
                }

                for (int j = 0; j < r; j++)
                    iovecs[j].iov_len = msgs[j].msg_len;

                int sent = 0;
                int err = 0;
                while (sent < r) {
                    int k = sendmmsg(fd, &msgs[sent], r - sent, MSG_DONTWAIT);
                    if (k < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        err = 1;
                        break;
                    }
                    if (k == 0) { err = 1; break; }
                    int last = sent + k - 1;
                    if (msgs[last].msg_len < iovecs[last].iov_len) {
                        iovecs[last].iov_base = (char *)iovecs[last].iov_base + msgs[last].msg_len;
                        iovecs[last].iov_len -= msgs[last].msg_len;
                        sent = last;
                    } else {
                        sent += k;
                    }
                }

                if (!err && sent < r) {
                    set_blocking(fd);
                    while (sent < r) {
                        int k = sendmmsg(fd, &msgs[sent], r - sent, 0);
                        if (k < 0) {
                            if (errno == EINTR) continue;
                            err = 1;
                            break;
                        }
                        if (k == 0) { err = 1; break; }
                        int last = sent + k - 1;
                        if (msgs[last].msg_len < iovecs[last].iov_len) {
                            iovecs[last].iov_base = (char *)iovecs[last].iov_base + msgs[last].msg_len;
                            iovecs[last].iov_len -= msgs[last].msg_len;
                            sent = last;
                        } else {
                            sent += k;
                        }
                    }
                    set_nonblocking(fd);
                }

                for (int j = 0; j < r; j++) {
                    iovecs[j].iov_base = buffers[j];
                    iovecs[j].iov_len = MSG_SIZE;
                }

                if (err) {
                    epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    break;
                }
            }
        }
    }
    return 0;
}
