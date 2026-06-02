#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <string.h>

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

int main()
{
    struct sockaddr_in sa;
    socklen_t sa_len;
    ssize_t msglen;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(1235);
    sa.sin_addr.s_addr = INADDR_ANY;
    int sock = socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC, IPPROTO_UDP);
    if (bind(sock, (struct sockaddr *)&sa, sizeof sa)) {
        perror("bind failure");
        return 0;
    }
    init_state();

    for (;;) { 
        int r = recvmmsg(sock, messages, MAX_MSG, MSG_WAITFORONE, NULL);
        sendmmsg(sock, messages, r, 0);
    }
}
