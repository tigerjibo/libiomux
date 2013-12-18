/**
 * \file
 *
 * \brief I/O multiplexer
 *
 * \todo Change 0/1 return values to FALSE/TRUE.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdarg.h>

#if defined(HAVE_EPOLL)
#include <sys/epoll.h>
#elif defined(HAVE_KQUEUE)
#include <sys/event.h>
#endif

#include "bsd_queue.h"

#include "iomux.h"

#define IOMUX_CONNECTIONS_MAX 65535
#define IOMUX_CONNECTION_BUFSIZE 16384
#define IOMUX_CONNECTION_SERVER (1)
#define IOMUX_EOF_TIMEOUT IOMUX_DEFAULT_TIMEOUT

int iomux_hangup = 0;

//! \brief iomux connection strucure
typedef struct __iomux_connection {
    uint32_t flags;
    iomux_callbacks_t cbs;
    unsigned char outbuf[IOMUX_CONNECTION_BUFSIZE];
    int eof;
    int outlen;
    iomux_timeout_id_t timeout_id;
#if defined(HAVE_KQUEUE)
    int16_t kfilters;
    struct kevent event;
#endif
} iomux_connection_t;

//! \brief iomux timeout structure
typedef struct __iomux_timeout {
    iomux_timeout_id_t id;
    struct timeval wait_time;
    TAILQ_ENTRY(__iomux_timeout) timeout_list;
    void (*cb)(iomux_t *iomux, void *priv);
    void *priv;
#if defined(HAVE_KQUEUE)
    int16_t kfilters;
    struct kevent event;
#endif
} iomux_timeout_t;

//! \brief IOMUX base structure
struct __iomux {
    iomux_connection_t *connections[IOMUX_CONNECTIONS_MAX];
    int maxfd;
    int minfd;
    int leave;

    iomux_cb_t loop_end_cb;
    void *loop_end_priv;
    iomux_cb_t hangup_cb;
    void *hangup_priv;

    char error[2048];

    struct timeval last_timeout_check;

#if defined(HAVE_EPOLL)
    struct epoll_event events[IOMUX_CONNECTIONS_MAX];
    int efd; 
#elif defined(HAVE_KQUEUE)
    struct kevent events[IOMUX_CONNECTIONS_MAX];
    int kfd;
#endif
    TAILQ_HEAD(, __iomux_timeout) timeouts;
    int last_timeout_id;
};

static void set_error(iomux_t *iomux, char *fmt, ...) {
    va_list arg;
    va_start(arg, fmt);
    vsnprintf(iomux->error, sizeof(iomux->error), fmt, arg);
    va_end(arg);
}


#define IOMUX_FLUSH_MAXRETRIES 5    //!< Maximum number of iterations for flushing the output buffer

static void iomux_handle_timeout(iomux_t *iomux, void *priv);

/**
 * \brief Create a new iomux handler
 * \returns a valid iomux handler
 */
iomux_t *iomux_create(void)
{
    iomux_t *iomux = (iomux_t *)calloc(1, sizeof(iomux_t));
    if (iomux)
        TAILQ_INIT(&iomux->timeouts);

#if defined(HAVE_EPOLL)
    iomux->efd = epoll_create1(0);
    if (iomux->efd == -1) {
        fprintf(stderr, "Errors creating the epoll descriptor : %s\n", strerror(errno));
        free(iomux);
        return NULL;
    }
#elif defined(HAVE_KQUEUE)
    iomux->kfd = kqueue();
    if (iomux->kfd == -1) {
        fprintf(stderr, "Errors creating the kqueue descriptor : %s\n", strerror(errno));
        free(iomux);
        return NULL;
    }
#endif

    iomux->last_timeout_id = 0;

    return iomux;
}
/**
 * \brief Add a filedescriptor to the mux
 * \param iomux a valid iomux handler
 * \param fd fd to add
 * \param cbs set of callbacks to use with fd
 * \returns TRUE on success; FALSE otherwise.
 */
