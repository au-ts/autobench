#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <string.h>
#include <pthread.h>

static void setbufsize(int sock, int size)
{
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
}

// Thread function that contains the logic for receiving and echoing messages
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
        if (msglen > 0) { // Check if message was received
            sendto(sock, buf, msglen, 0, (struct sockaddr *)&sa, sa_len);
        }
    }

    return NULL;
}

int main()
{
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(1235);
    sa.sin_addr.s_addr = INADDR_ANY;
    int sock = socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC, IPPROTO_UDP);
    setbufsize(sock, 2048*256);
    if (bind(sock, (struct sockaddr *)&sa, sizeof sa)) {
        perror("bind failure");
        return 0;
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

    // Wait for both threads to finish (though they are designed to run indefinitely here)
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);

    return 0;
}
