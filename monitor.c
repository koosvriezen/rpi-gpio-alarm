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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/stat.h>
#include <sys/types.h>

static void monitor_timeout(alarm_loop_t loop, alarm_timer_t timer, const struct timeval *tv, void* data);

struct Monitor {
    struct AlarmArray clients;
    alarm_process_t process;
    unsigned short remote_port;
    int initial_wait;
    int poll_wait;
    char* program;
    char* output_dir;;
};

static void monitor_init(struct Monitor *monitor)
{
    memset(monitor, 0, sizeof (struct Monitor));
    alarm_array_init(&monitor->clients);
    monitor->initial_wait = 150;
    monitor->poll_wait = 100;
}

static void monitor_clear(struct Monitor *monitor)
{
    alarm_array_clear(&monitor->clients);
}

struct HttpConnection {
    struct Monitor* monitor;
    alarm_socket_t control_socket;
    alarm_timer_t connect_back_timer;
    struct timeval time_offset;
    unsigned int remote_host;
    int have_headers;
    int found_boundary;
    int read_jpeg;
    char *mime_boundary;
    char *timestamp;
    char *buffer;
    const char* output_dir;;
    int buf_size;
    int buf_pos;
    int jpeg_mime;
    size_t jpeg_size;
    int failure;
};

static void http_destroy(struct HttpConnection* http)
{
    int i;
    for (i = 0; i < http->monitor->clients.size; ++i) {
        if (http == http->monitor->clients.data[i]) {
            alarm_array_remove(&http->monitor->clients, i);
            break;
        }
    }
    if (http->buffer)
        free(http->buffer);
    if (http->mime_boundary)
        free(http->mime_boundary);
    if (http->timestamp)
        free(http->timestamp);
    free(http);
}

static void parse_time(const char *buf, struct timeval *tv)
{
    const char *e;
    tv->tv_sec = strtol(buf, (char**)&e, 10);
    if (*e)
        tv->tv_usec = strtol(e+1, NULL, 10);
    else
        tv->tv_usec = 0;
}

static void process_read(alarm_loop_t loop, alarm_process_t process, const char* buffer, int size, void* data)
{
    fprintf(stdout, "process stdout %s", buffer);
}

static void written(alarm_loop_t loop, alarm_process_t process, void* data)
{
    fprintf(stdout, "process stdin written\n");
}

static void process_error(alarm_loop_t loop, alarm_process_t process, void* data)
{
    struct Monitor* monitor = (struct Monitor*)data;
    monitor->process = NULL;
    fprintf(stdout, "camera stopped\n");
}

static void http_connected(alarm_loop_t loop, alarm_socket_t so, void* data)
{
    fprintf(stderr, "http connected\n");
    alarm_socket_write(loop, so, "GET /?action=stream HTTP/1.0\r\n\r\n", 36);
}