int
iomux_add(iomux_t *iomux, int fd, iomux_callbacks_t *cbs)
{
    iomux_connection_t *connection = NULL;

    if (fd < 0) {
        set_error(iomux, "fd %d is invalid", fd);
        return 0;
    } else if (fd >= IOMUX_CONNECTIONS_MAX) {
        set_error(iomux, "fd %d exceeds max fd %d", fd, IOMUX_CONNECTIONS_MAX);
        return 0;
    }

    if (iomux->connections[fd]) {
        set_error(iomux, "filedescriptor %d already added", fd);
        return 0;
    }
    if (!cbs) {
        set_error(iomux, "no callbacks have been specified, skipping filedescriptor %d", fd);
        return 0;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);
    connection = (iomux_connection_t *)calloc(1, sizeof(iomux_connection_t));
    if (connection) {

#if defined(HAVE_EPOLL)
        struct epoll_event event = { 0 };
        event.data.fd = fd;
        event.events = EPOLLIN | EPOLLET;
        if (connection->cbs.mux_output)
            event.events |= EPOLLOUT;

        int rc = epoll_ctl(iomux->efd, EPOLL_CTL_ADD, fd, &event);
        if (rc == -1) {
            fprintf(stderr, "Errors adding fd %d to epoll instance %d : %s\n", 
                    fd, iomux->efd, strerror(errno));
            free(connection);
            return 0;;
        }
#elif defined(HAVE_KQUEUE)
        connection->kfilters = EVFILT_READ | EV_ONESHOT;
        if (connection->cbs.mux_output)
            connection->kfilters |= EVFILT_WRITE;

        EV_SET(&connection->event, fd, connection->kfilters, EV_ADD | EV_ENABLE, 0, 0, 0);
        struct timespec poll = { 0, 0 };
        int rc = kevent(iomux->kfd, &connection->event, 1, NULL, 0, &poll);
        if (rc != 0) {
            fprintf(stderr, "Errors adding fd %d to kqueue instance %d : %s\n", 
                    fd, iomux->kfd, strerror(errno));
            free(connection);
            return 0;
        }
#endif
        if (fd > iomux->maxfd)
            iomux->maxfd = fd;
        if (fd < iomux->minfd)
            iomux->minfd = fd;
        
        memcpy(&connection->cbs, cbs, sizeof(connection->cbs));
        iomux->connections[fd] = connection;
        while (!iomux->connections[iomux->minfd] && iomux->minfd != iomux->maxfd)
            iomux->minfd++;

        return 1;
    }
    return 0;
}

/**
 * \brief Remove a filedescriptor from the mux
 * \param iomux a valid iomux handler
 * \param fd fd to remove
 */
void
iomux_remove(iomux_t *iomux, int fd)
{
    iomux_unschedule(iomux, iomux->connections[fd]->timeout_id);

#if defined(HAVE_EPOLL)
    struct epoll_event event = { 0 };
    event.data.fd = fd;

    // NOTE: events might be NULL but on linux kernels < 2.6.9 
    //       it was required to be non-NULL even if ignored
    event.events = EPOLLIN | EPOLLET | EPOLLOUT;

    int rc = epoll_ctl(iomux->efd, EPOLL_CTL_DEL, fd, &event);
    if (rc == -1 && errno != EBADF) {
        fprintf(stderr, "Errors removing fd %d from epoll instance %d : %s\n", 
                fd, iomux->efd, strerror(errno));
    }
#elif defined(HAVE_KQUEUE)
    EV_SET(&iomux->connections[fd]->event, fd, iomux->connections[fd]->kfilters, EV_DELETE, 0, 0, 0);
    struct timespec poll = { 0, 0 };
    int rc = kevent(iomux->kfd, &iomux->connections[fd]->event, 1, NULL, 0, &poll);
    if (rc != 0) {
        fprintf(stderr, "Errors adding removing %d to kqueue instance %d : %s\n", 
                fd, iomux->kfd, strerror(errno));
    }
#endif
    free(iomux->connections[fd]);
    iomux->connections[fd] = NULL;

    if (iomux->maxfd == fd)
        while (iomux->maxfd >= 0 && !iomux->connections[iomux->maxfd])
            iomux->maxfd--;

    if (iomux->minfd == fd)
        while (iomux->minfd != iomux->maxfd && !iomux->connections[iomux->minfd])
            iomux->minfd++;
}

