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

typedef void (*program_run)(alarm_loop_t loop, void* data);

struct Program {
    program_run run;
};

static struct AlarmArray programs;


struct SensorIn {
    int pin_fd;
    char alarm;
    char pin_value;
};

struct HueLights {
    struct Program program;
    struct SensorIn *sensor;
    alarm_socket_t socket;
    alarm_timer_t on_timer;
    alarm_timer_t test_off_timer;
    unsigned int remote_host;
    int light;
    int extra_on;
    char *username;
    char on;
    char read_header;
};

struct Trigger {
    struct Program program;
    struct SensorIn *sensor;
    struct HueLights *lights;
    alarm_timer_t min_off_timer;
    alarm_timer_t extra_on_timer;
    alarm_timer_t max_on_timer;
    alarm_process_t process;
    alarm_socket_t socket;
    int relay_fd;
    unsigned int remote_host;
    int min_off;
    int extra_on;
    int max_on;
    unsigned short remote_port;
    char* command;
    char time_update[32];
};

static void switch_lights(alarm_loop_t loop, struct HueLights *lights, int on);

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
    if (trigger->relay_fd > -1) {
        lseek(trigger->relay_fd, SEEK_SET, 0);
        write(trigger->relay_fd, "0", 1);
    }
    if (!trigger->min_off_timer)
        trigger->min_off_timer = alarm_loop_add_timer(loop, trigger->min_off, trigger_timeout, data);
    else
        fprintf(stderr, "logic error: min_off_timer not NULL\n");
    if (trigger->lights && trigger->lights->on)
        switch_lights(loop, trigger->lights, 0);
}

static void turn_on(alarm_loop_t loop, struct Trigger* trigger) {
    if (trigger->relay_fd > -1) {
        lseek(trigger->relay_fd, SEEK_SET, 0);
        write(trigger->relay_fd, "1", 1);
    }
    if (trigger->command && !trigger->process) {
        fprintf(stdout, "start %s\n", trigger->command);
        trigger->process = alarm_loop_process_start(loop,
                trigger->command,
                trigger_process_read,
                NULL,
                trigger_process_written,
                trigger_process_error,
                trigger);
    }
    if (trigger->remote_host && !trigger->socket) {
        trigger->socket = alarm_loop_connect(loop,
                trigger->remote_host, trigger->remote_port,
                remote_connected, NULL, NULL, remote_error, trigger);
        if (!trigger->socket)
            fprintf(stderr, "failed to connect %X %s\n", trigger->remote_host, strerror(errno));
    }
    if (trigger->lights && !trigger->lights->on)
        switch_lights(loop, trigger->lights, 1);
}

static void trigger_run(alarm_loop_t loop, void* data) {
    struct Trigger* trigger = (struct Trigger*)data;

    if (trigger->sensor->pin_value == trigger->sensor->alarm) {
        if (trigger->extra_on_timer) {
            alarm_loop_remove_timer(loop, trigger->extra_on_timer);
            trigger->extra_on_timer = NULL;
        }
        if (trigger->min_off_timer || trigger->max_on_timer)
            return;
        trigger->max_on_timer = alarm_loop_add_timer(loop, trigger->max_on, trigger_timeout, data);
        turn_on(loop, trigger);
    } else {
        if (!trigger->extra_on_timer && trigger->max_on_timer)
            trigger->extra_on_timer = alarm_loop_add_timer(loop, trigger->extra_on, trigger_timeout, data);
    }
}

static void hue_error(alarm_loop_t loop, alarm_socket_t so, void* data)
{
    struct HueLights* lights = (struct HueLights*)data;
    if (errno)
        fprintf(stderr, "hue_remote error %X %s\n", lights->remote_host, strerror(errno));
    alarm_socket_destroy(loop, so);
    lights->socket = NULL;
}

static void hue_read(alarm_loop_t loop, alarm_socket_t so, const char* buffer, int size, void* data)
{
    struct HueLights* lights = (struct HueLights*)data;
    if (!lights->read_header) {
        const char* p = strstr(buffer, "\r\n\r\n");
        if (!p)
            return;
        lights->read_header = 1;
        if (p - buffer <= size - 4)
            return;
        buffer = p;
    }
    fprintf(stderr, "hue_remote read: %s\n", buffer);
    alarm_socket_destroy(loop, so);
    lights->socket = NULL;
}

static void hue_read_get(alarm_loop_t loop, alarm_socket_t so, const char* buffer, int size, void* data)
{
    struct HueLights* lights = (struct HueLights*)data;
    if (strstr(buffer, "\"on\":false")) {
        fprintf(stderr, "hue_remote_get read: off (%s)\n", buffer);
        alarm_socket_destroy(loop, so);
        lights->socket = NULL;
    } else if (strstr(buffer, "\"on\":true")) {
        fprintf(stderr, "hue_remote_get read: on (%s)\n", buffer);
        alarm_socket_destroy(loop, so);
        lights->socket = NULL;
        switch_lights(loop, lights, 0);
    }
}

