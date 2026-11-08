/**
 * io_uring UDP echo server, based on
 * https://github.com/kryggird/udp-echo-server/
 */
#include <stdio.h>
#include <netinet/in.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <liburing.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

#define SERVER_PORT     1235
#define NBUFS           256
#define BUF_SIZE        2048
#define BUF_RXGID       1
#define BUF_TXGID       2
#define QUEUE_DEPTH     128

#define NUM_CQE_SLOTS    256

/* Choose zero copy or not */
const int is_zc = 0;

typedef union op_uint64 {
    uint64_t as_u64;
    struct {
        uint32_t is_rx; /* nonzero if recvmesg command */
        uint32_t idx;   /* Buffer index */
    };
} op_metadata_t;

typedef struct {
    size_t nbuf;
    size_t buf_size;
    size_t size;
    struct io_uring_buf_ring *metadata;
    unsigned char *buffers;
} buffer_pool_t;

typedef struct recvmsg_result_t {
    int      is_valid;
    size_t   idx;
    void    *msg_name;
    uint32_t msg_name_length;
    void    *msg_payload;
    unsigned int msg_payload_length;
} recvmsg_result_t;

typedef struct sendmsg_metadata_t {
    struct iovec iov;
    struct msghdr msghdr;
} sendmsg_metadata_t;

struct io_uring ring;
struct io_uring_buf_ring *br;

void panic(const char *str) {
    perror(str);
    exit(1);
}

/*
 * This function is responsible for setting up the main listening
 * socket used by the echo server.
 */
int setup_listening_socket(int port) {
    int sock;
    struct sockaddr_in srv_addr;

    sock = socket(PF_INET, SOCK_DGRAM|SOCK_CLOEXEC, 0);
    if (sock == -1)
        panic("socket()");

    int enable = 1;
    if (setsockopt(sock,
                   SOL_SOCKET, SO_REUSEADDR,
                   &enable, sizeof(int)) < 0)
        panic("setsockopt(SO_REUSEADDR)");


    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(port);
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* We bind to a port and turn this socket into a listening
     * socket.
     * */
    if (bind(sock,
             (const struct sockaddr *)&srv_addr,
             sizeof srv_addr) < 0)
        panic("bind()");

    return (sock);
}


void sigint_handler(int signo) {
    printf("^C pressed. Shutting down.\n");
    io_uring_queue_exit(&ring);
    exit(0);
}

buffer_pool_t setup_buffer_pool(
    struct io_uring *rp, size_t buf_size, size_t num_buffers) {

    void *buffers;
    size_t mmap_size = (buf_size + sizeof(struct io_uring_buf)) * num_buffers;
    void *metadata = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);

    if (metadata == NULL) {
        return (buffer_pool_t){0, buf_size, 0, NULL, NULL};
    }

    buffers = (char *)metadata + sizeof(struct io_uring_buf) * num_buffers;

    return (buffer_pool_t){num_buffers, buf_size, mmap_size, metadata, buffers};
}

unsigned char *buffer_pointer(buffer_pool_t *pool, size_t idx) {
    return pool->buffers + idx * pool->buf_size;
}

void add_buffer(buffer_pool_t *pool, size_t idx) {
    io_uring_buf_ring_add(pool->metadata, buffer_pointer(pool, idx),
                          pool->buf_size, idx,
                          io_uring_buf_ring_mask(pool->nbuf), idx);
}

int register_buffer_pool(struct io_uring* ring, buffer_pool_t *pool) {
    io_uring_buf_ring_init(pool->metadata);

    struct io_uring_buf_reg reg =
        (struct io_uring_buf_reg){.ring_addr = (unsigned long)pool->metadata,
                                  .ring_entries = pool->nbuf,
                                  .bgid = 0};

    int ret = io_uring_register_buf_ring(ring, &reg, 0);
    if (ret < 0) {
        return ret;
    }

    for (size_t idx = 0; idx < pool->nbuf; ++idx) {
        add_buffer(pool, idx);
    }
    io_uring_buf_ring_advance(pool->metadata, pool->nbuf);

    return 0;
}


int prep_recv_multishot(struct io_uring* rp, struct msghdr* msg, int fd_index) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(rp);

    io_uring_prep_recvmsg_multishot(sqe, fd_index, msg, MSG_TRUNC);
    sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    sqe->buf_group = 0;

    op_metadata_t meta = {.idx = 0, .is_rx = 1};
    io_uring_sqe_set_data64(sqe, meta.as_u64);
    return 0;
}


recvmsg_result_t validate_msg(struct io_uring_cqe* cqe,
                                  buffer_pool_t *pool,
                                  struct msghdr *msg) {
    size_t idx = ((size_t)cqe->flags) >> 16;

    struct io_uring_recvmsg_out* slot =
        io_uring_recvmsg_validate(buffer_pointer(pool, idx), cqe->res, msg);

    if (!slot || slot->namelen > msg->msg_namelen || slot->flags & MSG_TRUNC) {
        return (recvmsg_result_t){0};
    }

    return (recvmsg_result_t){
        .is_valid = 1,
        .idx = idx,
        .msg_name = io_uring_recvmsg_name(slot),
        .msg_name_length = slot->namelen,
        .msg_payload = io_uring_recvmsg_payload(slot, msg),
        .msg_payload_length =
            io_uring_recvmsg_payload_length(slot, cqe->res, msg)};
}


