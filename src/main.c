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
#include "hexdump.h"
#include "http.h"
#include "socket.h"
#include "bufio.h"
#include "main.h"

/* HTML5 fallback flag (set via -a) */
bool html5_fallback = false;

/* Silent mode flag (set via -s, used during benchmarking) */
bool silent_mode = false;

/* Default token expiration time is 1 day */
int token_expiration_time = 24 * 60 * 60;

/* Root from which static files are served */
char *server_root;

/* ---------------------------------------------------------------------------
 * Per-connection state.  Lives from accept() until the connection closes.
 * --------------------------------------------------------------------------- */

struct conn {
    struct http_client client;
    int fd;
};

/* Shared epoll fd: workers all call epoll_wait on this directly. */
static int epoll_fd;
static int accept_fd;

/* Sentinel: accept-socket events use this as data.ptr to distinguish
 * them from client conn pointers. */
static struct conn accept_sentinel;

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

static void
accept_rearm(void)
{
    struct epoll_event ev = {
        .events   = EPOLLIN | EPOLLONESHOT,
        .data.ptr = &accept_sentinel,
    };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, accept_fd, &ev) == -1)
        perror("epoll_ctl MOD accept");
}

/* Accept all pending connections (loop until accept would block).
 * Called by whichever worker thread received the accept event. */
static void
do_accept(void)
{
    for (;;) {
        int fd = socket_accept_client(accept_fd);
        if (fd == -1) break;

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
    }
    accept_rearm();
}

/* ---------------------------------------------------------------------------
 * Worker thread: calls epoll_wait directly on the shared epoll_fd.
 * No queue, no mutex, no condvar — the kernel dispatches events.
 * EPOLLONESHOT guarantees each event goes to exactly one thread.
 * --------------------------------------------------------------------------- */

#define WORKER_BATCH 16

static void *
worker(void *arg)
{
    (void)arg;
    struct epoll_event events[WORKER_BATCH];

    for (;;) {
        int n = epoll_wait(epoll_fd, events, WORKER_BATCH, -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == &accept_sentinel) {
                do_accept();
                continue;
            }

            struct conn *conn = events[i].data.ptr;

            /* Drain any pipelined requests already buffered in bufio
             * before re-arming, to avoid an unnecessary epoll round-trip. */
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
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Server setup: create epoll, bind socket, spawn workers, wait.
 * --------------------------------------------------------------------------- */

static void
server_loop(char *port_string)
{
    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN) * 4;
    if (nthreads < 64) nthreads = 64;

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) { perror("epoll_create1"); return; }

    accept_fd = socket_open_bind_listen(port_string, 10000);
    if (accept_fd == -1) return;

    /* Register accept socket with EPOLLONESHOT so only one thread at a
     * time runs do_accept (avoids thundering herd on the shared listen). */
    struct epoll_event ev = {
        .events   = EPOLLIN | EPOLLONESHOT,
        .data.ptr = &accept_sentinel,
    };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, accept_fd, &ev) == -1) {
        perror("epoll_ctl ADD accept");
        return;
    }

    pthread_t *threads = malloc(nthreads * sizeof(pthread_t));
    if (!threads) { perror("malloc"); return; }

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
            perror("pthread_create");
            nthreads = i;
            break;
        }
    }

    /* Workers run forever; this thread just waits. */
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
