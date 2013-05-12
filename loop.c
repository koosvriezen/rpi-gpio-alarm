/*
 * Copyright (c) 2013, Koos Vriezen <koos.vriezen@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the <organization> nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "loop.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>

enum AlarmFlags {
    FlagNone=0, FlagRun=1, FlagProcessing=2,
    FlagMarkDelete=4, FlagFdClosed=8,
    FlagPassiveSocket=16, FlagConnectingSocket=32
};

struct AlarmLoopAdmin
{
    int flags;
};

static void init_loop_admin(struct AlarmLoopAdmin* admin, int f)
{
    admin->flags = f;
}

#define TEST(type, flag) (type->admin.flags & (flag))
#define FLAG(type, flag) (type->admin.flags |= (flag))
#define CLEAR(type, flag) (type->admin.flags &= ~(flag))

struct AlarmFileDescriptor
{
    struct AlarmLoopAdmin admin;
    int fd;
    char* buffer;
    int buffer_size;
    alarm_fd_read_cb read_callback;
    alarm_fd_error_cb written_callback;
    alarm_fd_except_cb except_callback;
    alarm_fd_error_cb error_callback;
    void* data;
};

static void fd_init(struct AlarmFileDescriptor* afd, int fd,
        alarm_fd_read_cb rcb,
        alarm_fd_written_cb wcb,
        alarm_fd_except_cb ecb,
        alarm_fd_error_cb err,
        void* d)
{
    init_loop_admin(&afd->admin, FlagNone);
    afd->fd = fd;
    afd->buffer = NULL;
    afd->buffer_size = 0;
    afd->read_callback = rcb;
    afd->written_callback = wcb;
    afd->except_callback = ecb;
    afd->error_callback = err;
    afd->data = d;
}

static void fd_close(alarm_fd_t afd)
{
    if (TEST(afd, FlagFdClosed) || afd->fd < 0)
        return;
    close(afd->fd);
    FLAG(afd, FlagMarkDelete | FlagFdClosed);
}

struct AlarmTimer
{
    struct AlarmLoopAdmin admin;
    struct timeval tv;
    int msec;
    alarm_timer_cb callback;
    void* data;
};

static void timer_init(struct AlarmTimer* at, int ms, alarm_timer_cb cb, void* d)
{
    init_loop_admin(&at->admin, FlagNone);
    at->tv.tv_sec = 0;
    at->msec = ms;
    at->callback = cb;
    at->data = d;
}

struct AlarmLoop
{
    struct AlarmLoopAdmin admin;
    struct AlarmArray fds;
    struct AlarmArray timers;
};

static void loop_init(struct AlarmLoop *loop)
{
    init_loop_admin(&loop->admin, FlagNone);
    alarm_array_init(&loop->fds);
    alarm_array_init(&loop->timers);
}

alarm_loop_t alarm_new_loop()
{
    struct AlarmLoop* loop = (struct AlarmLoop*)malloc(sizeof (struct AlarmLoop));
    loop_init(loop);
    return loop;
}

static inline void add_time (struct timeval* tv, int ms)
{
    if (ms >= 1000) {
        tv->tv_sec += ms / 1000;
        ms %= 1000;
    }
    tv->tv_sec += (tv->tv_usec + ms*1000) / 1000000;
    tv->tv_usec = (tv->tv_usec + ms*1000) % 1000000;
}

static inline int diff_time (const struct timeval* tv1, const struct timeval* tv2)
{
    return (tv1->tv_sec - tv2->tv_sec) * 1000 + (tv1->tv_usec - tv2->tv_usec) /1000;
}

alarm_timer_t alarm_loop_add_timer(alarm_loop_t loop, int msec, alarm_timer_cb cb, void* data)
{
    struct AlarmTimer* at = (struct AlarmTimer*)malloc(sizeof (struct AlarmTimer));;
    timer_init(at, msec, cb, data);
    alarm_array_append(&loop->timers, at);
    return at;
}

void alarm_loop_remove_timer(alarm_loop_t loop, alarm_timer_t timer)
{
    int i;
    for (i = 0; i < loop->timers.size; ++i) {
        struct AlarmTimer* t = (struct AlarmTimer*)loop->timers.data[i];
        if (t == timer) {
            if (TEST(t, FlagProcessing)) {
                FLAG(t, FlagMarkDelete);
            } else {
                free(t);
                alarm_array_remove(&loop->timers, i);
            }
            break;
        }
    }
}

alarm_fd_t alarm_loop_add_fd(alarm_loop_t loop, int fd,
        alarm_fd_read_cb rcb,
        alarm_fd_written_cb wcb,
        alarm_fd_except_cb ecb,
        alarm_fd_error_cb err,
        void* data)
{
    struct AlarmFileDescriptor* af = (struct AlarmFileDescriptor*)malloc(sizeof (struct AlarmFileDescriptor));
    while (fcntl(fd, F_SETFD, fcntl(fd , F_GETFL) | O_NONBLOCK | FD_CLOEXEC) < 0) {
        switch (errno) {
        case EAGAIN:
        case EINTR:
            break;
        default:
            fprintf(stderr, "fcntl %s\n", strerror(errno));
            return NULL;
        }
    }
    fd_init(af, fd, rcb, wcb, ecb, err, data);
    alarm_array_append(&loop->fds, af);
    return af;
}

void alarm_fd_write(alarm_loop_t loop, alarm_fd_t fd, const char* buffer, const int size)
{
    int i;
    for (i = 0; i < loop->fds.size; ++i) {
        struct AlarmFileDescriptor *afd = (struct AlarmFileDescriptor*)loop->fds.data[i];
        if (fd == afd) {
            if (afd->buffer_size)
                afd->buffer = (char*)realloc(afd->buffer, afd->buffer_size + size);
            else
                afd->buffer = (char*)malloc(size);
            memcpy(afd->buffer + afd->buffer_size, buffer, size);
            afd->buffer_size += size;
            break;
        }
    }
}

static int interupted;

static void child_signal(int s)
{
    (void)s;
    waitpid(-1, NULL, WNOHANG);
    /*fprintf(stderr, "sigchild\n");*/
}