/**
 * \brief Register timed callback.
 * \param iomux iomux handle
 * \param tv timeout
 * \param cb callback handle
 * \param priv context
 * \returns TRUE on success; FALSE otherwise.
 */
iomux_timeout_id_t
iomux_schedule(iomux_t *iomux, struct timeval *tv, iomux_cb_t cb, void *priv)
{
    iomux_timeout_t *timeout, *timeout2;

    if (!tv || !cb)
        return 0;

    if (iomux->last_timeout_check.tv_sec == 0)
        gettimeofday(&iomux->last_timeout_check, NULL);

    timeout = (iomux_timeout_t *)calloc(1, sizeof(iomux_timeout_t));
    memcpy(&timeout->wait_time, tv, sizeof(struct timeval));
    timeout->cb = cb;
    timeout->priv = priv;
    timeout->id = ++iomux->last_timeout_id;

    // keep the list sorted in ascending order
    TAILQ_FOREACH(timeout2, &iomux->timeouts, timeout_list) {
        if ((tv->tv_sec == timeout2->wait_time.tv_sec &&  tv->tv_usec < timeout2->wait_time.tv_usec) ||
                tv->tv_sec < timeout2->wait_time.tv_sec)
        {
            TAILQ_INSERT_BEFORE(timeout2, timeout, timeout_list);
            return 1;
        }
    }
    TAILQ_INSERT_TAIL(&iomux->timeouts, timeout, timeout_list);

#if defined(HAVE_EPOLL)
#elif defined(HAVE_KQUEUE)
        timeout->kfilters = EVFILT_TIMER;
        uint64_t msecs = (timeout->wait_time.tv_sec * 1000) + (timeout->wait_time.tv_usec / 1000);
        EV_SET(&timeout->event, timeout->id, timeout->kfilters, EV_ADD, 0, msecs, timeout);
        struct timespec poll = { 0, 0 };
        int rc = kevent(iomux->kfd, &timeout->event, 1, NULL, 0, &poll);
        if (rc != 0) {
            fprintf(stderr, "Errors adding timeout to kqueue instance %d : %s\n", 
                    iomux->kfd, strerror(errno));
            return 0;
        }
#endif
    return timeout->id;
}

/**
 * \brief Reset the schedule time on a timed callback.
 * \param iomux iomux handle
 * \param tv new timeout
 * \param cb callback handle
 * \param priv context
 * \returns TRUE on success; FALSE otherwise.
 *
 * \note If the timed callback is not found it is added.
 */
iomux_timeout_id_t
iomux_reschedule(iomux_t *iomux, iomux_timeout_id_t id, struct timeval *tv, iomux_cb_t cb, void *priv)
{
    iomux_timeout_t *timeout, *timeout2;

    if (!tv || !cb)
    return 0;

    if (iomux->last_timeout_check.tv_sec == 0)
        gettimeofday(&iomux->last_timeout_check, NULL);

    TAILQ_FOREACH(timeout, &iomux->timeouts, timeout_list) {
        if (timeout->id == id) {
            TAILQ_REMOVE(&iomux->timeouts, timeout, timeout_list);
            break;
        }
    }

    // not found, so create it.
    if (!timeout) {
        timeout = (iomux_timeout_t *)calloc(1, sizeof(iomux_timeout_t));
        timeout->cb = cb;
        timeout->priv = priv;
    }
    memcpy(&timeout->wait_time, tv, sizeof(struct timeval));

    // keep the list sorted in ascending order
    TAILQ_FOREACH(timeout2, &iomux->timeouts, timeout_list) {
        if ((tv->tv_sec == timeout2->wait_time.tv_sec
             &&  tv->tv_usec < timeout2->wait_time.tv_usec)
            || tv->tv_sec < timeout2->wait_time.tv_sec)
        {
            TAILQ_INSERT_BEFORE(timeout2, timeout, timeout_list);
            return 1;
        }
    }
    TAILQ_INSERT_TAIL(&iomux->timeouts, timeout, timeout_list);
    return 1;
}