struct io_uring_sqe* maybe_submit_and_get_sqe(struct io_uring* ring) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);

    if (!sqe) {
        // Retry after clearing the submission ring
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }

    return sqe;
}

int prep_sendmsg(struct io_uring *ring,
                 sendmsg_metadata_t *meta,
                 recvmsg_result_t *res,
                 int fd_index) {
    struct io_uring_sqe *sqe = maybe_submit_and_get_sqe(ring);

    meta->iov = (struct iovec){
        .iov_base = res->msg_payload,
        .iov_len = res->msg_payload_length,
    };

    /* res->msg_name is the address we're replying to */
    meta->msghdr = (struct msghdr){
        .msg_name = res->msg_name,
        .msg_namelen = res->msg_name_length,
        .msg_iov = &(meta->iov),
        .msg_iovlen = 1,
        .msg_control = NULL,
        .msg_controllen = 0,
    };

    op_metadata_t op_meta = {.idx = (uint32_t)res->idx,
                             .is_rx = 0};

    if (is_zc) {
        io_uring_prep_sendmsg_zc(sqe, fd_index, &(meta->msghdr), 0);
    } else {
        io_uring_prep_sendmsg(sqe, fd_index, &(meta->msghdr), 0);
    }
    io_uring_sqe_set_data64(sqe, op_meta.as_u64);
    sqe->flags |= IOSQE_FIXED_FILE;

    return 0;
}

buffer_pool_t *setup_bufs(struct io_uring *rp) {
    static buffer_pool_t pool;
    pool = setup_buffer_pool(rp, BUFSIZ, NBUFS);
    register_buffer_pool(rp, &pool);
    return &pool;    
}

void server_loop(struct io_uring *rp, int server_socket, buffer_pool_t *pool) {
    struct msghdr msg;
    struct io_uring_cqe *cqe_slots[NUM_CQE_SLOTS];
    sendmsg_metadata_t sendmsg_slots[NUM_CQE_SLOTS];
    struct io_uring_sqe *sqe;
    recvmsg_result_t res;

    sqe = io_uring_get_sqe(rp);
    io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
    sqe->buf_group = 0;

    io_uring_register_files(&ring, &server_socket, 1);
       
    msg.msg_namelen = sizeof(struct sockaddr_storage);
    msg.msg_controllen = 0;

    prep_recv_multishot(rp, &msg, 0);
    
    if (io_uring_submit(&ring) < 0) {
        perror("Failed to submit SQE");
        exit(1);
    }
    for (;;) {
        char * pkt;
        size_t cqe_count;
        size_t cpq;
        int    buf_consumed;
        struct io_uring_cqe **cp;

        int ret;
        ret = io_uring_submit_and_wait(rp, 1);
        if (ret == -EINTR)
            continue;
        if (ret < 0)
            return;

        buf_consumed = 0;
        cqe_count = io_uring_peek_batch_cqe(rp, cqe_slots, NUM_CQE_SLOTS);
        for (cpq = cqe_count, cp = cqe_slots; cpq-- > 0; cp++) {
            op_metadata_t op_meta = {.as_u64 = (*cp)->user_data};
            int needs_rearm = !((*cp)->flags & IORING_CQE_F_MORE);
            if (op_meta.is_rx && needs_rearm) {
                prep_recv_multishot(rp, &msg, 0);
                io_uring_submit(rp);
                break;
            }
        }

        for (cpq = cqe_count, cp = cqe_slots; cpq-- > 0; cp++) {
            op_metadata_t op_meta = {.as_u64 = (*cp)->user_data};
            if (op_meta.is_rx) {
                res = validate_msg((*cp), pool, &msg);
                if (res.is_valid) {
                    if (res.idx < NUM_CQE_SLOTS) {
                        sendmsg_metadata_t *mp = sendmsg_slots + res.idx;
                        prep_sendmsg(rp, mp, &res, 0);
                    } else {
                        add_buffer(pool, op_meta.idx);
                        buf_consumed++;
                    }
                } else {
                    /* panic */
                }
            } else {
                /* Send message complete */
                add_buffer(pool, op_meta.idx);
                buf_consumed++;
            }
        }
        io_uring_buf_ring_advance(pool->metadata, buf_consumed);
        io_uring_cq_advance(rp, cqe_count);
    }
}

int main() {
    buffer_pool_t *poolp;
    int server_socket = setup_listening_socket(SERVER_PORT);
    signal(SIGINT, sigint_handler);
    io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
    poolp = setup_bufs(&ring);
    server_loop(&ring, server_socket, poolp);
 
   return 0;
}