static void http_read(alarm_loop_t loop, alarm_socket_t process, const char* buffer, int size, void* data)
{
    struct HttpConnection* http = (struct HttpConnection*)data;
    int i = 0;

    if (http->failure)
        return;

    if (!http->buffer) {
        http->buffer = (char*)malloc(1024);
        http->buffer[0] = 0;
        http->buf_size = 1024;
    }

    for (; !http->failure && i < size; ++i) {
        if (!http->read_jpeg) {
            /*fprintf(stderr, "jpeg header %c %d\n", buffer[i] > 31 ? buffer[i] : '?', buffer[i]);*/
            switch (buffer[i]) {
            case '\r':
                break;
            case '\n':
                if (!http->buf_pos) {
                    if (!http->have_headers) {
                        http->have_headers = 1;
                        http->failure = !http->mime_boundary;
                    } else {
                        if (!http->found_boundary)
                            break;
                        http->read_jpeg = 1;
                        if (http->jpeg_size > http->buf_size) {
                            http->buf_size = http->jpeg_size;
                            http->buffer = (char*)realloc(http->buffer, http->buf_size);
                        }
                    }
                } else {
                    const char *s = NULL, *v;
                    http->buffer[http->buf_pos] = 0;
                    v = strchr(http->buffer, ':');
                    if (v) {
                        s = v++;
                        while (*v == ' ')
                            ++v;
                    }
                    /*fprintf(stderr, "header: %s\n", http->buffer);*/
                    if (s && !strncmp(http->buffer, "Content-Type", 12)) {
                        if (!http->have_headers) {
                            s = strstr(s + 1, "boundary=");
                            if (s)
                                http->mime_boundary = strdup(s + 9);
                        } else if (!strcmp(v, "image/jpeg")) {
                            http->jpeg_mime = 1;
                        }
                    } else if (http->have_headers) {
                        if (s && !strncmp(http->buffer, "Content-Length", 14)) {
                            http->jpeg_size = strtoul(v, NULL, 10);
                            if (http->jpeg_size > 1024*1024) {
                                fprintf(stderr, "jpeg size to large: %lu\n", http->jpeg_size);
                                http->failure = 1;
                            }
                        } else if (s && !strncmp(http->buffer, "X-Timestamp", 11)) {
                            if (strlen(v) < 32) {
                                char buf[64], tbuf[64];
                                unsigned int l = ntohl(http->remote_host);
                                struct timeval tv, res;
                                time_t t;
                                struct tm *tm;
                                parse_time(v, &tv);
                                timeradd(&http->time_offset, &tv, &res);
                                t = res.tv_sec;
                                tm = localtime(&t);
                                strftime(tbuf, sizeof (tbuf), "%Y%m%d_%H%M%S", tm);
                                snprintf(buf, sizeof (buf), "%s.%03d_%d.%d.%d.%d"
                                        , tbuf, res.tv_usec/1000
                                        , (l & 0xFF000000) >> 24
                                        , (l & 0xFF0000) >> 16
                                        , (l & 0xFF00) >> 8
                                        , l & 0xFF);
                                http->timestamp = strdup(buf);
                            } else
                                fprintf(stderr, "invalid X-Timestamp\n");
                        } else if (strstr(http->buffer, http->mime_boundary)) {
                            http->jpeg_mime = 0;
                            http->jpeg_size = 0;
                            if (http->timestamp) {
                                free(http->timestamp);
                                http->timestamp = NULL;
                            }
                            http->found_boundary = 1;
                        }
                    }
                }
                http->buffer[0] = 0;
                http->buf_pos = 0;
                break;
            default:
                if (http->buf_pos + 1 >= http->buf_size) {
                    http->buf_size = http->buf_size * 1.4;
                    http->buffer = (char*)realloc(http->buffer, http->buf_size);
                }
                http->buffer[http->buf_pos++] = buffer[i];
                break;
            }
        } else {
            http->buffer[http->buf_pos++] = buffer[i];
            if (http->buf_pos == http->jpeg_size) {
                if (http->timestamp) {
                    int fd;
                    char *name = (char*)malloc(strlen(http->output_dir) + 38);
                    sprintf(name, "%s/%s.jpg", http->output_dir, http->timestamp);
                    fd = open(name, O_WRONLY|O_CREAT|O_TRUNC, 0600);
                    if (fd > -1) {
                        write(fd, http->buffer, http->buf_pos);
                        close(fd);
                        fprintf(stdout, "wrote jpeg %s size %lu\n", name, http->jpeg_size);
                    } else {
                        fprintf(stderr, "skipped jpeg '%s' %s\n", name, strerror(errno));
                    }
                    free(name);
                } else {
                    fprintf(stderr, "skipped jpeg, no timestamp\n");
                }
                http->read_jpeg = 0;
                http->buf_pos = 0;
                http->found_boundary = 0;
            }
        }
    }
}

static void http_error(alarm_loop_t loop, alarm_socket_t so, void* data)
{
    struct HttpConnection* http = (struct HttpConnection*)data;
    fprintf(stdout, "http connect error\n");
    alarm_socket_destroy(loop, so);
    if (http->control_socket) {
        http->connect_back_timer = alarm_loop_add_timer(loop,
                http->monitor->poll_wait, monitor_timeout, http);
    } else {
        http_destroy(http);
    }
}

