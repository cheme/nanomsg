/*
    Copyright (c) 2012 250bpm s.r.o.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "aio.h"
#include "err.h"
#include "cont.h"
#include "fast.h"
#include "alloc.h"

#define _GNU_SOURCE
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

/*  Private functions. */
static void sp_cp_worker (void *arg);
static void sp_usock_tune (struct sp_usock *self);
static int sp_usock_send_raw (struct sp_usock *self, const void *buf,
    size_t *len);
static int sp_usock_recv_raw (struct sp_usock *self, void *buf, size_t *len);
static int sp_usock_geterr (struct sp_usock *self);

void sp_timer_init (struct sp_timer *self, const struct sp_cp_sink **sink,
    struct sp_cp *cp)
{
    self->sink = sink;
    self->cp = cp;
    self->active = 0;
}

void sp_timer_term (struct sp_timer *self)
{
    sp_timer_stop (self);
}

void sp_timer_start (struct sp_timer *self, int timeout)
{
    int rc;

    /*  If the timer is active, cancel it first. */
    if (self->active)
        sp_timer_stop (self);

    self->active = 1;
    rc = sp_timeout_add (&self->cp->timeout, timeout, &self->hndl);
    errnum_assert (rc >= 0, -rc);

    if (rc == 1 && !sp_thread_current (&self->cp->worker))
        sp_efd_signal (&self->cp->efd);
}

void sp_timer_stop (struct sp_timer *self)
{
    int rc;

    /*  If the timer is not active, do nothing. */
    if (!self->active)
         return;

    rc = sp_timeout_rm (&self->cp->timeout, &self->hndl);
    errnum_assert (rc >= 0, -rc);
    if (rc == 1 && !sp_thread_current (&self->cp->worker))
        sp_efd_signal (&self->cp->efd);
}

void sp_event_init (struct sp_event *self, const struct sp_cp_sink **sink,
    struct sp_cp *cp)
{
    self->sink = sink;
    self->cp = cp;
    self->active = 0;
}

void sp_event_term (struct sp_event *self)
{
    sp_assert (!self->active);
}

void sp_event_signal (struct sp_event *self)
{
    /*  Enqueue the event for later processing. */
    self->active = 1;
    sp_mutex_lock (&self->cp->events_sync);
    sp_queue_push (&self->cp->events, &self->item);
    sp_mutex_unlock (&self->cp->events_sync);
    sp_efd_signal (&self->cp->efd);
}

int sp_usock_init (struct sp_usock *self, const struct sp_cp_sink **sink,
    int domain, int type, int protocol, struct sp_cp *cp)
{
#if !defined SOCK_CLOEXEC && defined FD_CLOEXEC
    int rc;
#endif

    self->sink = sink;
    self->cp = cp;
    self->in.batch = NULL;
    self->in.batch_len = 0;
    self->in.batch_pos = 0;
    self->in.op = SP_USOCK_INOP_NONE;
    self->out.op = SP_USOCK_OUTOP_NONE;
    self->add_hndl.op = SP_USOCK_OP_ADD;
    self->rm_hndl.op = SP_USOCK_OP_RM;
    self->in.hndl.op = SP_USOCK_OP_IN;
    self->out.hndl.op = SP_USOCK_OP_OUT;
    self->domain = domain;
    self->type = type;
    self->protocol = protocol;

    /*  If the operating system allows to directly open the socket with CLOEXEC
        flag, do so. That way there are no race conditions. */
#ifdef SOCK_CLOEXEC
    type |= SOCK_CLOEXEC;
#endif

    /*  Open the underlying socket. */
    self->s = socket (domain, type, protocol);
    if (self->s < 0)
       return -errno;

    /*  Setting FD_CLOEXEC option immediately after socket creation is the
        second best option. There is a race condition (if process is forked
        between socket creation and setting the option) but the problem is
        pretty unlikely to happen. */
#if !defined SOCK_CLOEXEC && defined FD_CLOEXEC
    rc = fcntl (self->s, F_SETFD, FD_CLOEXEC);
    errno_assert (rc != -1);
#endif

    sp_usock_tune (self);

    return 0;
}

const struct sp_cp_sink **sp_usock_setsink (struct sp_usock *self,
    const struct sp_cp_sink **sink)
{
    const struct sp_cp_sink **original;

    original = self->sink;
    self->sink = sink;
    return original;
}