/**
 * \brief Unregister timed callback.
 * \param iomux iomux handle
 * \param cb callback handle
 * \param priv context
 * \note Removes _all_ instances that match.
 * \returns number of removed callbacks.
 */
int
iomux_unschedule_all(iomux_t *iomux, iomux_cb_t cb, void *priv)
{           
    iomux_timeout_t *timeout, *timeout_tmp;
    int count = 0;

    TAILQ_FOREACH_SAFE(timeout, &iomux->timeouts, timeout_list, timeout_tmp) {
        if (cb == timeout->cb && priv == timeout->priv) {
#if defined(HAVE_EPOLL)
#elif defined(HAVE_KQUEUE)
            EV_SET(&timeout->event, timeout->id, timeout->kfilters, EV_DELETE, 0, 0, 0);
            struct timespec poll = { 0, 0 };
            int rc = kevent(iomux->kfd, &timeout->event, 1, NULL, 0, &poll);
            if (rc != 0) {
                fprintf(stderr, "Errors adding timeout to kqueue instance %d : %s\n", 
                        iomux->kfd, strerror(errno));
                return 0;
            }
#endif
            TAILQ_REMOVE(&iomux->timeouts, timeout, timeout_list);
            free(timeout);
            count++;
        }
    }
    return count;
}

int
iomux_unschedule(iomux_t *iomux, iomux_timeout_id_t id)
{
    iomux_timeout_t *timeout, *timeout_tmp;

    if (!id)
        return 0;

    TAILQ_FOREACH_SAFE(timeout, &iomux->timeouts, timeout_list, timeout_tmp) {
        if (id == timeout->id) {
#if defined(HAVE_EPOLL)
#elif defined(HAVE_KQUEUE)
            EV_SET(&timeout->event, timeout->id, timeout->kfilters, EV_DELETE, 0, 0, 0);
            struct timespec poll = { 0, 0 };
            int rc = kevent(iomux->kfd, &timeout->event, 1, NULL, 0, &poll);
            if (rc != 0) {
                fprintf(stderr, "Errors adding timeout to kqueue instance %d : %s\n", 
                        iomux->kfd, strerror(errno));
                return 0;
            }
#endif
            TAILQ_REMOVE(&iomux->timeouts, timeout, timeout_list);
            free(timeout);
            break;
        }
    }

    return 1;
}

static void
iomux_handle_timeout(iomux_t *iomux, void *priv)
{
    int fd = (long int)priv;

    if (iomux->connections[fd]) {
        iomux_callbacks_t *cbs = &iomux->connections[fd]->cbs;
        if (cbs->mux_timeout)
            cbs->mux_timeout(iomux, fd, cbs->priv);
    }
}

/**
 * \brief Register a timeout on a connection.
 * \param iomux iomux handle
 * \param fd fd
 * \param tv timeout or NULL
 * \returns TRUE on success; FALSE otherwise.
 * \note If tv is NULL the timeout is disabled.
 * \note Needs to be reset after a timeout has fired.
 */
iomux_timeout_id_t
iomux_set_timeout(iomux_t *iomux, int fd, struct timeval *tv)
{
    if (!iomux->connections[fd])
        return 0;

    if (!tv) {
        (void) iomux_unschedule(iomux, iomux->connections[fd]->timeout_id);
        return 0;
    } else {
        return iomux_reschedule(iomux, iomux->connections[fd]->timeout_id, tv, iomux_handle_timeout, (void *)(long int)fd);
    }
}

/**
 * \brief put and fd to listening state (aka: server connection)
 * \param iomux a valid iomux handler
 * \param fd the fd to put in listening state
 * \returns TRUE on success; FALSE otherwise.
 */