static void int_signal(int s)
{
    (void)s;
    interupted = 1;
}

void alarm_loop_run (alarm_loop_t loop)
{
    int i;

    static int initialized;
    if (!initialized) {
        struct sigaction act;
        initialized = 1;
        memset (&act, 0, sizeof(act));
        act.sa_handler = child_signal;
        sigaction(SIGCHLD, &act, NULL);
        signal(SIGINT, int_signal);
    }

    FLAG(loop, FlagRun);

    while (!interupted && TEST(loop, FlagRun)) {
        struct timeval tv, *ptv = NULL;
        fd_set rdset;
        fd_set wrset;
        fd_set exset;

        if (loop->timers.size) {
            int timout = 0x7FFFFFFF;
            gettimeofday (&tv, 0L);
            for (i = 0; i < loop->timers.size; ++i) {
                struct AlarmTimer* timer = (struct AlarmTimer*)loop->timers.data[i];
                FLAG(timer, FlagProcessing);
                if (!timer->tv.tv_sec) {
                    timer->tv = tv;
                    add_time(&timer->tv, timer->msec);
                    if (timer->msec < timout)
                        timout = timer->msec;
                } else {
                    int dt = diff_time (&timer->tv, &tv);
                    if (TEST(loop, FlagRun) && dt < 2) {
                        timer->callback(loop, timer, &tv, timer->data);
                        FLAG(timer, FlagMarkDelete);
                    } else if (dt < timout) {
                        timout = dt;
                    }
                }
            }
            for (i = 0; i < loop->timers.size;) {
                struct AlarmTimer* timer = (struct AlarmTimer*)loop->timers.data[i];
                if (TEST(timer, FlagMarkDelete)) {
                    free(timer);
                    alarm_array_remove(&loop->timers, i);
                } else {
                    CLEAR(timer, FlagProcessing);
                    ++i;
                }
            }
            if (!TEST(loop, FlagRun))
                return;
            if (loop->timers.size) {
                tv.tv_sec = 0;
                tv.tv_usec = 0;
                add_time(&tv, timout);
                ptv = &tv;
            }
        }

        int maxfd = -1;
        FD_ZERO (&rdset);
        FD_ZERO (&wrset);
        FD_ZERO (&exset);
        /*fprintf(stderr, "select [");*/
        for (i = 0; i < loop->fds.size;) {
            struct AlarmFileDescriptor *fd = (struct AlarmFileDescriptor*)loop->fds.data[i];
            if (TEST(fd, FlagMarkDelete)) {
                free(fd);
                alarm_array_remove(&loop->fds, i);
            } else {
#define ALARM_ADD_TO_SET(fd,set,test)   \
                if (test) {             \
                    FD_SET(fd, &set);   \
                    if (maxfd < fd)     \
                        maxfd = fd;     \
                }
                ALARM_ADD_TO_SET(fd->fd, rdset, fd->read_callback)
                ALARM_ADD_TO_SET(fd->fd, wrset, fd->buffer)
                ALARM_ADD_TO_SET(fd->fd, wrset, TEST(fd, FlagConnectingSocket))
                ALARM_ADD_TO_SET(fd->fd, exset, fd->except_callback)
#undef ALARM_ADD_TO_SET
                /*fprintf(stderr, " %d", fd->fd);*/
                ++i;
            }
        }
        /*fprintf(stderr, "] nr=%d timeout %d.%d timers %d\n", loop->fds.size, ptv ? tv.tv_sec : 0, ptv ? tv.tv_usec : 0, loop->timers.size);*/
        int selval = select(maxfd + 1, &rdset, &wrset, &exset, ptv);
        if (selval > 0) {
            for (i = 0; TEST(loop, FlagRun) && i < loop->fds.size; ++i) {
                struct AlarmFileDescriptor *fd = (struct AlarmFileDescriptor*)loop->fds.data[i];
                if (!TEST(fd, FlagMarkDelete) && FD_ISSET(fd->fd, &rdset)) {
                    if (TEST(fd, FlagPassiveSocket)) {
                        fd->read_callback(loop, fd, "", 0, fd->data);
                    } else {
                        char buf[1024];
                        int nr = read(fd->fd, buf, sizeof (buf)-1);
                        if (nr > 0) {
                            buf[nr] = 0;
                            fd->read_callback(loop, fd, buf, nr, fd->data);
                        } else if (nr == 0 || !(EAGAIN == errno || EINTR == errno)) {
                            fd_close(fd);
                            if (fd->error_callback)
                                fd->error_callback(loop, fd, fd->data);
                        }
                    }
                }
                if (TEST(loop, FlagRun) && !TEST(fd, FlagMarkDelete) && FD_ISSET(fd->fd, &wrset)) {
                    int nr = 0;
                    if (fd->buffer_size)
                        nr = write(fd->fd, fd->buffer, fd->buffer_size);
                    if (!fd->buffer_size || nr > 0) {
                        if (nr == fd->buffer_size) {
                            free(fd->buffer);
                            fd->buffer = NULL;
                            fd->buffer_size = 0;
                            if (fd->written_callback)
                                fd->written_callback(loop, fd, fd->data);
                        } else {
                            memmove(fd->buffer, fd->buffer+nr, fd->buffer_size-nr);
                            fd->buffer_size -= nr;
                        }
                    } else if (nr == 0 || !(EAGAIN == errno || EINTR == errno)) {
                        fd_close(fd);
                        if (fd->error_callback)
                            fd->error_callback(loop, fd, fd->data);
                    }
                }
                if (TEST(loop, FlagRun) && !TEST(fd, FlagMarkDelete) && FD_ISSET(fd->fd, &exset)) {
                    fd->except_callback(loop, fd, fd->fd, fd->data);
                }
            }
        }
    }
}

