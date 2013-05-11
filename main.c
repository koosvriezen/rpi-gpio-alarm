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

struct Trigger {
    alarm_timer_t min_off_timer;
    alarm_timer_t extra_on_timer;
    alarm_timer_t max_on_timer;
    alarm_process_t process;
    alarm_socket_t socket;
    int alarm_fd;
    unsigned int remote_host;
    int min_off;
    int extra_on;
    int max_on;
    unsigned short remote_port;
    char alarm;
    char alarm_value;
    char* program;
    char time_update[32];
};

static void trigger_process_read(alarm_loop_t loop, alarm_process_t process, const char* buffer, int size, void* data)
{
    fprintf(stdout, "process stdout %s", buffer);
}

static void trigger_process_written(alarm_loop_t loop, alarm_process_t process, void* data)
{
    fprintf(stdout, "process stdin written\n");
}

static void trigger_process_error(alarm_loop_t loop, alarm_process_t process, void* data)
{
    struct Trigger* trigger = (struct Trigger*)data;
    trigger->process = NULL;
    fprintf(stdout, "camera stopped\n");
}

static void remote_error(alarm_loop_t loop, alarm_socket_t so, void* data)
{
    struct Trigger* trigger = (struct Trigger*)data;
    fprintf(stderr, "remote error %X %s\n", trigger->remote_host, strerror(errno));
    alarm_socket_destroy(loop, so);
    trigger->socket = NULL;
}

static void remote_connected(alarm_loop_t loop, alarm_socket_t so, void* data)
{
    struct Trigger* trigger = (struct Trigger*)data;
    fprintf(stderr, "connected to %X\n", trigger->remote_host);
    alarm_socket_write(loop, so, trigger->time_update, strlen(trigger->time_update));
}

static void trigger_timeout(alarm_loop_t loop, alarm_timer_t timer, const struct timeval *tv, void* data) {
    struct Trigger* trigger = (struct Trigger*)data;

    if (timer == trigger->min_off_timer) {
        trigger->min_off_timer = NULL;
        if (!trigger->process)
            return;
        fprintf(stderr, "camera still running\n");
    } else if (timer == trigger->extra_on_timer) {
        trigger->extra_on_timer = NULL;
        if (trigger->max_on_timer) {
            alarm_loop_remove_timer(loop, trigger->max_on_timer);
            trigger->max_on_timer = NULL;
        } else {
            fprintf(stderr, "logic error: on extra_on_timer, max_on_timer is NULL\n");
        }
    } else if (timer == trigger->max_on_timer) {
        trigger->max_on_timer = NULL;
        if (trigger->extra_on_timer) {
            alarm_loop_remove_timer(loop, trigger->extra_on_timer);
            trigger->extra_on_timer = NULL;
        }
    }
    if (trigger->process) {
        time_t t;
        struct tm *tm;
        char buf[64];
        t = tv->tv_sec;
        tm = localtime(&t);
        strftime(buf, sizeof (buf), "%Y-%m-%d %H:%M:%S", tm);

        fprintf(stdout, "%s stop camera\n", buf);
        alarm_process_signal(loop, trigger->process, SIGINT);
    }
    if (trigger->socket) {
        alarm_socket_destroy(loop, trigger->socket);
        trigger->socket = NULL;
    }
    if (trigger->alarm_fd > -1) {
        lseek(trigger->alarm_fd, SEEK_SET, 0);
        write(trigger->alarm_fd, "0", 1);
    }
    if (!trigger->min_off_timer)
        trigger->min_off_timer = alarm_loop_add_timer(loop, trigger->min_off, trigger_timeout, data);
    else
        fprintf(stderr, "logic error: min_off_timer not NULL\n");
}

static void trigger_event(alarm_loop_t loop, alarm_fd_t afd, int fd, void* data) {
    struct Trigger* trigger = (struct Trigger*)data;
    int count;
    char c;
    (void)afd;

    lseek(fd, SEEK_SET, 0);
    while ((count = read(fd, &c, 1)) < 1) {
        if (count == 0 || (EAGAIN != errno && EINTR != errno)) {
            fprintf(stderr, "input %s\n", strerror(errno));
            alarm_loop_exit(loop);
            return;
        }
    }
    trigger->alarm_value = c;
    fprintf(stdout, "input \x1B[01;%dm%c\x1B[0m\n", c == trigger->alarm ? 31 : 32, c);
    if (c == trigger->alarm) {
        if (trigger->extra_on_timer) {
            alarm_loop_remove_timer(loop, trigger->extra_on_timer);
            trigger->extra_on_timer = NULL;
        }
        if (trigger->min_off_timer || trigger->max_on_timer)
            return;
        trigger->max_on_timer = alarm_loop_add_timer(loop, trigger->max_on, trigger_timeout, data);
        if (trigger->alarm_fd > -1) {
            lseek(trigger->alarm_fd, SEEK_SET, 0);
            write(trigger->alarm_fd, "1", 1);
        }
        if (trigger->program && !trigger->process) {
            fprintf(stdout, "start %s\n", trigger->program);
            trigger->process = alarm_loop_process_start(loop,
                    trigger->program,
                    trigger_process_read,
                    NULL,
                    trigger_process_written,
                    trigger_process_error,
                    data);
        }
        if (trigger->remote_host && !trigger->socket) {
            trigger->socket = alarm_loop_connect(loop,
                    trigger->remote_host, trigger->remote_port,
                    remote_connected, NULL, NULL, remote_error, trigger);
            if (!trigger->socket)
                fprintf(stderr, "failed to connect %X %s\n", trigger->remote_host, strerror(errno));
        }
    } else {
        if (!trigger->extra_on_timer && trigger->max_on_timer)
            trigger->extra_on_timer = alarm_loop_add_timer(loop, trigger->extra_on, trigger_timeout, data);
    }
}

