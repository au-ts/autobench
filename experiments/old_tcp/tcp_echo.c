#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_EVENTS 16
#define RECV_BUF_SIZE 4096

int epoll_fd;

void on_recv_available(int fd)
{
    uint8_t buf[RECV_BUF_SIZE];
    ssize_t len = recv(fd, buf, RECV_BUF_SIZE, 0);
    if (len < 0) {
        perror("read");
        exit(1);
    } else if (len > 0) {
        send(fd, buf, len, 0);
    } else {
        printf("tcp_echo: disconnect (this should never be printed)\n");
        close(fd);
    }
}

int main()
{
    // create socket
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);

    // bind to port
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(1237),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(listen_sock, (struct sockaddr*) &sa, sizeof(sa))) {
        perror("bind");
        return 1;
    }

    // accept connections
    if (listen(listen_sock, 4)) {
        perror("listen");
        return 1;
    }

    socklen_t sa_len = sizeof(sa);
    if (getsockname(listen_sock, (struct sockaddr*) &sa, &sa_len)) {
        perror("getsockname");
        return 1;
    }
    printf("tcp_echo: listening on %s:%d\n", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));

    // set up epoll
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll");
        return 1;
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_sock, &(struct epoll_event){
        .events = EPOLLIN,
        .data.fd = listen_sock,
    })) {
        perror("epool_ctl(listen_sock)");
        return 1;
    }

    while(1) {
        struct epoll_event events[MAX_EVENTS];
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (num_events == -1) {
            perror("epoll_wait");
            return 1;
        }

        for (int i = 0; i < num_events; ++i) {
            if (events[i].data.fd == listen_sock) {
                // new connection
                struct sockaddr_in conn_sa;
                socklen_t conn_sa_len = sizeof(conn_sa);
                int conn_sock = accept(listen_sock, (struct sockaddr*) &conn_sa, &conn_sa_len);
                if (conn_sock == -1) {
                    perror("accept");
                    return 1;
                }

                printf("tcp_echo[%s:%d]: accept\n", inet_ntoa(conn_sa.sin_addr), conn_sa.sin_port);

                // add to epoll
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_sock, &(struct epoll_event){
                    .events = EPOLLIN | EPOLLRDHUP | EPOLLHUP,
                    .data.fd = conn_sock,
                })) {
                    perror("epool_ctl(conn_sock)");
                    return 1;
                }
            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
                printf("tcp_echo: disconnect\n");
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                close(events[i].data.fd);

            } else if (events[i].events & EPOLLIN) {
                on_recv_available(events[i].data.fd);
            }
        }
    }
}