int sp_usock_init_child (struct sp_usock *self, struct sp_usock *parent,
    int s, const struct sp_cp_sink **sink, struct sp_cp *cp)
{
    self->sink = sink;
    self->s = s;
    self->cp = cp;
    self->in.batch = NULL;
    self->in.batch_len = 0;
    self->in.batch_pos = 0;
    self->in.op = SP_USOCK_INOP_NONE;
    self->out.op = SP_USOCK_OUTOP_NONE;
    self->add_hndl.op = SP_USOCK_OP_ADD;
    self->rm_hndl.op = SP_USOCK_OP_RM;
    self->in.hndl.op = SP_USOCK_OP_IN;
    self->out.hndl.op = SP_USOCK_OP_OUT;
    self->domain = parent->domain;
    self->type = parent->type;
    self->protocol = parent->protocol;

    sp_usock_tune (self);

    /*  Register the new socket with the suplied completion port. 
        If the function is called from the worker thread, modify the pollset
        straight away. Otherwise send an event to the worker thread. */
    if (sp_thread_current (&self->cp->worker))
        sp_poller_add (&self->cp->poller, self->s, &self->hndl);
    else {
        sp_queue_push (&self->cp->opqueue, &self->add_hndl.item);
        sp_efd_signal (&self->cp->efd);
    }

    return 0;
}

static void sp_usock_tune (struct sp_usock *self)
{
    int rc;
    int opt;
    int flags;
    int only;

    /*  If applicable, prevent SIGPIPE signal when writing to the connection
        already closed by the peer. */
#ifdef SO_NOSIGPIPE
    opt = 1;
    rc = setsockopt (self->s, SOL_SOCKET, SO_NOSIGPIPE,
        &opt, sizeof (opt));
    errno_assert (rc == 0);
#endif

    /*  Switch the socket to the non-blocking mode. All underlying sockets
        are always used in the asynchronous mode. */
	flags = fcntl (self->s, F_GETFL, 0);
	if (flags == -1)
        flags = 0;
	rc = fcntl (self->s, F_SETFL, flags | O_NONBLOCK);
    errno_assert (rc != -1);

    /*  On TCP sockets switch off the Nagle's algorithm to get
        the best possible latency. */
    if ((self->domain == AF_INET || self->domain == AF_INET6) &&
          self->type == SOCK_STREAM) {
        opt = 1;
        rc = setsockopt (self->s, IPPROTO_TCP, TCP_NODELAY,
            (const char*) &opt, sizeof (opt));
        errno_assert (rc == 0);
    }

    /*  If applicable, disable delayed acknowledgements to improve latency. */
#if defined TCP_NODELACK
    opt = 1;
    rc = setsockopt (self->s, IPPROTO_TCP, TCP_NODELACK, &opt, sizeof (opt));
    errno_assert (rc == 0);
#endif

    /*  On some operating systems IPv4 mapping for IPv6 sockets is disabled
        by default. In such case, switch it on. */
#if defined IPV6_V6ONLY
    if (self->domain == AF_INET6) {
        only = 0;
        rc = setsockopt (self->s, IPPROTO_IPV6, IPV6_V6ONLY,
            (const char*) &only, sizeof (only));
        errno_assert (rc == 0);
    }
#endif
}

void sp_cp_init (struct sp_cp *self)
{
    sp_mutex_init (&self->sync, 0);
    sp_timeout_init (&self->timeout);
    sp_efd_init (&self->efd);
    sp_poller_init (&self->poller);
    sp_queue_init (&self->opqueue);
    sp_mutex_init (&self->events_sync, 0);
    sp_queue_init (&self->events);

    /*  Make poller listen on the internal efd object. */
    sp_poller_add (&self->poller, sp_efd_getfd (&self->efd),
        &self->efd_hndl);
    sp_poller_set_in (&self->poller, &self->efd_hndl);

    /*  Launch the worker thread. */
    self->stop = 0;
    sp_thread_init (&self->worker, sp_cp_worker, self);
}

void sp_cp_term (struct sp_cp *self)
{
    /*  Ask worker thread to terminate. */
    sp_mutex_lock (&self->sync);
    self->stop = 1;
    sp_efd_signal (&self->efd);
    sp_mutex_unlock (&self->sync);

    /*  Wait till it terminates. */
    sp_thread_term (&self->worker);

    /*  Remove the remaining internal fd from the poller. */
    sp_poller_rm (&self->poller, &self->efd_hndl);

    /*  Deallocate the resources. */
    sp_queue_term (&self->opqueue);
    sp_queue_term (&self->events);
    sp_mutex_term (&self->events_sync);
    sp_poller_term (&self->poller);
    sp_efd_term (&self->efd);
    sp_timeout_term (&self->timeout);
    sp_mutex_term (&self->sync);
}