int
iomux_listen(iomux_t *iomux, int fd)
{
    if (!iomux->connections[fd]) {
        set_error(iomux, "%s: No connections for fd %d", __FUNCTION__, fd);
        return 0;
    }
    assert(iomux->connections[fd]->cbs.mux_connection);

    if (listen(fd, -1) != 0) {
        set_error(iomux, "%s: Error listening on fd %d: %s", __FUNCTION__, fd, strerror(errno));
        return 0;
    }

    iomux->connections[fd]->flags |= IOMUX_CONNECTION_SERVER;

    return 1;
}

void
iomux_loop_end_cb(iomux_t *iomux, iomux_cb_t cb, void *priv)
{
    iomux->loop_end_cb = cb;
    iomux->loop_end_priv = priv;
}

void
iomux_hangup_cb(iomux_t *iomux, iomux_cb_t cb, void *priv)
{
    iomux->hangup_cb = cb;
    iomux->hangup_priv = priv;
}

static void
iomux_update_timeouts(iomux_t *iomux)
{
    iomux_timeout_t *timeout = NULL;
    struct timeval diff = { 0, 0 };
    struct timeval now;

    gettimeofday(&now, NULL);
    if (iomux->last_timeout_check.tv_sec)
        timersub(&now, &iomux->last_timeout_check, &diff);

    memcpy(&iomux->last_timeout_check, &now, sizeof(struct timeval));

    memset(&diff, 0, sizeof(diff));

#if defined (HAVE_KQUEUE)
    // remove expired timeouts (but they should have been removed when fired ... weird)
    while ((timeout = TAILQ_FIRST(&iomux->timeouts)) && timercmp(&timeout->wait_time, &diff, <=)) {
        TAILQ_REMOVE(&iomux->timeouts, timeout, timeout_list);
        fprintf(stderr, "Expired (not-fired) timeout found : %d\n", timeout->id);
        free(timeout);
    }
#endif

    // update timeouts' waiting time
    TAILQ_FOREACH(timeout, &iomux->timeouts, timeout_list)
        timersub(&timeout->wait_time, &diff, &timeout->wait_time);
}

static void
iomux_accept_connections_fd(iomux_t *iomux, int fd)
{
    iomux_callbacks_t *cbs =  &iomux->connections[fd]->cbs;
    int newfd;
    struct sockaddr_in peer;
    socklen_t socklen = sizeof(struct sockaddr);
    // if it is, accept all pending connections and add them to the mux
    while ((newfd = accept(fd, (struct sockaddr *)&peer, &socklen)) >= 0) {
        cbs->mux_connection(iomux, newfd, cbs->priv);
    }
}

static void 
iomux_read_fd(iomux_t *iomux, int fd)
{
    iomux_callbacks_t *cbs =  &iomux->connections[fd]->cbs;
    char inbuf[IOMUX_CONNECTION_BUFSIZE];
    int rb = read(fd, inbuf, sizeof(inbuf));
    if (rb == -1) {
        if (errno != EINTR && errno != EAGAIN) {
             fprintf(stderr, "read on fd %d failed: %s\n", fd, strerror(errno));
             iomux_close(iomux, fd);
         }
    } else if (rb == 0) {
         iomux_close(iomux, fd);
    } else {
         if (cbs->mux_input)
             cbs->mux_input(iomux, fd, inbuf, rb, cbs->priv);
    }
}

