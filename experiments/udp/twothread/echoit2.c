#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <string.h>
#include <pthread.h>

#define DEFAULT_PORT 1235
// largest possible UDP payload, so no datagram is ever truncated
#define MAX_UDP_MSG 65536

static void setbufsize(int sock, int size)
{
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
}

// Thread function that contains the logic for receiving and echoing messages
void* echo_function(void* arg)
{
    int sock = *(int*)arg;
    char* buf = malloc(MAX_UDP_MSG);
    struct sockaddr_in sa;
    socklen_t sa_len;
    ssize_t msglen;

    if (buf == NULL) {
        perror("malloc");
        exit(1);
    }

    for (;;) {
        sa_len = sizeof sa;
        msglen = recvfrom(sock, buf, MAX_UDP_MSG, 0, (struct sockaddr *)&sa, &sa_len);
        if (msglen >= 0) { // Check if message was received
            sendto(sock, buf, msglen, 0, (struct sockaddr *)&sa, sa_len);
        }
    }

    return NULL;
}

// usage: echoit2 [port]
int main(int argc, const char* argv[])
{
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: Invalid port number '%s'\n", argv[1]);
        return 1;
    }

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
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

    pthread_t thread1, thread2;
    // Create two threads running the echo_function
    if (pthread_create(&thread1, NULL, echo_function, &sock)) {
        perror("Could not create thread 1");
        return 1;
    }
    if (pthread_create(&thread2, NULL, echo_function, &sock)) {
        perror("Could not create thread 2");
        return 1;
    }

    printf("Echo server running on port %d with 2 threads...\n", port);
    fflush(stdout);

    // Wait for both threads to finish (though they are designed to run indefinitely here)
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);

    return 0;
}