void alarm_loop_exit(alarm_loop_t loop)
{
    CLEAR(loop, FlagRun);
}

void alarm_loop_free(alarm_loop_t loop)
{
    int i;
    for (i = 0; i < loop->timers.size; ++i)
        free(loop->timers.data[i]);
    for (i = 0; i < loop->fds.size; ++i)
        free(loop->fds.data[i]);
    alarm_array_clear(&loop->fds);
    alarm_array_clear(&loop->timers);
    free(loop);
}

struct AlarmProcess
{
    int pid;
    alarm_fd_t fdin;
    alarm_fd_t fdout;
    alarm_fd_t fderr;
    char* buffer;
    int buffer_size;
    alarm_process_read_cb std_out_callback;
    alarm_process_read_cb std_err_callback;
    alarm_process_written_cb std_in_written_callback;
    alarm_process_error_cb error_callback;
    void* data;
};

static void process_init(struct AlarmProcess* ap,
        int pid,
        alarm_fd_t ifd, alarm_fd_t ofd, alarm_fd_t efd,
        alarm_process_read_cb std_out_cb,
        alarm_process_read_cb std_err_cb,
        alarm_process_written_cb std_in_written_cb,
        alarm_process_error_cb error_cb,
        void* d)
{
    ap->pid = pid;
    ap->fdin = ifd;
    ap->fdout = ofd;
    ap->fderr = efd;
    ap->buffer = NULL;
    ap->buffer_size = 0;
    ap->std_out_callback = std_out_cb;
    ap->std_err_callback = std_err_cb;
    ap->std_in_written_callback = std_in_written_cb;
    ap->error_callback = error_cb;
    ap->data = d;
}