static void
iomux_write_fd(iomux_t *iomux, int fd)
{
    iomux_callbacks_t *cbs =  &iomux->connections[fd]->cbs;
    if (!iomux->connections[fd]->outlen && cbs->mux_output) {
        cbs->mux_output(iomux, fd, cbs->priv);
    }

    // note that the fd might have been closed by the mux_output callback
    // so we need to check for its presence again
    if (!iomux->connections[fd] || !iomux->connections[fd]->outlen)
        return;

    int wb = write(fd, iomux->connections[fd]->outbuf, iomux->connections[fd]->outlen);
    if (wb == -1) {
        if (errno != EINTR || errno != EAGAIN) {
            fprintf(stderr, "write on fd %d failed: %s\n", fd, strerror(errno));
            iomux_close(iomux, fd);
        }
    } else if (wb == 0) {
        iomux_close(iomux, fd);
    } else {
        iomux->connections[fd]->outlen -= wb;
        if (iomux->connections[fd]->outlen) { // shift data if we didn't write it all at once
            memmove(iomux->connections[fd]->outbuf, &iomux->connections[fd]->outbuf[wb], iomux->connections[fd]->outlen);
        } else if (!cbs->mux_output) {
#if defined(HAVE_EPOLL)
            // let's unregister this fd from EPOLLOUT events (seems nothing needs to be sent anymore)
            struct epoll_event event = { 0 };
            event.data.fd = fd;
            event.events = EPOLLIN | EPOLLET;

            int rc = epoll_ctl(iomux->efd, EPOLL_CTL_MOD, fd, &event);
            if (rc == -1) {
                fprintf(stderr, "Errors modifying fd %d on epoll instance %d : %s\n", 
                        fd, iomux->efd, strerror(errno));
            }
#elif defined(HAVE_KQUEUE)
            struct timespec poll = { 0, 0 };
            iomux->connections[fd]->kfilters &= ~EVFILT_WRITE;
            EV_SET(&iomux->connections[fd]->event, fd, iomux->connections[fd]->kfilters, EV_DELETE, 0, 0, 0);
            int rc = kevent(iomux->kfd, &iomux->connections[fd]->event, 1, NULL, 0, &poll);
            if (rc == -1) {
                fprintf(stderr, "Errors modifying fd %d on kqueue instance %d : %s\n", 
                        fd, iomux->kfd, strerror(errno));
            }
#endif
        }
    }
}

static struct timeval *
iomux_adjust_timeout(iomux_t *iomux, struct timeval *tv_default)
{
    struct timeval *tv = NULL;
    iomux_timeout_t *timeout = NULL;

    timeout = TAILQ_FIRST(&iomux->timeouts);
    if (timeout && timeout) {
        if (timercmp(&timeout->wait_time, tv_default, >))
            tv = tv_default;
        else
            tv = &timeout->wait_time;
    } else if (timeout) {
        tv = &timeout->wait_time;
    } else if (timeout) {
        tv = tv_default;
    } else {
        tv = NULL;
    }
    return tv;
}

#if defined(HAVE_KQUEUE)
void
iomux_run(iomux_t *iomux, struct timeval *tv_default)
{
    int i;

    struct timeval *tv = iomux_adjust_timeout(iomux, tv_default);
    struct timespec ts = { tv->tv_sec, tv->tv_usec * 1000 };
    int n = kevent(iomux->kfd, NULL, 0, iomux->events, IOMUX_CONNECTIONS_MAX, &ts);

    if (n == -1) {
        // TODO : error messges
    } else if (n > 0) {
        for (i = 0; i < n; i++) {
            struct kevent *event = &iomux->events[i];
            int fd = event->ident;
            iomux_connection_t *conn = iomux->connections[fd];
            if (!conn) {
                // TODO - Error Messages
                continue;
            }

            if (event->filter & EVFILT_READ) {
                if ((iomux->connections[fd]->flags&IOMUX_CONNECTION_SERVER) == (IOMUX_CONNECTION_SERVER) && event->data) {
                    while(event->data--)
                        iomux_accept_connections_fd(iomux, fd);
                } else {
                    iomux_read_fd(iomux, fd);
                }
            }

            if (event->filter & EVFILT_WRITE) {
                iomux_write_fd(iomux, fd);
            }

            if (event->filter & EVFILT_TIMER) {
                iomux_timeout_t *timeout = (iomux_timeout_t *)event->udata;
                TAILQ_REMOVE(&iomux->timeouts, timeout, timeout_list);
                timeout->cb(iomux, timeout->priv);
                free(timeout);
            } 
            
            if (event->flags & EV_EOF) {
                iomux_close(iomux, fd);
            }
        }
    }
    iomux_update_timeouts(iomux);
}