void sp_cp_lock (struct sp_cp *self)
{
    sp_mutex_lock (&self->sync);
}

void sp_cp_unlock (struct sp_cp *self)
{
    sp_mutex_unlock (&self->sync);
}

static void sp_cp_worker (void *arg)
{
    int rc;
    struct sp_cp *self;
    int timeout;
    struct sp_cp_op_hndl *ophndl;
    struct sp_timeout_hndl *tohndl;
    struct sp_timer *timer;
    int op;
    struct sp_poller_hndl *phndl;
    struct sp_queue_item *it;
    struct sp_event *event;
    struct sp_usock *usock;
    size_t sz;
    int newsock;

    self = (struct sp_cp*) arg;

    sp_mutex_lock (&self->sync);

    while (1) {

        /*  Compute the time interval till next timer expiration. */
        timeout = sp_timeout_timeout (&self->timeout);

        /*  Wait for new events and/or timeouts. */
        sp_mutex_unlock (&self->sync);
again:
        rc = sp_poller_wait (&self->poller, timeout);
if (rc == -EINTR) goto again;
        errnum_assert (rc == 0, -rc);
        sp_mutex_lock (&self->sync);

        /*  Termination of the worker thread. */
        if (self->stop) {
            sp_mutex_unlock (&self->sync);
            break;
        }

        /*  Process the events in the opqueue. */
        while (1) {

            ophndl = sp_cont (sp_queue_pop (&self->opqueue),
                struct sp_cp_op_hndl, item);
            if (!ophndl)
                break;

            switch (ophndl->op) {
            case SP_USOCK_OP_IN:
                usock = sp_cont (ophndl, struct sp_usock, in.hndl);
                sp_poller_set_in (&self->poller, &usock->hndl);
                break;
            case SP_USOCK_OP_OUT:
                usock = sp_cont (ophndl, struct sp_usock, out.hndl);
                sp_poller_set_out (&self->poller, &usock->hndl);
                break;
            case SP_USOCK_OP_ADD:
                usock = sp_cont (ophndl, struct sp_usock, add_hndl);
                sp_poller_add (&self->poller, usock->s, &usock->hndl);
                break;
            case SP_USOCK_OP_RM:
                /*  TODO: Race condition here! usock may not exist at this point. */
                usock = sp_cont (ophndl, struct sp_usock, rm_hndl);
                sp_poller_rm (&self->poller, &usock->hndl);
                rc = close (usock->s);
                errno_assert (rc == 0);
                break;
            default:
                sp_assert (0);
            }
        }

        /*  Process any expired timers. */
        while (1) {
            rc = sp_timeout_event (&self->timeout, &tohndl);
            if (rc == -EAGAIN)
                break;
            errnum_assert (rc == 0, -rc);

            /*  Fire the timeout event. */
            timer = sp_cont (tohndl, struct sp_timer, hndl);
            sp_assert ((*timer->sink)->timeout);
            (*timer->sink)->timeout (timer->sink, timer);
        }

        /*  Process any events from the poller. */
        while (1) {
            rc = sp_poller_event (&self->poller, &op, &phndl);
            if (rc == -EAGAIN)
                break;
            errnum_assert (rc == 0, -rc);

            /*  The events delivered through the internal efd object require
                no action in response. Their sole intent is to interrupt the
                waiting. */
            if (phndl == &self->efd_hndl) {
                sp_assert (op == SP_POLLER_IN);
                sp_efd_unsignal (&self->efd);
                continue;
            }

            /*  Process the I/O event. */
            usock = sp_cont (phndl, struct sp_usock, hndl);
            switch (op) {
            case SP_POLLER_IN:
                switch (usock->in.op) {
                case SP_USOCK_INOP_RECV:
                    sz = usock->in.len;
                    rc = sp_usock_recv_raw (usock, usock->in.buf, &sz);
                    if (rc < 0)
                        goto err;
                    usock->in.len -= sz;
                    if (!usock->in.len) {
                        usock->in.op = SP_USOCK_INOP_NONE;
                        sp_poller_reset_in (&self->poller, &usock->hndl);
                        sp_assert ((*usock->sink)->received);
                        (*usock->sink)->received (usock->sink, usock);
                    }
                    break;                    
                case SP_USOCK_INOP_ACCEPT:
                    newsock = accept (usock->s, NULL, NULL);
                    if (newsock == -1) {

                        /*  The following are recoverable errors when accepting
                            a new connection. We can continue waiting for new
                            connection without even notifying the user. */
                        if (errno == ECONNABORTED ||
                              errno == EPROTO || errno == ENOBUFS ||
                              errno == ENOMEM || errno == EMFILE ||
                              errno == ENFILE)
                            break;

                        usock->in.op = SP_USOCK_INOP_NONE;
                        sp_poller_reset_in (&self->poller, &usock->hndl);
                        rc = -errno;
                        goto err;
                    }
                    usock->in.op = SP_USOCK_INOP_NONE;
                    sp_poller_reset_in (&self->poller, &usock->hndl);
                    sp_assert ((*usock->sink)->accepted);
                    (*usock->sink)->accepted (usock->sink, usock, newsock);
                    break;
                default:
                    /*  TODO:  When async connect succeeds both OUT and IN
                        are signaled, which means we can end up here. */
                    sp_assert (0);
                }
                break;
            case SP_POLLER_OUT:
                switch (usock->out.op) {
                case SP_USOCK_OUTOP_SEND:
                    sz = usock->out.len;
                    rc = sp_usock_send_raw (usock, usock->out.buf, &sz);
                    if (rc < 0)
                        goto err;
                    usock->out.buf += sz;
                    usock->out.len -= sz;
                    if (!usock->out.len) {
                        usock->out.op = SP_USOCK_OUTOP_NONE;
                        sp_poller_reset_out (&self->poller, &usock->hndl);
                        sp_assert ((*usock->sink)->sent);
                        (*usock->sink)->sent (usock->sink, usock);
                    }
                    break;
                case SP_USOCK_OUTOP_CONNECT:
                    usock->out.op = SP_USOCK_OUTOP_NONE;
                    sp_poller_reset_out (&self->poller, &usock->hndl);
                    rc = sp_usock_geterr (usock);
                    if (rc != 0)
                        goto err;
                    sp_assert ((*usock->sink)->connected);
                    (*usock->sink)->connected (usock->sink, usock);
                    break;
                default:
                    sp_assert (0);
                }
                break;
            case SP_POLLER_ERR:
                rc = sp_usock_geterr (usock);
err:
                sp_assert ((*usock->sink)->err);
                (*usock->sink)->err (usock->sink, usock, rc);
                break;
            default:
                sp_assert (0);
            }
        }

        /*  Process any external events. */
        sp_mutex_lock (&self->events_sync);
        while (1) {
            it = sp_queue_pop (&self->events);
            if (!it)
                break;
            event = sp_cont (it ,struct sp_event, item);
            sp_assert ((*event->sink)->event);
            (*event->sink)->event (event->sink, event);
            event->active = 0;
        }
        sp_mutex_unlock (&self->events_sync);
    }
}

