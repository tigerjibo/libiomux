#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>


#include <sys/stat.h>
#include <sys/types.h>

#include "iomux.h"

#include "testing.h"

#define TEST_STRING "CIAO"
#define TEST_SERVER_PORT   6543
#define TEST_CLIENT_PORT   6544

void test_input(iomux_t *mux, int fd, void *data, int len, void *priv);
void test_timeout(iomux_t *mux, int fd, void *priv);
//void test_eof(iomux_t *mux, int fd, void *priv);
void test_connection(iomux_t *mux, int fd, void *priv);

struct {
    int number;
    char string[256];
} test_context;

int client, server;

iomux_callbacks_t callbacks = 
{ 
    test_input, NULL, test_timeout, NULL, test_connection, (void *)&test_context
};

static int
string2sockaddr(const char *host, int port, struct sockaddr_in *sockaddr)
{
    u_int32_t ip = htonl(INADDR_LOOPBACK);
    errno = EINVAL;

    if (host) {
        char host2[512];
        char *p;
        char *pe;

        strncpy(host2, host, sizeof(host2)-1);
        p = strchr(host2, ':');

        if (p) {                // check for <host>:<port>
            *p = '\0';                // point to port part
            p++;
            port = strtol(p, &pe, 10);        // convert string to number
            if (*pe != '\0') {            // did not match complete string? try as string
#if (defined(__APPLE__) && defined(__MACH__))
		        struct servent *e = getservbyname(p, "tcp");
#else
                struct servent *e = NULL, ebuf;
                char buf[1024];
                getservbyname_r(p, "tcp", &ebuf, buf, sizeof(buf), &e);
#endif
                if (!e) {
                    errno = ENOENT;        // to avoid errno == 0 in error case
                    return -1;
                }
                port = ntohs(e->s_port);
            }
        }

        if (strcmp(host2, "*") == 0) {
            ip = INADDR_ANY;
        } else {
            if (!inet_aton(host2, (struct in_addr *)&ip)) {

                struct hostent *e = NULL;
#if (defined(__APPLE__) && defined(__MACH__))
                e = gethostbyname(host2);
#else
                struct hostent ebuf;
                char buf[1024];
                int herrno;
                gethostbyname_r(host2, &ebuf, buf, sizeof(buf), &e, &herrno);
#endif
                if (!e || e->h_addrtype != AF_INET) {
                    errno = ENOENT;        // to avoid errno == 0 in error case
                    return -1;
                }
                ip = ((unsigned long *) (e->h_addr_list[0]))[0];
            }
        }
    }
    if (port == 0)
        return -1;
    else
        port = htons(port);

    bzero(sockaddr, sizeof(struct sockaddr_in));
#ifndef __linux
    sockaddr->sin_len = sizeof(struct sockaddr_in);
#endif
    sockaddr->sin_family = AF_INET;
    sockaddr->sin_addr.s_addr = ip;
    sockaddr->sin_port = port;

    return 0;
}

static int
open_socket(const char *host, int port)
{
    int val = 1;
    struct sockaddr_in sockaddr;
    int sock;
    struct linger ling = {0, 0};

    errno = EINVAL;
    if (host == NULL || strlen(host) == 0 || port == 0)
    return -1;

    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == -1)
    return -1;

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &val,  sizeof(val));
    setsockopt(sock, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));

    if (string2sockaddr(host, port, &sockaddr) == -1
    || bind(sock, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) == -1) {
    shutdown(sock, SHUT_RDWR);
    close(sock);
    return -1;
    }

    listen(sock, -1);
    fcntl(sock, F_SETFD, FD_CLOEXEC);

    return sock;
}

static int
open_connection(const char *host, int port, unsigned int timeout)
{
    int val = 1;
    struct sockaddr_in sockaddr;
    int sock;

    errno = EINVAL;
    if (host == NULL || strlen(host) == 0 || port == 0)
    return -1;

    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == -1)
    return -1;

    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &val,  sizeof(val));
    if (timeout > 0) {
    struct timeval tv = { timeout, 0 };
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1
        || setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1)
        fprintf(stderr, "%s:%d: Failed to set timeout to %d\n", host, port, timeout);
    }

    if (string2sockaddr(host, port, &sockaddr) == -1 ||
        connect(sock, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) == -1)
    {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        return -1;
    }

    fcntl(sock, F_SETFD, FD_CLOEXEC);

    return sock;
}




void test_input(iomux_t *mux, int fd, void *data, int len, void *priv)
{
    struct timeval tv = { 1, 0 };

    if (len != strlen(TEST_STRING)) {
        t_failure("len %d should %d", len, strlen(TEST_STRING));
    } else {
        t_validate_buffer(data, len, TEST_STRING, len);
        t_testing("iomux_set_timeout(mux, server=%d, tv={ 1, 0 })", server);
        t_validate_int(iomux_set_timeout(mux, server, &tv), 1);
    }
}

void test_timeout(iomux_t *mux, int fd, void *priv)
{
    //struct timeval tv = { 1, 0 };
    t_testing("iomux_end_loop(mux)");
    iomux_end_loop(mux);
}

/*
void test_eof(iomux_t *mux, int fd, void *priv)
{
    printf("Closing fildescriptor %d \n", fd);
}
*/

void test_connection(iomux_t *mux, int fd, void *priv)
{
    iomux_add(mux, fd, &callbacks);
}

int
main(int argc, char **argv)
{
    iomux_t *mux;

    t_init();
     
    t_testing("iomux_create()");
    mux = iomux_create();
    if (mux)
        t_success();
    else
        t_failure("returned NULL");

    t_testing("opening server socket");
    server = open_socket("localhost", TEST_SERVER_PORT);
    if (!server) 
        t_failure("Error : %s\n", strerror(errno));
    else
        t_success();
    t_testing("iomux_add(mux, server=%d)", server);
    t_validate_int(iomux_add(mux, server, &callbacks), 1);
    if (!iomux_listen(mux, server))
        exit(-1);
    
    t_testing("opening client connection");
    client = open_connection("localhost", TEST_SERVER_PORT, 5);
    if (!client) 
        t_failure("Error : %s\n", strerror(errno));
    else
        t_success();
    t_testing("iomux_add(mux, client=%d)", client);
    t_validate_int(iomux_add(mux, client, &callbacks), 1);

    t_testing("iomux_write(mux, client, %s, %d)", TEST_STRING, strlen(TEST_STRING));
    t_validate_int(iomux_write(mux, client, TEST_STRING, strlen(TEST_STRING)), strlen(TEST_STRING));

    t_testing("iomux_input_callback() callback");
    iomux_loop(mux, 0);
    t_success();

    iomux_destroy(mux);

    t_summary();

    exit(0);
}