#elif defined(HAVE_EPOLL)

void
iomux_run(iomux_t *iomux, struct timeval *tv_default)
{
    int fd;

    struct timeval *tv = iomux_adjust_timeout(iomux, tv_default);

    int epoll_waiting_time = (tv->tv_sec * 1000) + (tv->tv_usec / 1000);
    int num_fds = iomux->maxfd - iomux->minfd;
    int n = epoll_wait(iomux->efd, iomux->events, num_fds, epoll_waiting_time);
    int i;
    for (i = 0; i < n; i++) {
        if ((iomux->events[i].events & EPOLLERR) ||
          (iomux->events[i].events & EPOLLHUP) ||
          (!(iomux->events[i].events & EPOLLIN || iomux->events[i].events & EPOLLOUT)))
        {
            fprintf (stderr, "epoll error : %s\n", strerror(errno));
            iomux_close(iomux, iomux->events[i].data.fd);
            continue;
        }
        fd  = iomux->events[i].data.fd;
        if ((iomux->connections[fd]->flags&IOMUX_CONNECTION_SERVER) == (IOMUX_CONNECTION_SERVER))
        {
            iomux_accept_connections_fd(iomux, fd);
        } else {
            if (iomux->events[i].events & EPOLLIN) {
                iomux_read_fd(iomux, fd);
            }

            if (!iomux->connections[fd]) // connection has been closed/removed
                continue;

            if (iomux->events[i].events& EPOLLOUT) {
                iomux_write_fd(iomux, fd);
            }
        }
    }
}

#else

void
iomux_run_timers(iomux_t *iomux)
{
    iomux_timeout_t *timeout = NULL;

    iomux_update_timeouts(iomux);

    // run expired timeouts
    struct timeval diff = { 0, 0 };
    while ((timeout = TAILQ_FIRST(&iomux->timeouts)) && timercmp(&timeout->wait_time, &diff, <=)) {
        TAILQ_REMOVE(&iomux->timeouts, timeout, timeout_list);
        timeout->cb(iomux, timeout->priv);
        free(timeout);
    }
}

/**
 * \brief trigger a runcycle on an iomux
 * \param iomux iomux
 * \param timeout return control to the caller if nothing
 *        happens in the mux within the specified timeout
 */
void
iomux_run(iomux_t *iomux, struct timeval *tv_default)
{
    int fd;
    fd_set rin, rout;
    int maxfd = iomux->minfd;;

    FD_ZERO(&rin);
    FD_ZERO(&rout);

    for (fd = iomux->minfd; fd <= iomux->maxfd; fd++) {
        if (iomux->connections[fd])  {
            iomux_connection_t *conn = iomux->connections[fd];
            // always register managed fds for reading (even if 
            // no mux_input callbacks is present) to detect EOF.
            FD_SET(fd, &rin);
            if (fd > maxfd)
                maxfd = fd;
            if (conn->outlen || conn->cbs.mux_output) {
                // output pending data
                FD_SET(fd, &rout);
                if (fd > maxfd)
                    maxfd = fd;
            }
        }
    }

    struct timeval *tv = iomux_adjust_timeout(iomux, tv_default);

    switch (select(maxfd+1, &rin, &rout, NULL, tv)) {
    case -1:
        if (errno == EINTR)
            return;
        if (errno == EAGAIN)
            return;
        set_error(iomux, "select(): %s", strerror(errno));
        break;
    case 0:
        break;
    default:
        for (fd = iomux->minfd; fd <= iomux->maxfd; fd++) {
            if (iomux->connections[fd]) {
                if (FD_ISSET(fd, &rin)) {
                    // check if this is a listening socket
                    if ((iomux->connections[fd]->flags&IOMUX_CONNECTION_SERVER) == (IOMUX_CONNECTION_SERVER)) {
                        iomux_accept_connections_fd(iomux, fd);
                    } else {
                        iomux_read_fd(iomux, fd);
                    }
                }
                if (!iomux->connections[fd]) // connection has been closed/removed
                    continue;

                if (FD_ISSET(fd, &rout)) {
                    iomux_write_fd(iomux, fd);
                }
            }
        }
    }

    iomux_run_timers(iomux);
}