static void trigger_error(alarm_loop_t loop, alarm_fd_t fd, void* data) {
    (void)fd;
    (void)data;
    fprintf(stderr, "trigger error %s\n", strerror(errno));
    alarm_loop_exit(loop);
}

int open_gpio_value(int pin, int flag) {
    char buf[256];
    int sz = snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/value", pin);
    if (sz > sizeof(buf) -2) {
        errno = ENOBUFS;
        return -1;
    }
    return open(buf, flag);
}

static void usage(const char* program, const char* msg) {
    if (msg)
        fprintf(stderr, "%s\n\n", msg);
    fprintf(stderr, "usage: %s -i input-pin [-alarm 0|1][-o output-pin] [-c camera-app] [-r host:port] [-min ms] [-extra ms] [-max ms]\n", program);
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

int main(int argc, char** argv) {
    alarm_loop_t loop;
    int trigger_fd;
    int trigger_pin = 0;
    int alarm_pin = 0;
    int i;
    struct Trigger trigger;

    memset(&trigger, 0, sizeof (struct Trigger));
    trigger.alarm_fd = -1;
    trigger.min_off = 5000;
    trigger.extra_on = 15000;
    trigger.max_on = 60000;
    trigger.alarm_value = '0';
    trigger.alarm = '0';

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-i")) {
            read_int_arg(argc, argv, &i, &trigger_pin, "option -i requires a pin number");
        } else if (!strcmp(argv[i], "-o")) {
            read_int_arg(argc, argv, &i, &alarm_pin, "option -o requires a pin number");
        } else if (!strcmp(argv[i], "-min")) {
            read_int_arg(argc, argv, &i, &trigger.min_off, "option -min requires msec");
        } else if (!strcmp(argv[i], "-extra")) {
            read_int_arg(argc, argv, &i, &trigger.extra_on, "option -extra requires msec");
        } else if (!strcmp(argv[i], "-max")) {
            read_int_arg(argc, argv, &i, &trigger.max_on, "option -max requires msec");
        } else if (!strcmp(argv[i], "-alarm")) {
            if (++i == argc || strlen(argv[i]) > 1 || (argv[i][0] != '0' && argv[i][0] != '1'))
                usage(argv[0], "option -alarm requires a 0 or 1");
            trigger.alarm = argv[i][0];
        } else if (!strcmp(argv[i], "-r")) {
            char *p, *host;
            struct addrinfo hints, *addrs;
            int s;
            if (++i == argc)
                usage(argv[0], "option -r requires host:port");
            p = strchr(argv[i], ':');
            if (!p)
                usage(argv[0], "option -r requires host:port");
            host = strndup(argv[i], p-argv[i]);
            trigger.remote_port = strtol(p+1, NULL, 10);
            memset(&hints, 0, sizeof(struct addrinfo));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;

            s = getaddrinfo(host, NULL, &hints, &addrs);
            free(host);
            if (s || !addrs) {
                fprintf(stderr, "host lookup: %s\n", gai_strerror(s));
            } else {
                struct timespec uptime;
                struct timeval now, off, sub;
                clock_gettime(CLOCK_MONOTONIC, &uptime);
                off.tv_sec = uptime.tv_sec;
                off.tv_usec = uptime.tv_nsec/1000;
                gettimeofday(&now, NULL);
                timersub(&now, &off, &sub);
                snprintf(trigger.time_update, sizeof (trigger.time_update), "%lu.%lu\n", sub.tv_sec, sub.tv_usec);

                trigger.remote_host = ((struct sockaddr_in*)addrs->ai_addr)->sin_addr.s_addr;
                freeaddrinfo(addrs);
                fprintf(stderr, "host lookup: %X:%d\n", trigger.remote_host, trigger.remote_port);
            }
        } else if (!strcmp(argv[i], "-c")) {
            if (++i == argc)
                usage(argv[0], "option -c requires a program name");
            trigger.program = argv[i];
        } else {
            usage(argv[0], NULL);
        }
    }
    if (trigger_pin <= 0)
        usage(argv[0], "option -i requires a positive number");
    trigger_fd = open_gpio_value(trigger_pin, O_RDONLY);
    if (trigger_fd < 0) {
        fprintf(stderr, "Trigger input %s\n", strerror(errno));
        exit(-1);
    }
    if (alarm_pin > 0) {
        trigger.alarm_fd = open_gpio_value(alarm_pin, O_WRONLY);
        if (trigger.alarm_fd < 0) {
            fprintf(stderr, "Trigger input %s\n", strerror(errno));
            exit(-1);
        }
    }

    loop = alarm_new_loop();

    alarm_loop_add_fd(loop, trigger_fd, NULL, NULL, trigger_event, trigger_error, &trigger);

    alarm_loop_run(loop);
    alarm_loop_free(loop);

    close(trigger_fd);
    if (trigger.alarm_fd > -1)
        close(trigger.alarm_fd);

    fprintf(stderr, "Have a nice day!\n");

    return 0;
}
