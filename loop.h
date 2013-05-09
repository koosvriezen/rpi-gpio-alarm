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

#ifndef _ALARM_LOOP_H_
#define _ALARM_LOOP_H_

#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AlarmLoop* alarm_loop_t;
alarm_loop_t alarm_new_loop();
void alarm_loop_run(alarm_loop_t loop);
void alarm_loop_exit(alarm_loop_t loop);
void alarm_loop_free(alarm_loop_t loop);

typedef struct AlarmTimer* alarm_timer_t;
typedef void (*alarm_timer_callback)(alarm_loop_t loop, alarm_timer_t timer, const struct timeval *tv, void* data);
alarm_timer_t alarm_loop_add_timer(alarm_loop_t loop, int msec, alarm_timer_callback callback, void* data);
void alarm_loop_remove_timer(alarm_loop_t loop, alarm_timer_t timer);

typedef struct AlarmFileDescriptor* alarm_fd_t;
typedef void (*alarm_fd_read_cb)(alarm_loop_t loop, alarm_fd_t afd, const char* buffer, int size, void* data);
typedef void (*alarm_fd_written_cb)(alarm_loop_t loop, alarm_fd_t afd, void* data);
typedef void (*alarm_fd_except_cb)(alarm_loop_t loop, alarm_fd_t afd, int fd, void* data);
typedef void (*alarm_fd_error_cb)(alarm_loop_t loop, alarm_fd_t afd, void* data);
alarm_fd_t alarm_loop_add_fd(alarm_loop_t loop,
        int fd,
        alarm_fd_read_cb read_cb,
        alarm_fd_written_cb written_cb,
        alarm_fd_except_cb except_cb,
        alarm_fd_error_cb error_cb,
        void* data);
void alarm_fd_write(alarm_loop_t loop, alarm_fd_t afd, const char* buffer, const int size);

typedef struct AlarmProcess* alarm_process_t;
typedef void (*alarm_process_read_cb)(alarm_loop_t loop, alarm_process_t process, const char* buffer, int size, void* data);
typedef void (*alarm_process_written_cb)(alarm_loop_t loop, alarm_process_t process, void* data);
typedef void (*alarm_process_error_cb)(alarm_loop_t loop, alarm_process_t process, void* data);
alarm_process_t alarm_loop_process_start(alarm_loop_t loop,
        const char* command,
        alarm_process_read_cb std_out,
        alarm_process_read_cb std_err,
        alarm_process_written_cb std_in_written_cb,
        alarm_process_error_cb error_cb,
        void* data);
void alarm_process_write(alarm_loop_t loop, alarm_process_t process, const char* buffer, const int size);
void alarm_process_signal(alarm_loop_t loop, alarm_process_t process, int sig);

typedef struct AlarmSocket* alarm_socket_t;
typedef void (*alarm_socket_connected_cb)(alarm_loop_t loop, alarm_socket_t so, void* data);
typedef void (*alarm_socket_new_connection_cb)(alarm_loop_t loop, alarm_socket_t so, unsigned int remote_host, void* data);
typedef void (*alarm_socket_read_cb)(alarm_loop_t loop, alarm_socket_t so, const char* buffer, int size, void* data);
typedef void (*alarm_socket_written_cb)(alarm_loop_t loop, alarm_socket_t so, void* data);
typedef void (*alarm_socket_error_cb)(alarm_loop_t loop, alarm_socket_t so, void* data);
alarm_socket_t alarm_loop_connect(alarm_loop_t loop,
        unsigned int host,
        unsigned short port,
        alarm_socket_connected_cb connected_cb,
        alarm_socket_read_cb read_cb,
        alarm_socket_written_cb written_cb,
        alarm_socket_error_cb error_cb,
        void* data);
alarm_socket_t alarm_loop_socket_listen(alarm_loop_t loop,
        unsigned int bind,
        unsigned short port,
        alarm_socket_new_connection_cb connected_cb,
        alarm_socket_read_cb read_cb,
        alarm_socket_written_cb written_cb,
        alarm_socket_error_cb error_cb,
        void* data);
void alarm_socket_write(alarm_loop_t loop, alarm_socket_t so, const char* buffer, const int size);
void alarm_socket_destroy(alarm_loop_t loop, alarm_socket_t so);

#ifdef __cplusplus
}
#endif

#endif
