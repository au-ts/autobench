#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>

static volatile sig_atomic_t stop_flag = 0;

static void setbufsize(int sock, int size)
{
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
}

static void signal_handler(int sig)
{
    (void)sig;
    stop_flag = 1;
}

/* echo process: echo until parent dies*/
static int echo_function(int sock, int parent_death_fd)
{
    char buf[2000];
    struct sockaddr_in sa;
    socklen_t sa_len;
    ssize_t msglen;

    struct pollfd fds[2];
    fds[0].fd = sock;
    fds[0].events = POLLIN;
    fds[1].fd = parent_death_fd;
    fds[1].events = POLLIN;

    for (;;) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            return 1;
        }

        /* Parent exited or closed its end of the pipe. */
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            break;
        }

        if (fds[0].revents & POLLIN) {
            sa_len = sizeof sa;
            msglen = recvfrom(sock, buf, sizeof buf, 0,
                              (struct sockaddr *)&sa, &sa_len);
            if (msglen > 0) {
                sendto(sock, buf, msglen, 0,
                       (struct sockaddr *)&sa, sa_len);
            }
        }
    }

    return 0;
}

static int parse_positive_int(const char* str, int* result)
{
    char* endptr;
    long val = strtol(str, &endptr, 10);

    if (endptr == str || *endptr != '\0') {
        return -1;
    }
    if (val == LONG_MAX || val == LONG_MIN) {
        return -1;
    }
    if (val <= 0 || val > INT_MAX) {
        return -1;
    }

    *result = (int)val;
    return 0;
}

static int parse_port(const char* str, uint16_t* result)
{
    char* endptr;
    long val = strtol(str, &endptr, 10);

    if (endptr == str || *endptr != '\0') {
        return -1;
    }
    if (val == LONG_MAX || val == LONG_MIN) {
        return -1;
    }
    if (val <= 0 || val > UINT16_MAX) {
        return -1;
    }

    *result = (uint16_t)val;
    return 0;
}

int main(int argc, const char* argv[])
{
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;

    if (argc < 3) {
        printf("usage: echoitn [TEST_PORT] [num_processes]\n");
        exit(-1);
    }

    uint16_t port;
    if (parse_port(argv[1], &port) != 0) {
        fprintf(stderr, "Error: Invalid port number '%s'. Must be 1-65535.\n", argv[1]);
        exit(-1);
    }

    int num_procs;
    if (parse_positive_int(argv[2], &num_procs) != 0) {
        fprintf(stderr, "Error: Invalid process count '%s'. Must be a positive integer.\n", argv[2]);
        exit(-1);
    }

    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;

    int sock = socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC, IPPROTO_UDP);
    if (sock < 0) {
        perror("socket creation failed");
        return 1;
    }

    setbufsize(sock, 2048*256);

    if (bind(sock, (struct sockaddr *)&sa, sizeof sa)) {
        perror("bind failure");
        close(sock);
        return 1;
    }

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe creation failed");
        close(sock);
        return 1;
    }

    pid_t* children = malloc(num_procs * sizeof(pid_t));
    if (children == NULL) {
        perror("Could not allocate memory for child PIDs");
        close(pipefd[0]);
        close(pipefd[1]);
        close(sock);
        return 1;
    }

    for (int i = 0; i < num_procs; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "Could not fork process %d: %s\n", i, strerror(errno));

            /* Close write end so already-forked children see EOF and exit. */
            close(pipefd[1]);

            for (int j = 0; j < i; j++) {
                waitpid(children[j], NULL, 0);
            }

            free(children);
            close(pipefd[0]);
            close(sock);
            return 1;
        }

        if (pid == 0) {
            /* Child: keep the socket and the read end of the death-pipe. */
            free(children);
            close(pipefd[1]);

            int ret = echo_function(sock, pipefd[0]);

            close(pipefd[0]);
            close(sock);
            exit(ret);
        }

        children[i] = pid;
    }

    /* parent: close read end, keep write end open as the sentinel. */
    close(pipefd[0]);

    struct sigaction sa_act = {0};
    sa_act.sa_handler = signal_handler;
    sigemptyset(&sa_act.sa_mask);
    sa_act.sa_flags = 0;
    sigaction(SIGINT, &sa_act, NULL);
    sigaction(SIGTERM, &sa_act, NULL);

    printf("Echo server running on port %u with %d processes...\n", port, num_procs);

    /* wait for children to exit. */
    pid_t pid;
    int status;
    while (!stop_flag) {
        pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("waitpid");
            break;
        }
        fprintf(stderr, "Child process %d exited\n", pid);
    }
    if (stop_flag) {
        close(pipefd[1]);
        while (waitpid(-1, &status, 0) > 0) {}
    }

    free(children);
    close(sock);
    return 0;
}

