#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef UNUSED
#elif defined(__GNUC__)
#    define UNUSED(x) UNUSED_ ## x __attribute__((unused))
#elif defined(__LCLINT__)
#    define UNUSED(x) /*@unused@*/ x
#else
#    define UNUSED(x) x
#endif

#define MAX(x, y) ((x)> (y) ? (x) : (y))

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
    if (r == NULL) { return -EINVAL; };

    r->done = true;
    if (gettimeofday(&(r->finish), NULL) == -1) {
        goto client_close_err0;
    }
    r->rtt.tv_sec = r->finish.tv_sec - r->start.tv_sec;
    r->rtt.tv_usec = r->finish.tv_usec - r->start.tv_usec;

    return 0;

client_close_err0:
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


int main (const int argc, const char *const argv[]) {
    struct addrinfo *listen_res;
    struct addrinfo *connect_res;
    struct addrinfo *res_p;
    int rv;
    int srv_fd;
    client_sock_t *client;
    fd_set fd_set;
    double ms;
    double min = 0, max = 0, avg = 0;
    unsigned int seq_no = 0;

    char ni_host[NI_MAXHOST];
    char ni_serv[NI_MAXSERV];

    struct sockaddr_storage remote_addr_storage;
    struct sockaddr *const remote_addr = (struct sockaddr *) &remote_addr_storage;
    socklen_t remote_addr_len;

    const char *const listen_host = argc > 1 ? argv[1] : DEFAULT_LISTEN_HOST;
    const char *const connect_host = argc > 2 ? argv[2] : DEFAULT_CONNECT_HOST;
    const char *const port = argc > 3 ? argv[3] : DEFAULT_PORT;

    if ((rv = getaddrinfo(listen_host, port, NULL, &listen_res)) != 0) {
        LOG_GAIERR(rv, "getaddrinfo() failed");
        goto main_err0;
    }

    if ((rv = getaddrinfo(connect_host, port, NULL, &connect_res)) != 0) {
        LOG_GAIERR(rv, "getaddrinfo() failed");
        goto main_err0;
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
    }
    while(1) {
        FD_ZERO(&fd_set);
        FD_SET(srv_fd, &fd_set);
        FD_SET(client->fd, &fd_set);
        if ((rv = select(MAX(srv_fd, client->fd)+1, &fd_set, NULL, NULL, NULL)) == -1) {
            LOG_ERRNO("select() failed");
            goto main_err2;
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
            if ((FD_ISSET(client->fd, &fd_set)) || client->done) {
                if ((rv = client_connected(client)) != 0) {
                    LOG_RETURN(rv, "client_connected() failed");
                }
                seq_no += 1;
                ms = ((double)client->rtt.tv_sec * 1000.0) + ((double)client->rtt.tv_usec / 1000.0);
                avg += ms;
                min = min > ms ? ms : min;
                max = max < ms ? ms : max;


                printf("response from %s:%s, seq=%d time=%.2f ms\n", connect_host, port, seq_no, ms);
                CLIENT_FREE(client);
                if (ms > 500) { goto main_err2; }

                if ((rv = client_connect(&client, connect_res)) != 0) {
                    LOG_RETURN(rv, "client_connect() failed");
                }
            }
        }
    }

    if (close(srv_fd) == -1) {
        LOG_ERRNO("close() failed");
    }
    FREEADDRINFO(connect_res);
    return EXIT_SUCCESS;

main_err2:
    if (close(srv_fd) == -1) {
        LOG_ERRNO("close() failed");
    }
    FREEADDRINFO(connect_res);
    return EXIT_FAILURE;

main_err1:
    FREEADDRINFO(listen_res);
main_err0:
    return EXIT_FAILURE;
}

/* vim: set ts=4 sw=4 sts et ai: */
