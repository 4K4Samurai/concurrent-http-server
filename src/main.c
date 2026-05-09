/*
 * Skeleton files for personal server assignment.
 *
 * @author Godmar Back
 * written for CS3214, Spring 2018.
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>
#include "buffer.h"
#include "hexdump.h"
#include "http.h"
#include "socket.h"
#include "bufio.h"
#include "main.h"

/* Implement HTML5 fallback.
 * If HTML5 fallback is implemented and activated, the server should
 * treat requests to non-API paths specially.
 * If the requested file is not found, the server will serve /index.html
 * instead; that is, it should treat the request as if it
 * had been for /index.html instead.
 */
bool html5_fallback = false;

// silent_mode. During benchmarking, this will be true
bool silent_mode = false;

// default token expiration time is 1 day
int token_expiration_time = 24 * 60 * 60;

// root from which static files are served
char *server_root;

/* ---------------------------------------------------------------------------
 * Per-connection state.  Lives from accept() until the connection closes.
 * --------------------------------------------------------------------------- */

struct conn {
    struct http_client client;
    int fd;
};

/* epoll fd — global so worker threads can re-arm connections. */
static int epoll_fd;

/* ---------------------------------------------------------------------------
 * Work queue: fixed-capacity ring buffer of conn pointers.
 * Main/epoll thread enqueues; worker threads dequeue.
 * --------------------------------------------------------------------------- */

#define QUEUE_CAP 4096

static struct {
    struct conn *conns[QUEUE_CAP];
    int      head, tail, count;
    bool     shutdown;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} wq;

static void
wq_init(void)
{
    wq.head = wq.tail = wq.count = 0;
    wq.shutdown = false;
    pthread_mutex_init(&wq.lock, NULL);
    pthread_cond_init(&wq.not_empty, NULL);
    pthread_cond_init(&wq.not_full, NULL);
}

static void
wq_push(struct conn *conn)
{
    pthread_mutex_lock(&wq.lock);
    while (wq.count == QUEUE_CAP && !wq.shutdown)
        pthread_cond_wait(&wq.not_full, &wq.lock);

    if (wq.shutdown) {
        bufio_close(conn->client.bufio);
        free(conn);
    } else {
        wq.conns[wq.tail] = conn;
        wq.tail = (wq.tail + 1) % QUEUE_CAP;
        wq.count++;
        pthread_cond_signal(&wq.not_empty);
    }
    pthread_mutex_unlock(&wq.lock);
}

/* Returns NULL when shut down and queue is drained. */
static struct conn *
wq_pop(void)
{
    pthread_mutex_lock(&wq.lock);
    while (wq.count == 0 && !wq.shutdown)
        pthread_cond_wait(&wq.not_empty, &wq.lock);

    struct conn *conn = NULL;
    if (wq.count > 0) {
        conn = wq.conns[wq.head];
        wq.head = (wq.head + 1) % QUEUE_CAP;
        wq.count--;
        pthread_cond_signal(&wq.not_full);
    }
    pthread_mutex_unlock(&wq.lock);
    return conn;
}

/* ---------------------------------------------------------------------------
 * Worker thread.
 *
 * Handles exactly ONE http_handle_transaction per wakeup, then either:
 *   - re-arms the fd in epoll (keep-alive, no buffered data), or
 *   - re-queues itself (keep-alive, pipelined data already buffered), or
 *   - closes the connection.
 * --------------------------------------------------------------------------- */

static void
conn_rearm(struct conn *conn)
{
    struct epoll_event ev = {
        .events   = EPOLLIN | EPOLLONESHOT,
        .data.ptr = conn,
    };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev) == -1) {
        bufio_close(conn->client.bufio);
        free(conn);
    }
}

