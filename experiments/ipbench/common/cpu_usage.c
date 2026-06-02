#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define WHOAMI "100 IPBENCH V1.0\n"
#define HELLO "HELLO\n"
#define OK_READY "200 OK (Ready to go)\n"
#define LOAD "LOAD cpu_target_lukem\n"
#define OK "200 OK\n"
#define SETUP "SETUP args::\"\"\n"
#define START "START\n"
#define STOP "STOP\n"
#define QUIT "QUIT\n"
#define RESPONSE "220 VALID DATA (Data to follow)\n"    \
    "Content-length: %d\n"                              \
    "%s\n"
#define IDLE_FORMAT ",%ld,%ld"
#define ERROR "400 ERROR\n"

#define msg_match(msg, match) (strncmp(msg, match, strlen(match))==0)

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define RES(x, y, z) "220 VALID DATA (Data to follow)\n"    \
    "Content-length: "STR(x)"\n"\
    ","STR(y)","STR(z)

#define MAX_EVENTS 16
#define RECV_BUF_SIZE 4096
#define PORT 1236


struct state
{
    size_t partial_len;
    char partial[RECV_BUF_SIZE*2];

    float start_uptime;
    float start_idle;
};

static bool uptime(float *uptime, float *idle)
{
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) {
        perror("fopen");
        return false;
    }

    if (fscanf(fp, "%f %f", uptime, idle) != 2) {
        perror("fscanf");
        fclose(fp);
        return false;
    }

    fclose(fp);
    return true;
}

static int util_send(int socket, const char *msg, int len)
{
    printf("Util send: %.*s", len, msg);
    int nsent = send(socket, msg, len, 0);
    if (nsent < len) { perror("send"); return -1; }
    return nsent;
}

static ssize_t consume(struct state *state, int socket)
{
    char *buf = state->partial;
    size_t remaining = state->partial_len;

    while (remaining > 0) {
        // Find newline (or end of buffer)
        ssize_t newline_pos = 0;
        while (newline_pos < remaining && buf[newline_pos] != '\n') {
            newline_pos++;
        }
        // If no newline, wait for more data
        if (newline_pos == remaining) {
            break;
        }

        // Process message
        printf("Util recv: %.*s\n", (int)newline_pos, buf);

        if (msg_match(buf, HELLO)) {
            int nsent = util_send(socket, OK_READY, strlen(OK_READY));
            if (nsent < strlen(OK_READY)) { return -1; }

        } else if (msg_match(buf, LOAD)) {
            int nsent = util_send(socket, OK, strlen(OK));
            if (nsent < strlen(OK)) { return -1; }

        } else if (msg_match(buf, SETUP)) {
            int nsent = util_send(socket, OK, strlen(OK));
            if (nsent < strlen(OK)) { return -1; }

        } else if (msg_match(buf, START)) {
            bool ok = uptime(&state->start_uptime, &state->start_idle);
            if (!ok) {
                int nsent = util_send(socket, ERROR, strlen(ERROR));
                if (nsent < strlen(ERROR)) { return -1; }
                return -1;
            }
        } else if (msg_match(buf, STOP)) {
            float stop_uptime, stop_idle;
            bool ok = uptime(&stop_uptime, &stop_idle);
            if (!ok) {
                int nsent = util_send(socket, ERROR, strlen(ERROR));
                if (nsent < strlen(ERROR)) { return -1; }
                return -1;
            }

            float duptime = stop_uptime - state->start_uptime;
            float didle = stop_idle - state->start_idle;

            int content_len = snprintf(NULL, 0, ",%f,%f", didle, duptime) + 1;
            char *content = malloc(content_len);
            snprintf(content, content_len, ",%f,%f", didle, duptime);

            int response_len = snprintf(NULL, 0, RESPONSE, content_len, content);
            char *response = malloc(response_len + 1);
            snprintf(response, response_len + 1, RESPONSE, content_len, content);

            int nsent = util_send(socket, response, response_len);

            free(content);
            free(response);

            if (nsent < response_len) { return -1; }
            return -1;
        } else if (msg_match(buf, QUIT)) {
            return -1;
        } else {
            printf("Unknown message: %.*s\n", (int)newline_pos, buf);
            return -1;
        }

        newline_pos++; // Skip newline
        remaining -= newline_pos;
        buf += newline_pos;
    }

    return state->partial_len - remaining;
}

static bool on_recv(struct state *state, int socket, char *buf, ssize_t len)
{
    // Convert stream-based protocol to message-based protocol
    // Could probably do this with less copies but ok for utilization server.
    memcpy(state->partial + state->partial_len, buf, len);
    state->partial_len += len;

    ssize_t consumed = consume(state, socket);
    if (consumed < 0) {
        return false;
    }
    memcpy(state->partial, state->partial + consumed, state->partial_len - consumed);
    state->partial_len -= consumed;
    return true;
}

int main()
{
    // create socket
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);

    // bind to port
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
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
    printf("Utilization socket: listening on %s:%d\n",
           inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
    

    while(1) {
        // new connection
        struct sockaddr_in conn_sa;
        socklen_t conn_sa_len = sizeof(conn_sa);
        int conn_sock = accept(listen_sock, (struct sockaddr*) &conn_sa, &conn_sa_len);
        if (conn_sock == -1) {
            perror("accept");
            return 1;
        }
        printf("Got utilization connection from %s:%d\n",
               inet_ntoa(conn_sa.sin_addr), ntohs(conn_sa.sin_port));

        struct state state;
        state.partial_len = 0;

        printf("Sent WHOAMI\n");
        int nsent = send(conn_sock, WHOAMI, strlen(WHOAMI), 0);
        if (nsent < strlen(WHOAMI)) { perror("send"); return 1; }

        while (1) {
            // read data
            char buf[RECV_BUF_SIZE];
            ssize_t len = recv(conn_sock, buf, RECV_BUF_SIZE, 0);
            if (len < 0) {
                perror("read");
                return 1;
            } else if (len == 0) {
                printf("Utilization connection closed\n");
                break;
            }

            bool ok = on_recv(&state, conn_sock, buf, len);
            if (!ok) {
                printf("Closing utilization socket\n");
                close(conn_sock);
                break;
            }
        }
    }
}
