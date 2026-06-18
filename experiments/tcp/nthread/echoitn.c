#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>

static void setbufsize(int sock, int size)
{
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
}

void* echo_function(void* arg)
{
    int sock = *(int*)arg;
    char buf[2000];
    struct sockaddr_in sa;
    socklen_t sa_len;
    ssize_t msglen;

    for (;;) {
        sa_len = sizeof sa;
        msglen = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&sa, &sa_len);
        if (msglen > 0) {
            sendto(sock, buf, msglen, 0, (struct sockaddr *)&sa, sa_len);
        }
    }

    return NULL;
}

static int parse_positive_int(const char* str, int* result)
{
    char* endptr;
    long val = strtol(str, &endptr, 10);

    if (endptr == str || *endptr != '\0') {
        return -1; // Not a valid number
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
    // Port must be in valid range: 1-65535 (0 is reserved)
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
        printf("usage: echoitn [TEST_PORT] [num_threads]\n");
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
        return 1;
    }

    // Dynamically allocate thread handles based on num_threads
    pthread_t* threads = malloc(num_threads * sizeof(pthread_t));
    if (threads == NULL) {
        perror("Could not allocate memory for threads");
        return 1;
    }

    // Create the specified number of threads
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, echo_function, &sock)) {
            fprintf(stderr, "Could not create thread %d: %s\n", i, strerror(errno));

            // Wait for any threads that were successfully created
            for (int j = 0; j < i; j++) {
                pthread_join(threads[j], NULL);
            }

            free(threads);
            close(sock);
            return 1;
        }
    }

    printf("Echo server running on port %u with %d threads...\n", port, num_threads);

    // Wait for all threads to finish
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    close(sock);
    return 0;
}

