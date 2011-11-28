/*
 * Copyright (C) 2005-2011 Cleversafe, Inc. All rights reserved.
 *
 * Contact Information:
 * Cleversafe, Inc.
 * 222 South Riverside Plaza
 * Suite 1700
 * Chicago, IL 60606, USA
 *
 * licensing@cleversafe.com
 *
 * END-OF-HEADER
 * -----------------------
 * @author: jyoung
 *
 * Date: Nov 18, 2011
 * ---------------------
 *
 *  Sets up a server to listen for connections, and immediately closes them.
 *  Concurrently runs a client that connects to the server, measuring the
 *  time it takes to set up the connection
 *
 *  Usage: ./packetloss [SERVER_HOST] [CLIENT_HOST] [PORT]
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

/* http://sourcefrog.net/weblog/software/languages/C/unused.html */
#ifdef UNUSED
#elif defined(__GNUC__)
#    define UNUSED(x) UNUSED_ ## x __attribute__((unused))
#elif defined(__LCLINT__)
#    define UNUSED(x) /*@unused@*/ x
#else
#    define UNUSED(x) x
#endif

#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#define FREE(r) do {if(r){free(r); r = NULL;}}while(0)
#define CLIENT_FREE(r) do{if(r){client_free(r); r = NULL;}}while(0)
#define FREEADDRINFO(r) do{if(r){freeaddrinfo(r); r = NULL;}}while(0)

#define LOG(fmt, ...) do { \
        fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
    } while(0)

#define LOG_ERRNO(fmt, ...) do { \
        LOG(fmt ": %s", ##__VA_ARGS__, strerror(errno)); \
    } while(0)

#define LOG_RETURN(err, fmt, ...) do { \
        LOG(fmt ": %s", ##__VA_ARGS__, strerror(-err)); \
    } while(0)

#define LOG_GAIERR(err, fmt, ...) do { \
        LOG(fmt ": %s", ##__VA_ARGS__, gai_strerror(err)); \
    } while(0)