static void connection_disconnect(alarm_loop_t loop, alarm_socket_t so, void* data)
{
    struct Monitor* monitor = (struct Monitor*)data;
    int i;
    for (i = 0; i < monitor->clients.size; ++i) {
        struct HttpConnection *conn = (struct HttpConnection*)monitor->clients.data[i];
        if (conn->control_socket == so) {
            fprintf(stdout, "End of alarm at %X\n", conn->remote_host);
            conn->control_socket = NULL;
            if (conn->connect_back_timer) {
                fprintf(stderr, "not yet connected\n");
                alarm_loop_remove_timer(loop, conn->connect_back_timer);
                http_destroy(conn);
            }
            break;
        }
    }
    alarm_socket_destroy(loop, so);
}

static void monitor_timeout(alarm_loop_t loop, alarm_timer_t timer, const struct timeval *tv, void* data)
{
    struct HttpConnection* http = (struct HttpConnection*)data;
    http->connect_back_timer = NULL;
    alarm_loop_connect(loop,
            http->remote_host, http->monitor->remote_port,
            http_connected, http_read, NULL, http_error, http);
}

static void connection_read(alarm_loop_t loop, alarm_socket_t so, const char* buffer, int size, void* data)
{
    struct Monitor* monitor = (struct Monitor*)data;
    int i;
    for (i = 0; i < monitor->clients.size; ++i) {
        struct HttpConnection *conn = (struct HttpConnection*)monitor->clients.data[i];
        if (conn->control_socket == so) {
            parse_time(buffer, &conn->time_offset);
            break;
        }
    }
}

static void new_connection(alarm_loop_t loop, alarm_socket_t so, unsigned int remote, void* data)
{
    struct Monitor* monitor = (struct Monitor*)data;
    fprintf(stdout, "Alarm at %X\n", remote);
    if (monitor->remote_port) {
        struct HttpConnection *http = (struct HttpConnection*)malloc(sizeof (struct HttpConnection));
        memset(http, 0, sizeof (struct HttpConnection));
        http->monitor = monitor;
        http->control_socket = so;
        http->output_dir = monitor->output_dir ? monitor->output_dir : ".";
        http->remote_host = remote;
        http->connect_back_timer = alarm_loop_add_timer(loop,
                monitor->initial_wait, monitor_timeout, http);
        alarm_array_append(&monitor->clients, http);
    }
}


static void usage(const char* program, const char* msg) {
    if (msg)
        fprintf(stderr, "%s\n\n", msg);
    fprintf(stderr, "usage: %s [-p listen-port] [-s program-to-run] [-r remote-http-port] [-o output-dir]\n", program);
    exit(1);
}

static void read_int_arg(int argc, char** argv, int *i, int *v, const char* err)
{
    char* end;
    if (++(*i) == argc)
        usage(argv[0], err);
    *v = strtol(argv[*i], &end, 10);
    if (*end)
        usage(argv[0], err);
}

int main(int argc, char** argv)
{
    alarm_loop_t loop;
    alarm_socket_t server;
    int i, port;
    struct Monitor monitor;

    monitor_init(&monitor);

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-p")) {
            read_int_arg(argc, argv, &i, &port, "option -p requires a TCP port number");
        } else if (!strcmp(argv[i], "-r")) {
            int rp;
            read_int_arg(argc, argv, &i, &rp, "option -p requires a TCP port number");
            monitor.remote_port = (unsigned short)rp;
        } else if (!strcmp(argv[i], "-o")) {
            if (++i == argc)
                usage(argv[0], "option -o requires a directory name");
            monitor.output_dir = argv[i];
        } else if (!strcmp(argv[i], "-s")) {
            if (++i == argc)
                usage(argv[0], "option -s requires a program name");
            monitor.program = argv[i];
        } else {
            usage(argv[0], NULL);
        }
    }
    loop = alarm_new_loop();

    server = alarm_loop_socket_listen(loop, 0, (unsigned short)port,
            new_connection, connection_read, NULL, connection_disconnect, &monitor);

    alarm_loop_run(loop);

    alarm_socket_destroy(loop, server);

    alarm_loop_free(loop);
    monitor_clear(&monitor);

    fprintf(stderr, "Have a nice day!\n");

    return 0;
}
