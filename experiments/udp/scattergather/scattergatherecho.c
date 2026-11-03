#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <string.h>

#define DEFAULT_PORT 1235
#define MAX_MSG 512
#define MTU (2048-64*2)
static struct sockaddr_in addrs[MAX_MSG];
static unsigned char buffers[MAX_MSG][MTU];
static struct mmsghdr messages[MAX_MSG];
static struct iovec iovecs[MAX_MSG];

static void setbufsize(int sock, int size)
{
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
}

static void init_state(void) {
    int i;
    struct mmsghdr *msg = messages;
    struct iovec *iovec = iovecs;
    for (i = 0; i < MAX_MSG; i++) {
        msg->msg_hdr.msg_name = addrs + i;
        msg->msg_hdr.msg_namelen = sizeof (struct sockaddr_in);
        msg->msg_hdr.msg_iov = iovec;
        msg++->msg_hdr.msg_iovlen = 1;
        iovec->iov_base = buffers[i];
        iovec++->iov_len = MTU;
    }
}

// usage: scattergatherecho [port]
int main(int argc, const char *argv[])
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
    init_state();
    printf("scattergatherecho: listening on port %d\n", port);
    fflush(stdout);

    for (;;) {
        int r = recvmmsg(sock, messages, MAX_MSG, MSG_WAITFORONE, NULL);
        if (r <= 0)
            continue;

        // echo only what was received, not the whole buffer
        for (int i = 0; i < r; i++)
            iovecs[i].iov_len = messages[i].msg_len;

        int sent = 0;
        while (sent < r) {
            int k = sendmmsg(sock, &messages[sent], r - sent, 0);
            if (k < 0) {
                if (errno == EINTR)
                    continue;
                // this datagram can't be sent; skip it, keep the rest
                sent++;
                continue;
            }
            sent += k;
        }

        for (int i = 0; i < r; i++) {
            iovecs[i].iov_len = MTU;
            messages[i].msg_hdr.msg_namelen = sizeof (struct sockaddr_in);
        }
    }
}