#define XLOG_RETURN(err, ...) do { \
        LOG_RETURN(err, ##__VA_ARGS__); \
        return -err; \
    while(0)

#define XLOG_ERRNO(...) do { \
        LOG_ERRNO(##__VA_ARGS__); \
        return -errno; \
    while(0)

#define XLOG_GAIERR(err, ...) do { \
        LOG_GAIERR(err, ##__VA_ARGS__); \
        return -err; \
    while(0)

typedef struct __client_sock_t {
    int fd;
    struct timeval start;
    struct timeval finish;
    struct timeval rtt;
    bool done;
} client_sock_t;

typedef struct __ping_stats_t {
    const char *host;
    const char *port;
    unsigned long long errors;
    unsigned long long sent;
    unsigned long long recvd;
    double min;
    double max;
    double cum_time;
} ping_stats_t;

static const struct addrinfo HINTS = {
    .ai_family = AF_UNSPEC,
    .ai_socktype = SOCK_STREAM,
    .ai_flags = AI_PASSIVE | AI_V4MAPPED | AI_ADDRCONFIG,
    .ai_protocol = 0,
    .ai_addrlen = 0,
    .ai_addr = NULL,
    .ai_canonname = NULL,
    .ai_next = NULL
};

static const char *const DEFAULT_CONNECT_HOST = "::";
static const char *const DEFAULT_LISTEN_HOST = "::";
static const char *const DEFAULT_PORT = "8009";
static const int BACKLOG = 1024;
static const int YES = 1;

static int print_ping_stats(ping_stats_t *r) {
    int rv = 0;
    char *lbracket = "";
    char *rbracket = "";
    if (strchr(r->host, ':') != NULL) {
        lbracket = "[";
        rbracket = "]";
    }

    if ((rv = printf("--- %s%s%s:%s ping statistics ---\n", lbracket, r->host,
                    rbracket, r->port)) < 0) {
        return rv;
    } else if ((rv = printf("%lld responses, %lld ok, %3.2f%% failed\n", r->recvd,
                    r->recvd - r->errors, (double)r->errors / (double)r->sent * 100.0)) < 0) {
        return rv;
    } else if ((rv = printf("round-trip min/avg/max = %.1f/%.1f/%.1f ms\n", r->min,
                    r->cum_time / (double)r->recvd, r->max)) < 0) {
        return rv;
    }
    return rv;
}


static int client_connect(client_sock_t **r, struct addrinfo *ai) {
    int rv = 0;
    int flags;

    if (r == NULL) { return -EINVAL; };
    if (*r != NULL) { return -EINVAL; };
    if (ai == NULL) { return -EINVAL; };

    if ((*r= malloc(sizeof(client_sock_t))) == NULL) {
            return -errno;
    }
    memset(*r, 0, sizeof(client_sock_t));

    if (((*r)->fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
        goto client_connect_err0;
    } else if ((flags = fcntl((*r)->fd, F_GETFL, 0)) == -1) {
        goto client_connect_err1;
    } else if (fcntl((*r)->fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        goto client_connect_err1;
    } else if (gettimeofday(&((*r)->start), NULL) == -1) {
        goto client_connect_err1;
    } else if ((connect((*r)->fd, ai->ai_addr, ai->ai_addrlen) == -1) &&
            (errno != EINPROGRESS)) {
        goto client_connect_err1;
    }

    if (errno != EINPROGRESS) { /* connect() completed synchronously */
        (*r)->done = true;
        if (gettimeofday(&((*r)->finish), NULL) == -1) {
            goto client_connect_err1;
        }
    }

    return 0;

client_connect_err1:
    rv = rv ? rv : -errno;
    if (close((*r)->fd) == -1) {
        LOG_ERRNO("close() failed");
    }
client_connect_err0:
    rv = rv ? rv : -errno;
    free(*r);
    *r = NULL;
    return rv;
}

static int client_connected(client_sock_t *r) {
    int rv = 0;
    struct sockaddr_storage remote_addr;
    socklen_t remote_addr_len = sizeof(struct sockaddr_storage);
    char ch;

    if (r == NULL) { return -EINVAL; };

    r->done = true;
    if (gettimeofday(&(r->finish), NULL) == -1) {
        goto client_connected_err0;
    /* http://cr.yp.to/docs/connect.html */
    } else if (getpeername(r->fd, (struct sockaddr *)&remote_addr,
                &remote_addr_len) == -1) {
        rv = -errno;
        if (rv == -ENOTCONN) {
            assert(read(r->fd,&ch,1) == -1);
            rv = -errno;
        }
        r->fd = 0;
    }
    r->rtt.tv_sec = r->finish.tv_sec - r->start.tv_sec;
    r->rtt.tv_usec = r->finish.tv_usec - r->start.tv_usec;
    return rv;

client_connected_err0:
    rv = rv ? rv : -errno;
    return rv;
}

static int client_free(client_sock_t *r) {
    if (r == NULL) { return -EINVAL; }
    if (close(r->fd) == -1) {
        return -errno;
    }
    free(r);
    return 0;
}

bool signal_die = false;
static void sig_handler(int UNUSED(status), siginfo_t *UNUSED(info), void *UNUSED(context)) {
    signal_die = true;
}

int main(const int argc, const char *const argv[]) {
    struct addrinfo *listen_res;
    struct addrinfo *connect_res;
    struct addrinfo *res_p;
    int rv;
    unsigned int i;
    int srv_fd;
    int max_fd;
    client_sock_t *client;
    fd_set fd_set;
    double ms;
    unsigned int seq_no = 0;
    ping_stats_t ping_stats;

    memset(&ping_stats, 0, sizeof(ping_stats_t));

    char ni_host[NI_MAXHOST];
    char ni_serv[NI_MAXSERV];

    struct sockaddr_storage remote_addr_storage;
    struct sockaddr *const remote_addr = (struct sockaddr *) &remote_addr_storage;
    socklen_t remote_addr_len;

    const char *const listen_host = argc > 1 ? argv[1] : DEFAULT_LISTEN_HOST;
    const char *const connect_host = argc > 2 ? argv[2] : DEFAULT_CONNECT_HOST;
    const char *const port = argc > 3 ? argv[3] : DEFAULT_PORT;

    const int sigs[] = { SIGTERM, SIGHUP, SIGINT };
    struct sigaction sigact;
    sigset_t empty_sigset;
    sigemptyset(&empty_sigset);
    memset(&sigact, 0, sizeof(struct sigaction));
    sigact.sa_flags = SA_SIGINFO;
    sigact.sa_sigaction = &sig_handler;
    sigact.sa_mask = empty_sigset;
    for (i=0; i < sizeof(sigs)/sizeof(sigs[0]); ++i) {
        if (sigaction(sigs[i], &sigact, NULL) == -1) {
            LOG_ERRNO("sigaction() failed");
            goto main_err0;
        }
    }

    ping_stats.host = connect_host;
    ping_stats.port = port;

    if ((rv = getaddrinfo(listen_host, port, NULL, &listen_res)) != 0) {
        LOG_GAIERR(rv, "getaddrinfo() failed");
        goto main_err0;
    }

    if ((rv = getaddrinfo(connect_host, port, NULL, &connect_res)) != 0) {
        LOG_GAIERR(rv, "getaddrinfo() failed");
        goto main_err1;
    }

    /* server socket() bind() listen() */
    for (res_p = listen_res; res_p != NULL; res_p = res_p->ai_next) {
        if ((rv = getnameinfo(res_p->ai_addr, res_p->ai_addrlen, ni_host,
                        NI_MAXHOST, ni_serv, NI_MAXSERV, NI_NUMERICHOST
                        | NI_NUMERICSERV)) == 0) {
            if (strchr(ni_host, ':') == NULL) {
                /* IPv4 address */
                LOG("Trying %s:%s", ni_host, ni_serv);
            } else {
                /* IPv6 literal */
                LOG("Trying [%s]:%s", ni_host, ni_serv);
            }
        } else {
            if (rv == EAI_SYSTEM) {
                LOG_ERRNO("getnameinfo() failed");
            } else {
                LOG_GAIERR(rv, "getnameinfo() failed");
            }
            goto main_err1;
        }

        if ((srv_fd = socket(res_p->ai_family, res_p->ai_socktype, res_p->ai_protocol)) < 0) {
            LOG_ERRNO("socket() failed");
        } else if (setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &YES, sizeof(YES)) == -1) {
            LOG_ERRNO("setsockopt() failed");
            if (close(srv_fd) == -1) {
                LOG_ERRNO("close() failed");
                goto main_err1;
            }
        } else if (bind(srv_fd, res_p->ai_addr, res_p->ai_addrlen) == -1) {
            LOG_ERRNO("bind() failed");
            if (close(srv_fd) == -1) {
                LOG_ERRNO("close() failed");
                goto main_err1;
            }
        } else if (listen(srv_fd, BACKLOG) == -1) {
            LOG_ERRNO("listen() failed");
            if (close(srv_fd) == -1) {
                LOG_ERRNO("close() failed");
                goto main_err1;
            }
        } else {
            /* Everything worked */
            break;
        }
    }
    res_p = NULL;
    FREEADDRINFO(listen_res);

    if (srv_fd < 0) { goto main_err1; }

    /*
     * client_connect() will return -EINVAL for safety unless client is
     * non-null Also, valgrind will raise issue, since client_connect(),
     * branches on the value of client.
     */
    client = NULL;
    if ((rv = client_connect(&client, connect_res)) != 0) {
        LOG_RETURN(rv, "client_connect() failed");
    } else {
        ping_stats.sent += 1;
    }

    while(1) {
        FD_ZERO(&fd_set);
        FD_SET(srv_fd, &fd_set);
        if (client) {
            FD_SET(client->fd, &fd_set);
        }
        max_fd = client ? MAX(client->fd, srv_fd) : srv_fd;
        if ((rv = select(max_fd+1, &fd_set, NULL, NULL, NULL)) == -1) {
            LOG_ERRNO("select() failed");
            goto main_err2;
        } else if (signal_die) {
            CLIENT_FREE(client);
            break;
        } else if (rv > 0) {
            if (FD_ISSET(srv_fd, &fd_set)) {
                remote_addr_len = sizeof(remote_addr_storage);
                const int fd = accept(srv_fd, remote_addr, &remote_addr_len);
                if (fd == -1) {
                    LOG_ERRNO("accept() failed");
                    goto main_err2;
                }

                if ((rv = getnameinfo(remote_addr, remote_addr_len, ni_host,
                                NI_MAXHOST, ni_serv, NI_MAXSERV, NI_NUMERICHOST
                                | NI_NUMERICSERV)) == 0) {
                    if (strchr(ni_host, ':') == NULL) {
                        /* IPv4 address */
                        fprintf(stderr, "%s:%s Connected...", ni_host, ni_serv);
                    } else {
                        /* IPv6 literal */
                        fprintf(stderr, "[%s]:%s Connected...", ni_host, ni_serv);
                    }
                } else {
                    if (rv == EAI_SYSTEM) {
                        LOG_ERRNO("getnameinfo() failed: system error");
                    } else {
                        LOG_GAIERR(rv, "getnameinfo() failed");
                    }
                    if (close(fd) == -1) {
                        LOG_ERRNO("close() failed");
                    }
                    goto main_err2;
                }

                if (close(fd) == -1) {
                    LOG_ERRNO("close() failed");
                    goto main_err2;
                }
                LOG(" closed");
            }
            if (client && ((FD_ISSET(client->fd, &fd_set)) || client->done)) {
                if ((rv = client_connected(client)) != 0) {
                    LOG_RETURN(rv, "client_connected() failed");
                    ping_stats.errors += 1;
                } else {
                    ping_stats.recvd += 1;
                }
                ms = ((double)client->rtt.tv_sec * 1000.0) + ((double)client->rtt.tv_usec / 1000.0);
                ping_stats.cum_time += ms;
                ping_stats.min = MIN(ping_stats.min, ms);
                ping_stats.max = MIN(ping_stats.max, ms);

                printf("response from %s:%s, seq=%d time=%.2f ms\n", connect_host, port, seq_no, ms);
                CLIENT_FREE(client);
                if (ms > 500) { goto main_err2; }

                if ((rv = client_connect(&client, connect_res)) != 0) {
                    LOG_RETURN(rv, "client_connect() failed");
                } else {
                    ping_stats.sent += 1;
                }
            }
        }
    }

    if (close(srv_fd) == -1) {
        LOG_ERRNO("close() failed");
    }
    FREEADDRINFO(connect_res);
    print_ping_stats(&ping_stats);
    return ping_stats.errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

main_err2:
    if (close(srv_fd) == -1) {
        LOG_ERRNO("close() failed");
    }
    FREEADDRINFO(connect_res);
    print_ping_stats(&ping_stats);
    return EXIT_FAILURE;

main_err1:
    FREEADDRINFO(listen_res);
main_err0:
    return EXIT_FAILURE;
}

/* vim: set ts=4 sw=4 sts et ai: */
