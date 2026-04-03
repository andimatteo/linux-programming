#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <liburing.h>
#define NUM_THREADS 8
#define CACHELINE 64
#define LISTEN_PORT 4242
#define MAX_CONNS 65536
static int set_thread_affinity(pthread_t thread, int cpu_id)
{
    cpu_set_t cpuset;
    if (cpu_id < 0)
        return EINVAL;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    return pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);
}
/*
 * NETWORKING STUFF */
int create_listen_socket(int port) {
    int fd, one = 1;
    struct sockaddr_in addr;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        close(fd);
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0) {
        close(fd);
        return -1;
    }
    return fd; // listening socket
}
// available operations
enum op_type {
    OP_ACCEPT = 1,
    OP_RECV   = 2,
    OP_SEND   = 3,
    OP_CLOSE  = 4,
};
struct conn {
    int fd;
    char buf[4096];
};
static struct conn conns[MAX_CONNS];
static inline uint64_t pack_ud(int op, int id)
{
    return ((uint64_t)op << 32) | (uint32_t)id;
}
static inline int ud_op(uint64_t ud) { return (int)(ud >> 32); }
static inline int ud_id(uint64_t ud) { return (int)(ud & 0xffffffffu); }
static int alloc_conn_slot(void) {
    int i;
    for (i = 1; i < MAX_CONNS; i++) {
        if (conns[i].fd == 0) return i;
    }
    return -1;
}
static void submit_accept(struct io_uring *ring, int lsock) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
        return;
    io_uring_prep_accept(sqe, lsock, NULL, NULL, 0);
    sqe->user_data = pack_ud(OP_ACCEPT, 0);
}
static void submit_recv(struct io_uring *ring, int id) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
        return;
    io_uring_prep_recv(sqe, conns[id].fd, conns[id].buf, sizeof(conns[id].buf), 0);
    sqe->user_data = pack_ud(OP_RECV, id);
}
static void submit_send(struct io_uring *ring, int id, int len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
        return;
    io_uring_prep_send(sqe, conns[id].fd, conns[id].buf, len, 0);
    sqe->user_data = pack_ud(OP_SEND, id);
}
static void submit_close(struct io_uring *ring, int id) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
        return;
    io_uring_prep_close(sqe, conns[id].fd);
    sqe->user_data = pack_ud(OP_CLOSE, id);
}
/*
 * IO URING STUFF */
int init_ring(struct io_uring *ring) {
    struct io_uring_params p;
    int ret;
    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CLAMP;
    ret = io_uring_queue_init_params(256, ring, &p);
    if (ret < 0)
        return ret;
    return 0;
}
static inline struct io_uring_cqe *wait_cqe_fast(struct io_uring *ring)
{
    struct io_uring_cqe *cqe;
    unsigned head;
    int ret;
    io_uring_for_each_cqe(ring, head, cqe)
        return cqe;
    ret = io_uring_wait_cqe(ring, &cqe);
    if (ret) {
        fprintf(stderr, "wait cqe failed: %s (%d)\n", strerror(-ret), ret);
        return NULL;
    }
    return cqe;
}
/*
 * WORKER STUFF */
struct thread_info {
    int id;                 // thread_id
    struct io_uring ring;   // thread io_uring
    int ls;                 // thread listening socket
    int cpu_id;
} alignas(CACHELINE);
static void *worker_fn(void *arg)
{
    /*
     * init worker structure */
    struct thread_info * t = (struct thread_info *)arg;
    int ret;
    ret = set_thread_affinity(pthread_self(), t->cpu_id);
    if (ret != 0) {
        fprintf(stderr, "worker %d: set affinity cpu %d failed: %s\n",
                t->id, t->cpu_id, strerror(ret));
        return NULL;
    }
    t->ls = create_listen_socket(LISTEN_PORT);
    if (t->ls < 0) {
        fprintf(stderr, "worker %d: create_listen_socket failed: %s\n",
                t->id, strerror(errno));
        return NULL;
    }
    ret = init_ring(&t->ring);
    if (ret < 0) {
        fprintf(stderr, "worker %d: init_ring failed: %s (%d)\n",
                t->id, strerror(-ret), ret);
        close(t->ls);
        t->ls = -1;
        return NULL;
    }
    submit_accept(&t->ring, t->ls);
    io_uring_submit(&t->ring);
    /* worker loop */
    while (1) {
        struct io_uring_cqe *cqe = wait_cqe_fast(&t->ring);
        uint64_t ud;
        int op, id, res;
        if (!cqe)
            break;
        ud = cqe->user_data;
        op = ud_op(ud);
        id = ud_id(ud);
        res = cqe->res;
        io_uring_cqe_seen(&t->ring, cqe);
        if (res < 0) {
            // errore op: chiudi conn se serve
            if (id > 0 && id < MAX_CONNS && conns[id].fd > 0)
                submit_close(&t->ring, id);
            // rearm accept se era accept
            if (op == OP_ACCEPT)
                submit_accept(&t->ring, t->ls);
            io_uring_submit(&t->ring);
            continue;
        }
        switch (op) {
            case OP_ACCEPT: {
                                int cfd = res;
                                int slot = alloc_conn_slot();
                                if (slot < 0) {
                                    close(cfd); // could not accept connection
                                } else {
                                    conns[slot].fd = cfd;
                                    submit_recv(&t->ring, slot);
                                }
                                // submit next listen
                                submit_accept(&t->ring, t->ls);
                                break;
                            }
            case OP_RECV:
                            if (res == 0) {
                                // peer ha chiuso
                                submit_close(&t->ring, id);
                            } else {
                                // echo: rimanda gli stessi bytes
                                submit_send(&t->ring, id, res);
                            }
                            break;
            case OP_SEND:
                            // dopo il send, torna a ricevere
                            submit_recv(&t->ring, id);
                            break;
            case OP_CLOSE:
                            if (id > 0 && id < MAX_CONNS)
                                conns[id].fd = 0;  // libera slot
                            break;
            default:
                            break;
        }
        io_uring_submit(&t->ring);
    }
    io_uring_queue_exit(&t->ring);
    if (t->ls >= 0)
        close(t->ls);
    return NULL;
}

/*
 * MAIN THREAD */
int main(int argc, char * argv[]) {
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int nthreads = NUM_THREADS;
    int i, ret;
    (void)argc;
    (void)argv;
    // create the different threads
    if (ncpu <= 0) {
        fprintf(stderr, "sysconf(_SC_NPROCESSORS_ONLN) failed\n");
        return 1;
    }
    // create the different threads
    pthread_t *threads = calloc(nthreads, sizeof(*threads));
    struct thread_info * args = NULL;
    ret = posix_memalign((void **)&args, CACHELINE, nthreads * sizeof(*args));
    if (ret != 0 || !threads || !args) {
        fprintf(stderr, "allocation failed\n");
        free(threads);
        free(args);
        return 1;
    }
    memset(args, 0, nthreads * sizeof(*args));
    // start workers
    for (i = 0; i < nthreads; i++) {
        args[i].id = i;
        args[i].cpu_id = i % (int)ncpu;
        args[i].ls = -1;
        ret = pthread_create(&threads[i], NULL, worker_fn, &args[i]);
        // if not successfull then stop here
        if (ret != 0) {
            fprintf(stderr, "pthread_create[%d] failed: %s\n", i, strerror(ret));
            nthreads = i;
            break;
        }
    }
    // wait for completion of all threads
    for (i = 0; i < nthreads; i++) {
        pthread_join(threads[i], NULL);
    }
    free(threads);
    free(args);
    return 0;
}
