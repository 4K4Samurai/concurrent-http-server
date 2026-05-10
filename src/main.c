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

bool html5_fallback = false;
bool silent_mode = false;
int token_expiration_time = 24 * 60 * 60;
char *server_root;

/* ---------------------------------------------------------------------------
 * Per-connection state.  Lives from accept() until the connection closes.
 * --------------------------------------------------------------------------- */

struct conn {
    struct http_client client;
    int fd;
};

static int epoll_fd;

/* ---------------------------------------------------------------------------
 * Per-worker queues with sticky scheduling.
 *
 * The single epoll thread distributes connection events to per-worker queues
 * by hashing the fd. This gives:
 *   - 1 epoll thread (no kernel epoll wait-queue overhead)
 *   - 1 producer + 1 consumer per queue (minimal mutex contention)
 *   - Cache locality (same conn always handled by same worker)
 * --------------------------------------------------------------------------- */

#define NWORKERS         128      /* must be power of 2 for fast mod */
#define WORKER_QUEUE_CAP 4096

struct worker_state {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    struct conn    *queue[WORKER_QUEUE_CAP];
    int             head, tail, count;
    bool            shutdown;
};

static struct worker_state workers[NWORKERS];

static inline int
worker_for_fd(int fd)
{
    return fd & (NWORKERS - 1);
}

static void
dispatch(struct conn *conn)
{
    struct worker_state *w = &workers[worker_for_fd(conn->fd)];
    pthread_mutex_lock(&w->lock);
    if (w->count < WORKER_QUEUE_CAP) {
        w->queue[w->tail] = conn;
        w->tail = (w->tail + 1) % WORKER_QUEUE_CAP;
        w->count++;
        pthread_cond_signal(&w->not_empty);
    }
    /* Queue full: drop event. Connection still has EPOLLONESHOT armed,
     * the kernel will redeliver on next epoll_wait. */
    pthread_mutex_unlock(&w->lock);
}

static struct conn *
worker_pop(struct worker_state *w)
{
    pthread_mutex_lock(&w->lock);
    while (w->count == 0 && !w->shutdown)
        pthread_cond_wait(&w->not_empty, &w->lock);

    struct conn *conn = NULL;
    if (w->count > 0) {
        conn = w->queue[w->head];
        w->head = (w->head + 1) % WORKER_QUEUE_CAP;
        w->count--;
    }
    pthread_mutex_unlock(&w->lock);
    return conn;
}

/* ---------------------------------------------------------------------------
 * Worker thread: pulls from its own queue, handles all pipelined requests
 * inline (sticky scheduling makes this fair — other connections go to other
 * workers), then re-arms via epoll for the next round-trip.
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
worker_main(void *arg)
{
    struct worker_state *w = arg;
    for (;;) {
        struct conn *conn = worker_pop(w);
        if (!conn) break;

        /* Drain pipelined requests for this connection inline.
         * Sticky scheduling guarantees other connections go to other
         * workers, so this can't starve them. */
        for (;;) {
            bool keep = http_handle_transaction(&conn->client);
            if (!keep) {
                bufio_close(conn->client.bufio);
                free(conn);
                break;
            }
            if (!bufio_has_data(conn->client.bufio)) {
                conn_rearm(conn);
                break;
            }
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * epoll thread: only one. Accepts new connections and dispatches events
 * to per-worker queues.
 * --------------------------------------------------------------------------- */

static struct conn accept_sentinel;

static void
server_loop(char *port_string)
{
    /* Initialize per-worker queues and start workers. */
    for (int i = 0; i < NWORKERS; i++) {
        struct worker_state *w = &workers[i];
        w->head = w->tail = w->count = 0;
        w->shutdown = false;
        pthread_mutex_init(&w->lock, NULL);
        pthread_cond_init(&w->not_empty, NULL);
        if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
            perror("pthread_create");
            return;
        }
    }

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) { perror("epoll_create1"); return; }

    int accepting_socket = socket_open_bind_listen(port_string, 10000);
    if (accepting_socket == -1) return;

    struct epoll_event ev = {
        .events   = EPOLLIN,
        .data.ptr = &accept_sentinel,
    };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, accepting_socket, &ev) == -1) {
        perror("epoll_ctl ADD accept");
        return;
    }

    #define MAX_EVENTS 256
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
                /* Single accept per event; the listen socket is
                 * level-triggered so epoll_wait fires again if there
                 * are more pending connections. socket_accept_client
                 * uses a blocking accept(), so we cannot loop here. */
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
                    bufio_close(conn->client.bufio);
                    free(conn);
                }
            } else {
                dispatch(events[i].data.ptr);
            }
        }
    }

    /* Shutdown */
    for (int i = 0; i < NWORKERS; i++) {
        struct worker_state *w = &workers[i];
        pthread_mutex_lock(&w->lock);
        w->shutdown = true;
        pthread_cond_signal(&w->not_empty);
        pthread_mutex_unlock(&w->lock);
    }
    for (int i = 0; i < NWORKERS; i++)
        pthread_join(workers[i].thread, NULL);
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
            default:
                usage(av[0]);
        }
    }

    if (port_string == NULL)
        usage(av[0]);

    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "Using port %s\n", port_string);
    server_loop(port_string);
    exit(EXIT_SUCCESS);
}