#endif

/**
 * \brief Take over the runloop and handle timeouthandlers while running the mux.
 * \param iomux a valid iomux handler
 */
void
iomux_loop(iomux_t *iomux, int timeout)
{
    while (!iomux->leave) {
        struct timeval tv_default = { timeout, 0 };

        iomux_run(iomux, &tv_default);

        if (iomux->loop_end_cb)
            iomux->loop_end_cb(iomux, iomux->loop_end_priv);

        if (iomux_hangup && iomux->hangup_cb)
            iomux->hangup_cb(iomux, iomux->hangup_priv);
    }
    iomux->leave = 0;
}

/**
 * \brief stop a running mux and return control back to the iomux_loop() caller
 * \param iomux a valid iomux handler
 */
void
iomux_end_loop(iomux_t *iomux)
{
    iomux->leave = 1;
}

/**
 * \brief write to an fd handled by the iomux
 * \param iomux a valid iomux handler
 * \param fd the fd we want to write to
 * \param buf the buffer to write
 * \param len length of the buffer
 * \returns the number of written bytes
 */
int
iomux_write(iomux_t *iomux, int fd, const void *buf, int len)
{
    int free_space = IOMUX_CONNECTION_BUFSIZE-iomux->connections[fd]->outlen;
    int wlen = (len > free_space)?free_space:len;

    if (wlen) {
#ifdef HAVE_EPOLL
        struct epoll_event event = { 0 };
        event.data.fd = fd;
        event.events = EPOLLIN | EPOLLET | EPOLLOUT;

        int rc = epoll_ctl(iomux->efd, EPOLL_CTL_MOD, fd, &event);
        if (rc == -1) {
            fprintf(stderr, "Errors adding fd %d to epoll instance %d : %s\n", 
                    fd, iomux->efd, strerror(errno));
            return 0;
        }
#endif
        memcpy(iomux->connections[fd]->outbuf+iomux->connections[fd]->outlen,
                buf, wlen);
        iomux->connections[fd]->outlen += wlen;
    }

    return wlen;
}

/**
 * \brief close a file handled by the iomux
 * \param iomux a valid iomux handler
 * \param fd the fd to close
 */
void
iomux_close(iomux_t *iomux, int fd)
{
    iomux_connection_t *conn = iomux->connections[fd];
    if (!conn) // fd is not registered within iomux
        return;

    if (conn->outlen) { // there is pending data
        int retries = 0;
        while (conn->outlen && retries <= IOMUX_FLUSH_MAXRETRIES) {
            int wb = write(fd, conn->outbuf, conn->outlen);
            if (wb == -1) {
                if (errno == EINTR || errno == EAGAIN)
                    retries++;
                else
                    break;
            } else if (wb == 0) {
                fprintf(stderr, "%s: closing filedescriptor %d with %db pending data\n", __FUNCTION__, fd, conn->outlen);
                break;
            } else {
                conn->outlen -= wb;
            }
        }
    }

    if(conn->cbs.mux_eof)
        conn->cbs.mux_eof(iomux, fd, conn->cbs.priv);

    iomux_remove(iomux, fd);
}

/**
 * \brief relase all resources used by an iomux
 * \param iomux a valid iomux handler
 */
void
iomux_destroy(iomux_t *iomux)
{
    int fd;

    for (fd = iomux->maxfd; fd >= iomux->minfd; fd--)
        if (iomux->connections[fd])
            iomux_close(iomux, fd);

    free(iomux);
}

int
iomux_isempty(iomux_t *iomux)
{
    int fd;
    int ret = 1;
    for (fd = iomux->minfd; fd <= iomux->maxfd; fd++) {
        if (iomux->connections[fd]) {
            ret = 0;
            break;
        }
    }
    return ret;
}