void sp_usock_term (struct sp_usock *self)
{
    int rc;

    /*  TODO:  What happens if listen or connect wasn't called yet? Then the
        socket is not registered with the poller and we can't remove it. */

    /*  If the function is called from the worker thread remove the socket
        from the pollset straight away. */
    if (sp_thread_current (&self->cp->worker)) {
        sp_poller_rm (&self->cp->poller, &self->hndl);
        rc = close (self->s);
        errno_assert (rc == 0);
        if (self->in.batch)
            sp_free (self->in.batch);
        return;
    }

    /*  Otherwise, send an event to the worker thread. */
    /*  TODO: Possible race condition. */
    sp_assert (0);
    sp_queue_push (&self->cp->opqueue, &self->rm_hndl.item);
    sp_efd_signal (&self->cp->efd);

    if (self->in.batch)
        sp_free (self->in.batch);
}

int sp_usock_bind (struct sp_usock *self, const struct sockaddr *addr,
    sp_socklen addrlen)
{
    int rc;

    rc = bind (self->s, addr, addrlen);
    if (sp_slow (rc < 0))
       return -errno;

    return 0;
}

int sp_usock_listen (struct sp_usock *self, int backlog)
{
    int rc;
    int opt;

    /*  To allow for rapid restart of SP services, allow new bind to succeed
        immediately after previous instance of the process failed, skipping the
        grace period. */
    opt = 1;
    rc = setsockopt (self->s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt));
    errno_assert (rc == 0);

    rc = listen (self->s, backlog);
    if (sp_slow (rc < 0))
       return -errno;

    /*  If the function is called from the worker thread, modify the pollset
        straight away. Otherwise send an event to the worker thread. */
    if (sp_thread_current (&self->cp->worker))
        sp_poller_add (&self->cp->poller, self->s, &self->hndl);
    else {
        sp_queue_push (&self->cp->opqueue, &self->add_hndl.item);
        sp_efd_signal (&self->cp->efd);
    }

    return 0;
}