static void process_stdout(alarm_loop_t loop, alarm_fd_t fd, const char* buffer, int size, void* data)
{
    struct AlarmProcess* p = (struct AlarmProcess*)data;
    (void)fd;
    if (p->std_out_callback)
        p->std_out_callback(loop, p, buffer, size, data);
}

static void process_stderr(alarm_loop_t loop, alarm_fd_t fd, const char* buffer, int size, void* data)
{
    struct AlarmProcess* p = (struct AlarmProcess*)data;
    (void)fd;
    p->std_err_callback(loop, p, buffer, size, p->data);
}

static void process_written(alarm_loop_t loop, alarm_fd_t fd, void* data)
{
    struct AlarmProcess* p = (struct AlarmProcess*)data;
    (void)fd;
    if (p->std_in_written_callback)
        p->std_in_written_callback(loop, p, p->data);
}

static void loop_process_error(alarm_loop_t loop, alarm_fd_t fd, void* data)
{
    struct AlarmProcess* p = (struct AlarmProcess*)data;
    int i;
    (void)fd;
    for (i = 0; i < loop->fds.size; ++i) {
        struct AlarmFileDescriptor *fd = (struct AlarmFileDescriptor*)loop->fds.data[i];
        if (fd == p->fdin || fd == p->fdout || fd == p->fderr)
            fd_close(fd);
    }
    if (p->error_callback)
        p->error_callback(loop, p, p->data);
    free(p);
}

