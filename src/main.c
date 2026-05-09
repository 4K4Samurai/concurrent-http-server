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
 * Work queue: a fixed-capacity ring buffer of accepted client fds.
 * The main thread enqueues; worker threads dequeue.
 * --------------------------------------------------------------------------- */

#define QUEUE_CAP 4096

static struct {
    int      fds[QUEUE_CAP];
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
wq_push(int fd)
{
    pthread_mutex_lock(&wq.lock);
    while (wq.count == QUEUE_CAP && !wq.shutdown)
        pthread_cond_wait(&wq.not_full, &wq.lock);

    if (wq.shutdown) {
        close(fd);
    } else {
        wq.fds[wq.tail] = fd;
        wq.tail = (wq.tail + 1) % QUEUE_CAP;
        wq.count++;
        pthread_cond_signal(&wq.not_empty);
    }
    pthread_mutex_unlock(&wq.lock);
}

/* Returns -1 when shut down and queue is empty. */
static int
wq_pop(void)
{
    pthread_mutex_lock(&wq.lock);
    while (wq.count == 0 && !wq.shutdown)
        pthread_cond_wait(&wq.not_empty, &wq.lock);

    int fd = -1;
    if (wq.count > 0) {
        fd = wq.fds[wq.head];
        wq.head = (wq.head + 1) % QUEUE_CAP;
        wq.count--;
        pthread_cond_signal(&wq.not_full);
    }
    pthread_mutex_unlock(&wq.lock);
    return fd;
}

/* ---------------------------------------------------------------------------
 * Worker thread: dequeues fds and handles HTTP transactions.
 * --------------------------------------------------------------------------- */

static void *
worker(void *arg)
{
    (void)arg;
    for (;;) {
        int fd = wq_pop();
        if (fd == -1)
            break;

        struct http_client client;
        http_setup_client(&client, bufio_create(fd));
        while (http_handle_transaction(&client))
            ;
        bufio_close(client.bufio);
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Main accept loop + thread pool management.
 * --------------------------------------------------------------------------- */

static void
server_loop(char *port_string)
{
    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nthreads < 1)
        nthreads = 4;

    wq_init();

    pthread_t *threads = malloc(nthreads * sizeof(pthread_t));
    if (!threads) {
        perror("malloc");
        return;
    }

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
            perror("pthread_create");
            nthreads = i;
            break;
        }
    }

    int accepting_socket = socket_open_bind_listen(port_string, 10000);
    while (accepting_socket != -1) {
        int fd = socket_accept_client(accepting_socket);
        if (fd == -1)
            break;
        wq_push(fd);
    }

    /* Signal workers to drain and exit. */
    pthread_mutex_lock(&wq.lock);
    wq.shutdown = true;
    pthread_cond_broadcast(&wq.not_empty);
    pthread_mutex_unlock(&wq.lock);

    for (int i = 0; i < nthreads; i++)
        pthread_join(threads[i], NULL);

    free(threads);
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