void sp_usock_connect (struct sp_usock *self, const struct sockaddr *addr,
    sp_socklen addrlen)
{
    int rc;

    /*  Make sure that there's no outbound operation already in progress. */
    sp_assert (self->out.op == SP_USOCK_OUTOP_NONE);

    /*  Adjust the handle. */
    self->out.op = SP_USOCK_OUTOP_CONNECT;

    /*  Do the connect itself. */
    rc = connect (self->s, addr, addrlen);

    /*  Immediate success. */
    if (sp_fast (rc == 0)) { 
        sp_assert ((*self->sink)->connected);
        (*self->sink)->connected (self->sink, self);
        return;
    }

    /*  Return unexpected errors to the caller. */
    if (sp_slow (errno != EINPROGRESS)) {
        sp_assert ((*self->sink)->err);
        (*self->sink)->err (self->sink, self, errno);
        return;
    }

    /*  If we are in the worker thread we can simply start polling for out.
        Otherwise, ask worker thread to start polling for out. */
    if (sp_thread_current (&self->cp->worker)) {
        sp_poller_add (&self->cp->poller, self->s, &self->hndl);
        sp_poller_set_out (&self->cp->poller, &self->hndl);
    }
    else {
        sp_queue_push (&self->cp->opqueue, &self->add_hndl.item);
        sp_queue_push (&self->cp->opqueue, &self->out.hndl.item);
        sp_efd_signal (&self->cp->efd);
    }
}

void sp_usock_accept (struct sp_usock *self)
{
    /*  Make sure that there's no inbound operation already in progress. */
    sp_assert (self->in.op == SP_USOCK_INOP_NONE);

    /*  Adjust the handle. */
    self->in.op = SP_USOCK_INOP_ACCEPT;

    /*  If we are in the worker thread we can simply start polling for out.
        Otherwise, ask worker thread to start polling for in. */
    if (sp_thread_current (&self->cp->worker))
        sp_poller_set_in (&self->cp->poller, &self->hndl);
    else {
        sp_queue_push (&self->cp->opqueue, &self->in.hndl.item);
        sp_efd_signal (&self->cp->efd);
    }
}

void sp_usock_send (struct sp_usock *self, const void *buf, size_t len)
{
    int rc;
    size_t nbytes;

    /*  Make sure that there's no outbound operation already in progress. */
    sp_assert (self->out.op == SP_USOCK_OUTOP_NONE);

    /*  Try to send the data immediately. */
    nbytes = len;
    rc = sp_usock_send_raw (self, buf, &nbytes);
    if (sp_slow (rc < 0)) {
        errnum_assert (rc == -ECONNRESET, -rc);
        sp_assert ((*self->sink)->err);
        (*self->sink)->err (self->sink, self, -rc);
        return;
    }

    /*  Success. */
    if (sp_fast (nbytes == len)) {
        sp_assert ((*self->sink)->sent);
        (*self->sink)->sent (self->sink, self);
        return;
    }

    /*  There are still data to send in the background. */ 
    self->out.op = SP_USOCK_OUTOP_SEND;
    self->out.buf = ((char*) buf) + nbytes;
    self->out.len = len - nbytes;

    /*  If we are in the worker thread we can simply start polling for out.
        Otherwise, ask worker thread to start polling for out. */
    if (sp_thread_current (&self->cp->worker))
        sp_poller_set_out (&self->cp->poller, &self->hndl);
    else {
        sp_queue_push (&self->cp->opqueue, &self->out.hndl.item);
        sp_efd_signal (&self->cp->efd);
    }
}