alarm_process_t alarm_loop_process_start(alarm_loop_t loop,
        const char* command,
        alarm_process_read_cb std_out,
        alarm_process_read_cb std_err,
        alarm_process_written_cb std_in_written_cb,
        alarm_process_error_cb error_cb,
        void* data)
{
    pid_t child;
    int in[2] = { -1, -1 };
    int out[2] = { -1, -1 };
    int err[2] = { -1, -1 };

    if (pipe(in) == -1) {
        fprintf(stderr, "pipe %s\n", strerror(errno));
        return NULL;
    }
    if (pipe(out) == -1) {
        fprintf(stderr, "pipe %s\n", strerror(errno));
        return NULL;
    }
    if (std_err && pipe(err) == -1) {
        fprintf(stderr, "pipe %s\n", strerror(errno));
        return NULL;
    }
    child = fork();
    if (child < 0) {
        fprintf(stderr, "fork %s", strerror(errno));
        return NULL;
    }
    if (child) {
        struct AlarmProcess* p;
        alarm_fd_t fdin, fdout, fderr = NULL;
        p = (struct AlarmProcess*)malloc(sizeof (struct AlarmProcess));
        close(in[0]);
        fdin = alarm_loop_add_fd(loop, in[1], NULL, process_written, NULL, loop_process_error, p);
        close(out[1]);
        fdout = alarm_loop_add_fd(loop, out[0], process_stdout, NULL, NULL, loop_process_error, p);
        if (err[0] != -1) {
            close(err[1]);
            fderr = alarm_loop_add_fd(loop, err[0], process_stderr, NULL, NULL, NULL, p);
        }
        process_init(p, child, fdin, fdout, fderr, std_out, std_err, std_in_written_cb, error_cb, data);
        return p;
    } else {
        setsid();
        char* const argv[] = { strdup(command), NULL };
        close(in[1]);
        dup2(in[0], 0);
        close(out[0]);
        dup2(out[1], 1);
        close(out[1]);
        if (err[0] != -1) {
            close(err[0]);
            dup2(err[1], 2);
            close(err[1]);
        }
        if (execv(command, argv) == -1 )
            fprintf(stderr, "execv %s\n", strerror(errno));
    }
    return NULL;
}

void alarm_process_write(alarm_loop_t loop, alarm_process_t p, const char* buffer, const int size)
{
    alarm_fd_write(loop, p->fdin, buffer, size);
}

void alarm_process_signal(alarm_loop_t loop, alarm_process_t p, int sig)
{
    if (p->pid > 0)
        kill(-1 * p->pid, sig);
}


struct AlarmSocket
{
    struct AlarmLoopAdmin admin;
    alarm_fd_t socket;
    alarm_socket_connected_cb connected_callback;
    alarm_socket_new_connection_cb new_connection_callback;
    alarm_socket_read_cb read_callback;
    alarm_socket_written_cb written_callback;
    alarm_socket_error_cb error_callback;
    void* data;
};

static void socket_init(struct AlarmSocket* so,
        alarm_socket_connected_cb ccb,
        alarm_socket_new_connection_cb ncb,
        alarm_socket_read_cb rcb,
        alarm_socket_written_cb wcb,
        alarm_socket_error_cb err,
        void* d)
{
    init_loop_admin(&so->admin, FlagNone);
    so->socket = NULL;
    so->connected_callback = ccb;
    so->new_connection_callback = ncb;
    so->read_callback = rcb;
    so->written_callback = wcb;
    so->error_callback = err;
    so->data = d;
}

static void socket_written(alarm_loop_t loop, alarm_fd_t afd, void* data)
{
    struct AlarmSocket* so = (struct AlarmSocket*)data;
    if (TEST(afd, FlagConnectingSocket)) {
        CLEAR(afd, FlagConnectingSocket);
        socklen_t len = sizeof (errno);
        if (!getsockopt(afd->fd, SOL_SOCKET, SO_ERROR, &errno, &len) && !errno) {
            if (so->connected_callback)
                so->connected_callback(loop, so, so->data);
        } else {
            fd_close(so->socket);
            so->socket = NULL;
            if (so->error_callback)
                so->error_callback(loop, so, so->data);
        }
    } else if (so->written_callback) {
        so->written_callback(loop, so, so->data);
    }
}

