#include <signal.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_EVENTS    16
#define RECV_BUF_SIZE 4096

/* Connection-socket interest set: re-armed each time via EPOLL_CTL_MOD.
   EPOLLONESHOT guarantees only one worker thread owns a given fd at once. */
#define CONN_EVENTS (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLONESHOT)

static int epoll_fd;
static int listen_sock;

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void drop_connection(int fd)
{
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

static void on_recv_available(int fd)
{
    uint8_t buf[RECV_BUF_SIZE];
    ssize_t len = recv(fd, buf, RECV_BUF_SIZE, 0);

    if (len < 0) {
        /* Spurious wakeup on a non-blocking socket: just re-arm. */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            goto rearm;
        perror("recv");
        drop_connection(fd); /* don't take down the whole server */
        return;
    } else if (len == 0) {
        /* Orderly shutdown. EPOLLRDHUP normally beats us to it,
           hence the original's "should never be printed" note. */
        printf("tcp_echo: disconnect (recv returned 0)\n");
        drop_connection(fd);
        return;
    }

    /* Echo it all back. send() on a blocking-style stream may short-write. */
    for (ssize_t off = 0; off < len; ) {
        ssize_t n = send(fd, buf + off, (size_t)(len - off), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("send");
            drop_connection(fd);
            return;
        }
        off += n;
    }

rearm:
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &(struct epoll_event){
            .events = CONN_EVENTS,
            .data.fd = fd,
        })) {
        perror("epoll_ctl(MOD rearm)");
        drop_connection(fd);
    }
}

static void accept_new_connections(void)
{
    for (;;) {
        struct sockaddr_in conn_sa;
        socklen_t conn_sa_len = sizeof conn_sa;
        int conn_sock = accept(listen_sock, (struct sockaddr*)&conn_sa, &conn_sa_len);

        if (conn_sock == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; /* drained all pending connections */
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("accept");
            break;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &conn_sa.sin_addr, ip, sizeof ip);
        /* NB: original printed sin_port without ntohs(); kept for parity. */
        printf("tcp_echo[%s:%d]: accept\n", ip, conn_sa.sin_port);

        if (set_nonblocking(conn_sock)) {
            perror("set_nonblocking(conn_sock)");
            close(conn_sock);
            continue;
        }

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_sock, &(struct epoll_event){
                .events = CONN_EVENTS,
                .data.fd = conn_sock,
            })) {
            perror("epoll_ctl(ADD conn_sock)");
            close(conn_sock);
        }
    }
}

static void* worker_loop(void* arg)
{
    (void)arg;
    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (num_events == -1) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < num_events; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_sock) {
                accept_new_connections();
            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
                printf("tcp_echo: disconnect\n");
                drop_connection(fd);
            } else if (events[i].events & EPOLLIN) {
                on_recv_available(fd);
            }
        }
    }

    return NULL;
}

static int parse_positive_int(const char* str, int* result)
{
    char* endptr;
    long val = strtol(str, &endptr, 10);
    if (endptr == str || *endptr != '\0')
        return -1;
    if (val == LONG_MAX || val == LONG_MIN)
        return -1;
    if (val <= 0 || val > INT_MAX)
        return -1;
    *result = (int)val;
    return 0;
}

static int parse_port(const char* str, uint16_t* result)
{
    char* endptr;
    long val = strtol(str, &endptr, 10);
    if (endptr == str || *endptr != '\0')
        return -1;
    if (val == LONG_MAX || val == LONG_MIN)
        return -1;
    if (val <= 0 || val > UINT16_MAX)
        return -1;
    *result = (uint16_t)val;
    return 0;
}

int main(int argc, const char* argv[])
{
    if (argc < 3) {
        printf("usage: echo [TEST_PORT] [num_threads]\n");
        exit(-1);
    }

    uint16_t port;
    if (parse_port(argv[1], &port) != 0) {
        fprintf(stderr, "Error: Invalid port number '%s'. Must be 1-65535.\n", argv[1]);
        exit(-1);
    }

    int num_threads;
    if (parse_positive_int(argv[2], &num_threads) != 0) {
        fprintf(stderr, "Error: Invalid thread count '%s'. Must be a positive integer.\n", argv[2]);
        exit(-1);
    }

    /* Ignore SIGPIPE so a send() to a closed peer returns EPIPE instead
       of killing the process. */
    signal(SIGPIPE, SIG_IGN);

    listen_sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_sock < 0) {
        perror("socket");
        return 1;
    }

    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(listen_sock, (struct sockaddr*)&sa, sizeof sa)) {
        perror("bind");
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN)) {
        perror("listen");
        return 1;
    }

    if (set_nonblocking(listen_sock)) {
        perror("set_nonblocking(listen_sock)");
        return 1;
    }

    socklen_t sa_len = sizeof sa;
    if (getsockname(listen_sock, (struct sockaddr*)&sa, &sa_len)) {
        perror("getsockname");
        return 1;
    }
    printf("tcp_echo: listening on %s:%d with %d threads\n",
           inet_ntoa(sa.sin_addr), ntohs(sa.sin_port), num_threads);

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        return 1;
    }

    /* EPOLLEXCLUSIVE: only one worker is woken per incoming connection,
       avoiding a thundering herd on accept(). */
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_sock, &(struct epoll_event){
            .events = EPOLLIN | EPOLLEXCLUSIVE,
            .data.fd = listen_sock,
        })) {
        perror("epoll_ctl(listen_sock)");
        return 1;
    }

    pthread_t* threads = malloc((size_t)num_threads * sizeof(pthread_t));
    if (threads == NULL) {
        perror("malloc(threads)");
        return 1;
    }

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, worker_loop, NULL)) {
            fprintf(stderr, "Could not create thread %d: %s\n", i, strerror(errno));
            for (int j = 0; j < i; j++)
                pthread_join(threads[j], NULL);
            free(threads);
            close(epoll_fd);
            close(listen_sock);
            return 1;
        }
    }

    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    free(threads);
    close(epoll_fd);
    close(listen_sock);
    return 0;
}