static void hue_connected_set(alarm_loop_t loop, alarm_socket_t so, void* data)
{
    struct HueLights* lights = (struct HueLights*)data;
    const char* tmpl = "PUT /api/%s/lights/%d/state HTTP/1.1\r\n"
        "User-Agent: alarm\r\n"
        "Accept: */*\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "\r\n"
        "{\"on\":%s}";
    char buf[256];
    if (lights->on)
        snprintf(buf, sizeof (buf), tmpl, lights->username, lights->light, 11, "true");
    else
        snprintf(buf, sizeof (buf), tmpl, lights->username, lights->light, 12, "false");
    fprintf(stderr, "hue_connected to %X\n", lights->remote_host);
    lights->read_header = 0;
    alarm_socket_write(loop, so, buf, strlen(buf));
}

static void hue_connected_get(alarm_loop_t loop, alarm_socket_t so, void* data)
{
    struct HueLights* lights = (struct HueLights*)data;
    const char* tmpl = "GET /api/%s/lights/%d HTTP/1.1\r\n"
        "User-Agent: alarm\r\n"
        "Accept: */*\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    char buf[256];
    snprintf(buf, sizeof (buf), tmpl, lights->username, lights->light);
    fprintf(stderr, "hue_connected_get to %X\n", lights->remote_host);
    lights->read_header = 0;
    alarm_socket_write(loop, so, buf, strlen(buf));
}

static void lights_timeout(alarm_loop_t loop, alarm_timer_t timer, const struct timeval *tv, void* data) {
    struct HueLights* lights = (struct HueLights*)data;
    (void)tv;
    if (timer == lights->on_timer) {
        lights->on_timer = NULL;
        if (lights->on) {
            switch_lights(loop, lights, 0);
        } else {
            fprintf(stderr, "Err time-out lights already off\n");
        }
    } else if (timer == lights->test_off_timer) {
        lights->test_off_timer = NULL;
        if (lights->socket)
            alarm_socket_destroy(loop, lights->socket);
        lights->socket = alarm_loop_connect(loop,
                lights->remote_host, 80,
                hue_connected_get, hue_read_get, NULL, hue_error, lights);
    }
}

static void switch_lights(alarm_loop_t loop, struct HueLights *lights, int on) {
    fprintf(stderr, "Turn light %s\n", on ? "on" : "off");
    if (!lights->remote_host) {
        fprintf(stderr, "hue-bridge IP not received\n");
        return;
    }
    if (lights->test_off_timer) {
        alarm_loop_remove_timer(loop, lights->test_off_timer);
        lights->test_off_timer = NULL;
    }
    lights->on = on;
    if (lights->socket)
        alarm_socket_destroy(loop, lights->socket);
    lights->socket = alarm_loop_connect(loop,
            lights->remote_host, 80,
            hue_connected_set, hue_read, NULL, hue_error, lights);
    if (!lights->socket)
        fprintf(stderr, "Hue failed to connect %s\n", strerror(errno));
    if ( !on)
        lights->test_off_timer = alarm_loop_add_timer(loop, 5000, lights_timeout, lights);
}

static void lights_run(alarm_loop_t loop, void* data) {
    struct HueLights* lights = (struct HueLights*)data;

    if (lights->sensor->pin_value == lights->sensor->alarm) {
        if (!lights->on) {
            time_t UTC;
            struct tm* lt;
            int sh, sm, rh, rm;

            time(&UTC);
            lt = localtime(&UTC);
            sunrise_sunset(lt, &sh, &sm, &rh, &rm);
            fprintf(stderr, "T %d:%d-%d:%d\n", sh, sm, rh, rm);

            if ((lt->tm_hour < rh || (lt->tm_hour == rh && lt->tm_min <= rm))
                    || (lt->tm_hour > sh || (lt->tm_hour == sh && lt->tm_min >= sm))) {
                switch_lights(loop, lights, 1);
            }
        }
        if (lights->on_timer) {
            alarm_loop_remove_timer(loop, lights->on_timer);
            lights->on_timer = NULL;
        }
    } else if (lights->on) {
        if (!lights->on_timer) {
            lights->on_timer = alarm_loop_add_timer(loop, lights->extra_on, lights_timeout, data);
        } else {
            fprintf(stderr, "Err on timer already running\n");
        }
    }
}

static void sensor_event(alarm_loop_t loop, alarm_fd_t afd, int fd, void* data) {
    struct SensorIn* sensor = (struct SensorIn*)data;
    int count, i;
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
    sensor->pin_value = c;
    fprintf(stdout, "input \x1B[01;%dm%c\x1B[0m\n", c == sensor->alarm ? 31 : 32, c);
    for (i = 0; i < programs.size; ++i)
        ((struct Program*)programs.data[i])->run(loop, programs.data[i]);
}

static void trigger_error(alarm_loop_t loop, alarm_fd_t fd, void* data) {
    (void)fd;
    (void)data;
    fprintf(stderr, "trigger error %s\n", strerror(errno));
    alarm_loop_exit(loop);
}

static int open_gpio_value(int pin, int flag) {
    char buf[256];
    int sz = snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/value", pin);
    if (sz > sizeof(buf) -2) {
        errno = ENOBUFS;
        return -1;
    }
    return open(buf, flag);
}

