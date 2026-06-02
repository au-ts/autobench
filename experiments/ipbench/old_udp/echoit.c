#include <arpa/inet.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <string.h>

static void setbufsize(int sock, int size)
{
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
}

int main()
{
    char buf[2000];
    struct sockaddr_in sa;
    ssize_t msglen;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(1235);
    sa.sin_addr.s_addr = INADDR_ANY;
    int sock = socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC, IPPROTO_UDP);
    setbufsize(sock, 2048*256);
    if (bind(sock, (struct sockaddr *)&sa, sizeof sa)) {
        perror("bind failure");
        return 0;
    }

    socklen_t sa_len = sizeof(sa);
    if (getsockname(sock, (struct sockaddr*) &sa, &sa_len)) {
        perror("getsockname");
        return 1;
    }
    printf("echoit: listening on %s:%d\n", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));

    for (;;) {
        sa_len = sizeof sa;
        msglen = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&sa, &sa_len);
        sendto(sock, buf, msglen, 0, (struct sockaddr *)&sa, sa_len);
    }
}