static void *
worker(void *arg)
{
    (void)arg;
    for (;;) {
        struct conn *conn = wq_pop();
        if (!conn)
            break;

        bool keep = http_handle_transaction(&conn->client);
        if (!keep) {
            bufio_close(conn->client.bufio);
            free(conn);
            continue;
        }

        /* Keep-alive: if a pipelined request is already in bufio's buffer,
         * dispatch directly back to the thread pool to avoid missing it
         * (epoll won't fire again since the socket buffer may be empty). */
        if (bufio_has_data(conn->client.bufio))
            wq_push(conn);
        else
            conn_rearm(conn);
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * epoll accept loop + thread pool management.
 * --------------------------------------------------------------------------- */

/* Sentinel: the accept socket's epoll event uses this as data.ptr so we can
 * distinguish it from client conn pointers. */
static struct conn accept_sentinel;

static void
server_loop(char *port_string)
{
    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nthreads < 1)
        nthreads = 4;

    wq_init();

    pthread_t *threads = malloc(nthreads * sizeof(pthread_t));
    if (!threads) { perror("malloc"); return; }

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
            perror("pthread_create");
            nthreads = i;
            break;
        }
    }

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) { perror("epoll_create1"); return; }

    int accepting_socket = socket_open_bind_listen(port_string, 10000);
    if (accepting_socket == -1) return;

    /* Register the accept socket — level-triggered, no ONESHOT. */
    struct epoll_event ev = {
        .events   = EPOLLIN,
        .data.ptr = &accept_sentinel,
    };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, accepting_socket, &ev) == -1) {
        perror("epoll_ctl ADD accept");
        return;
    }

    #define MAX_EVENTS 64
    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == &accept_sentinel) {
                /* New connection. */
                int fd = socket_accept_client(accepting_socket);
                if (fd == -1) continue;

                struct conn *conn = malloc(sizeof *conn);
                if (!conn) { close(fd); continue; }

                conn->fd = fd;
                http_setup_client(&conn->client, bufio_create(fd));

                struct epoll_event cev = {
                    .events   = EPOLLIN | EPOLLONESHOT,
                    .data.ptr = conn,
                };
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &cev) == -1) {
                    perror("epoll_ctl ADD client");
                    bufio_close(conn->client.bufio);
                    free(conn);
                }
            } else {
                /* Existing connection has data — hand off to thread pool. */
                struct conn *conn = events[i].data.ptr;
                wq_push(conn);
            }
        }
    }

    /* Shutdown: wake all workers and wait for them. */
    pthread_mutex_lock(&wq.lock);
    wq.shutdown = true;
    pthread_cond_broadcast(&wq.not_empty);
    pthread_mutex_unlock(&wq.lock);

    for (int i = 0; i < nthreads; i++)
        pthread_join(threads[i], NULL);

    free(threads);
    close(epoll_fd);
}

static void
usage(char * av0)
{
    fprintf(stderr, "Usage: %s -p port [-R rootdir] [-h] [-e seconds]\n"
        "  -p port      port number to bind to\n"
        "  -R rootdir   root directory from which to serve files\n"
        "  -e seconds   expiration time for tokens in seconds\n"
        "  -a           enable HTML5 fallback\n"
        "  -h           display this help\n"
        , av0);
    exit(EXIT_FAILURE);
}

int
main(int ac, char *av[])
{
    int opt;
    char *port_string = NULL;
    while ((opt = getopt(ac, av, "ahp:R:se:")) != -1) {
        switch (opt) {
            case 'a':
                html5_fallback = true;
                break;

            case 'p':
                port_string = optarg;
                break;

            case 'e':
                token_expiration_time = atoi(optarg);
                fprintf(stderr, "token expiration time is %d\n", token_expiration_time);
                break;

            case 's':
                silent_mode = true;
                break;

            case 'R':
                server_root = optarg;
                break;

            case 'h':
            default:    /* '?' */
                usage(av[0]);
        }
    }

    if (port_string == NULL)
        usage(av[0]);

    /* We ignore SIGPIPE to prevent the process from terminating when it tries
     * to send data to a connection that the client already closed.
     * This may happen, in particular, in bufio_sendfile.
     */
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "Using port %s\n", port_string);
    server_loop(port_string);
    exit(EXIT_SUCCESS);
}