static void socket_error(alarm_loop_t loop, alarm_fd_t afd, void* data)
{
    struct AlarmSocket* so = (struct AlarmSocket*)data;
    so->socket = NULL;
    if (so->error_callback)
        so->error_callback(loop, so, so->data);
}

static void socket_read(alarm_loop_t loop, alarm_fd_t afd, const char* buffer, int size, void* data)
{
    struct AlarmSocket* so = (struct AlarmSocket*)data;
    if (so->new_connection_callback) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof (from);
        int fd = accept(so->socket->fd, (struct sockaddr*)&from, &fromlen);
        if (fd > 0) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK | FD_CLOEXEC);
            struct AlarmSocket *client = (struct AlarmSocket*)malloc(sizeof (struct AlarmSocket));
            socket_init(client, NULL, NULL, so->read_callback, so->written_callback, so->error_callback, so->data);
            client->socket = alarm_loop_add_fd(loop, fd, socket_read, socket_written, NULL, socket_error, client);
            so->new_connection_callback(loop, client, from.sin_addr.s_addr, client->data);
        } else {
            perror("accept");
        }
    } else if (so->read_callback) {
        so->read_callback(loop, so, buffer, size, so->data);
    }
}

alarm_socket_t alarm_loop_connect(alarm_loop_t loop,
        unsigned int host,
        unsigned short port,
        alarm_socket_connected_cb connected_cb,
        alarm_socket_read_cb read_cb,
        alarm_socket_written_cb written_cb,
        alarm_socket_error_cb error_cb,
        void* data)
{
    struct sockaddr_in server;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK | FD_CLOEXEC);
    server.sin_family = AF_INET;
    memcpy(&server.sin_addr.s_addr, &host, sizeof (unsigned int));
    server.sin_port = htons(port);
    while (1) {
        if (!connect(sock, (struct sockaddr*)&server, sizeof (server))
                || errno == EINPROGRESS) {
            struct AlarmSocket *so = (struct AlarmSocket*)malloc(sizeof (struct AlarmSocket));
            socket_init(so, connected_cb, NULL, read_cb, written_cb, error_cb, data);
            so->socket = alarm_loop_add_fd(loop, sock, socket_read, socket_written, NULL, socket_error, so);
            FLAG(so->socket, FlagConnectingSocket);
            return so;
        }
        if (errno != EINTR)
            break;
    }
    return NULL;
}

alarm_socket_t alarm_loop_socket_listen(alarm_loop_t loop,
        unsigned int net,
        unsigned short port,
        alarm_socket_new_connection_cb connection_cb,
        alarm_socket_read_cb read_cb,
        alarm_socket_written_cb written_cb,
        alarm_socket_error_cb error_cb,
        void* data)
{
    struct AlarmSocket *so;
    struct sockaddr_in server;
    int opt = 1;
    int sock;
    sock = socket(AF_INET, SOCK_STREAM, 0);
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons (port);
    setsockopt (sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt));
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK | FD_CLOEXEC);
    if (bind (sock, (struct sockaddr*) &server, sizeof (server))) {
        perror("bind");
        close(sock);
        sock = -1;
        return NULL;
    }
    if (listen (sock, 5))
        perror("listen");

    so = (struct AlarmSocket*)malloc(sizeof (struct AlarmSocket));
    socket_init(so, NULL, connection_cb, read_cb, written_cb, error_cb, data);
    so->socket = alarm_loop_add_fd(loop, sock, socket_read, NULL, NULL, NULL, so);
    FLAG(so->socket, FlagPassiveSocket);
    return so;
}

void alarm_socket_write(alarm_loop_t loop, alarm_socket_t so, const char* buffer, const int size)
{
    alarm_fd_write(loop, so->socket, buffer, size);
}

void alarm_socket_destroy(alarm_loop_t loop, alarm_socket_t so)
{
    if (so->socket)
        fd_close(so->socket);
    free(so);
}