void sp_usock_recv (struct sp_usock *self, void *buf, size_t len)
{
    int rc;
    size_t nbytes;

    /*  Make sure that there's no inbound operation already in progress. */
    sp_assert (self->in.op == SP_USOCK_INOP_NONE);

    /*  Try to receive the data immediately. */
    nbytes = len;
    rc = sp_usock_recv_raw (self, buf, &nbytes);
    if (sp_slow (rc < 0)) {
        errnum_assert (rc == -ECONNRESET, -rc);
        sp_assert ((*self->sink)->err);
        (*self->sink)->err (self->sink, self, -rc);
        return;
    }

    /*  Success. */
    if (sp_fast (nbytes == len)) {
        sp_assert ((*self->sink)->received);
        (*self->sink)->received (self->sink, self);
        return;
    }

    /*  There are still data to receive in the background. */ 
    self->in.op = SP_USOCK_INOP_RECV;
    self->in.buf = ((char*) buf) + nbytes;
    self->in.len = len - nbytes;

    /*  If we are in the worker thread we can simply start polling for in.
        Otherwise, ask worker thread to start polling for in. */
    if (sp_thread_current (&self->cp->worker))
        sp_poller_set_in (&self->cp->poller, &self->hndl);
    else {
        sp_queue_push (&self->cp->opqueue, &self->in.hndl.item);
        sp_efd_signal (&self->cp->efd);
    }
}

static int sp_usock_send_raw (struct sp_usock *self, const void *buf,
    size_t *len)
{
    ssize_t nbytes;

#if defined MSG_NOSIGNAL
    nbytes = send (self->s, buf, *len, MSG_NOSIGNAL);
#else
    nbytes = send (self->s, buf, *len, 0);
#endif

    /*  Success. */
    if (sp_fast (nbytes >= 0)) {
        *len = (size_t) nbytes;
        return 0;
    }

    /*  Zero bytes sent. */
    if (nbytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        *len = 0;
        return 0;
    }

    /*  If the connection fails, return ECONNRESET. */
    sp_assert (errno == ECONNRESET || errno == ETIMEDOUT || errno == EPIPE);
    return -ECONNRESET;
}

static int sp_usock_recv_raw (struct sp_usock *self, void *buf, size_t *len)
{
    size_t sz;
    size_t length;
    ssize_t nbytes;

    /*  If batch buffer doesn't exist, allocate it. The point of delayed
        deallocation to allow non-receiving sockets, such as TCP listening
        sockets, to do without the batch buffer. */
    if (sp_slow (!self->in.batch)) {
        self->in.batch = sp_alloc (SP_USOCK_BATCH_SIZE);
        alloc_assert (self->in.batch);
    }

    /*  Try to satisfy the recv request by data from the batch buffer. */
    length = *len;
    sz = self->in.batch_len - self->in.batch_pos;
    if (sz) {
        if (sz > length)
            sz = length;
        memcpy (buf, self->in.batch + self->in.batch_pos, sz);
        self->in.batch_pos += sz;
        buf = ((char*) buf) + sz;
        length -= sz;
        if (!length)
            return 0;
    }

    /*  If recv request is greater than the batch buffer, get the data directly
        into the place. Otherwise, read data to the batch buffer. */
    if (length > SP_USOCK_BATCH_SIZE)
        nbytes = recv (self->s, buf, length, 0);
    else 
        nbytes = recv (self->s, self->in.batch, SP_USOCK_BATCH_SIZE, 0);

    /*  Handle any possible errors. */
    if (sp_slow (nbytes < 0)) {

        /*  Zero bytes received. */
        if (sp_fast (errno == EAGAIN || errno == EWOULDBLOCK))
            nbytes = 0;
        else {

            /*  If the peer closes the connection, return ECONNRESET. */
            sp_assert (nbytes == 0 || errno == ECONNRESET ||
                  errno == ENOTCONN || errno == ECONNREFUSED ||
                  errno == ETIMEDOUT || errno == EHOSTUNREACH);
            return -ECONNRESET;
        }
    }

    /*  If the data were received directly into the place we can return
        straight away. */
    if (length > SP_USOCK_BATCH_SIZE) {
        length -= nbytes;
        *len -= length;
        return 0;
    }

    /*  New data were read to the batch buffer. Copy the requested amount of it
        to the user-supplied buffer. */
    self->in.batch_len = nbytes;
    self->in.batch_pos = 0;
    if (nbytes) {
        sz = nbytes > length ? length : nbytes;
        memcpy (buf, self->in.batch, sz);
        length -= sz;
        self->in.batch_pos += sz;
    }

    *len -= length;
    return 0;
}

static int sp_usock_geterr (struct sp_usock *self)
{
    int rc;
    int err;
#if defined SP_HAVE_HPUX
    int errlen;
#else
    socklen_t errlen;
#endif

    err = 0;
    errlen = sizeof (err);
    rc = getsockopt (self->s, SOL_SOCKET, SO_ERROR, (char*) &err, &errlen);

    /*  On Solaris error is returned via errno. */
    if (rc == -1)
        return errno;

    /*  On other platforms the error is in err. */
    sp_assert (errlen == sizeof (err));
    return err;
}