static int ip4addr(char *host) {
    struct addrinfo hints, *addrs;
    int s;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    s = getaddrinfo(host, NULL, &hints, &addrs);
    if (s || !addrs) {
        fprintf(stderr, "host lookup: %s %s\n", host, gai_strerror(s));
        return 0;
    }
    s = ((struct sockaddr_in*)addrs->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(addrs);
    return s;
}

static int hue_init(struct HueLights *lights, /*const*/ char* config) {
    char *str = strtok(config, ":");
    if (!str)
        return -1;
    lights->remote_host = ip4addr(str);
    if (!lights->remote_host)
        return -2;
    str = strtok(NULL, ":");
    if (!str)
        return -3;
    lights->username = strdup(str);
    str = strtok(NULL, ":");
    if (!str)
        return -4;
    lights->light = strtol(str, NULL, 10);
    str = strtok(NULL, ":");
    if (str)
        lights->extra_on = str ? strtol(str, NULL, 10) : 0;
    lights->program.run = lights_run;
    fprintf(stderr, "Using Hue hub %08X with username %s and light %d with time %d\n",
            lights->remote_host, lights->username, lights->light, lights->extra_on);
    return 0;
}

static void usage(const char* program, const char* msg) {
    if (msg)
        fprintf(stderr, "%s\n\n", msg);
    fprintf(stderr, "usage: %s -i input-pin [-alarm 0|1][-o output-pin] [-c camera-app] [-r host:port] [-min ms] [-extra ms] [-max ms] [-hue hue-hub-ip:username:light[:extra]]\n", program);
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
    int trigger_pin = 0;
    int alarm_pin = 0;
    int i;
    struct SensorIn sensor;
    struct Trigger trigger;
    struct HueLights lights;

    memset(&sensor, 0, sizeof (struct SensorIn));
    sensor.pin_value = '0';
    sensor.alarm = '1';

    memset(&trigger, 0, sizeof (struct Trigger));
    trigger.program.run = trigger_run;
    trigger.sensor = &sensor;
    trigger.min_off = 5000;
    trigger.extra_on = 15000;
    trigger.max_on = 60000;

    memset(&lights, 0, sizeof (struct HueLights));
    lights.sensor = &sensor;

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
            sensor.alarm = argv[i][0];
        } else if (!strcmp(argv[i], "-r")) {
            char *p, *host;
            if (++i == argc)
                usage(argv[0], "option -r requires host:port");
            p = strchr(argv[i], ':');
            if (!p)
                usage(argv[0], "option -r requires host:port");
            host = strndup(argv[i], p-argv[i]);
            trigger.remote_port = strtol(p+1, NULL, 10);
            trigger.remote_host = ip4addr(host);
            free(host);
            if (trigger.remote_host) {
                struct timespec uptime;
                struct timeval now, off, sub;
                clock_gettime(CLOCK_MONOTONIC, &uptime);
                off.tv_sec = uptime.tv_sec;
                off.tv_usec = uptime.tv_nsec/1000;
                gettimeofday(&now, NULL);
                timersub(&now, &off, &sub);
                snprintf(trigger.time_update, sizeof (trigger.time_update), "%lu.%lu\n", sub.tv_sec, sub.tv_usec);

                fprintf(stderr, "host lookup: %X:%d\n", trigger.remote_host, trigger.remote_port);
            }
        } else if (!strcmp(argv[i], "-c")) {
            if (++i == argc)
                usage(argv[0], "option -c requires a program name");
            trigger.command = argv[i];
        } else if (!strcmp(argv[i], "-hue")) {
            if (++i == argc || hue_init(&lights, argv[i]))
                usage(argv[0], "option -hue requires hue-hub-ip:username:light argument");
        } else {
            usage(argv[0], NULL);
        }
    }
    if (trigger_pin <= 0)
        usage(argv[0], "option -i requires a positive number");
    sensor.pin_fd = open_gpio_value(trigger_pin, O_RDONLY);
    if (sensor.pin_fd < 0) {
        fprintf(stderr, "Trigger input %s\n", strerror(errno));
        exit(-1);
    }
    if (alarm_pin > 0) {
        trigger.relay_fd = open_gpio_value(alarm_pin, O_WRONLY);
        if (trigger.relay_fd < 0) {
            fprintf(stderr, "Trigger input %s\n", strerror(errno));
            exit(-1);
        }
    }

    alarm_array_init(&programs);
    alarm_array_append(&programs, &trigger);
    if (lights.remote_host) {
        if (!lights.extra_on)
            trigger.lights = &lights;
        alarm_array_append(&programs, &lights);
    }

    loop = alarm_new_loop();
    alarm_loop_add_fd(loop, sensor.pin_fd, NULL, NULL, sensor_event, trigger_error, &sensor);
    alarm_loop_run(loop);
    alarm_loop_free(loop);

    close(sensor.pin_fd);
    if (trigger.relay_fd > -1)
        close(trigger.relay_fd);
    if (lights.username)
        free(lights.username);
    alarm_array_clear(&programs);

    fprintf(stderr, "Have a nice day!\n");

    return 0;
}
