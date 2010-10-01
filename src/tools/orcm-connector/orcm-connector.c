/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
/* add the openrcm definitions */
#include "include/constants.h"
#include "runtime/runtime.h"

#include <inttypes.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "rtcp.h"

struct state {
    int sock;
    int rtcp;
    struct sockaddr_in addrA;
    struct sockaddr_in addrB;
    uint32_t ackA;
    uint32_t ackB;
};

void setup_control_connection(struct state *state)
{
    int ret;
    state->rtcp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (state->rtcp == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    ret = bind(state->rtcp, (struct sockaddr *)&state->addrA,
               sizeof(state->addrA));
    if (ret == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    ret = connect(state->rtcp, (struct sockaddr *)&state->addrB,
                  sizeof(state->addrB));
    if (ret == -1) {
        perror("connect");
        exit(EXIT_FAILURE);
    }
}

void *receiver(void *arg)
{
    struct rtcp_command command;
    struct state *state = arg;
    char buf[1024];
    ssize_t size;
    ssize_t sent;
    ssize_t ret;

    command.type = RTCP_ACKNOWLEDGE;
    while ((size = recv(state->sock, buf, sizeof(buf), 0)) > 0)
        for (sent = 0; sent < size; sent += ret) {
            ret = write(1, &buf[sent], size - sent);
            if (ret < 0) {
                perror("write");
                exit(EXIT_FAILURE);
            }
            state->ackA += ret;
            command.ackA = htonl(state->ackA);
            send(state->rtcp, &command, sizeof(command), 0);
        }

    if (size < 0) {
        perror("recv");
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "Done reading.\n");
    command.type = RTCP_SHUTDOWN_READING;
    send(state->rtcp, &command, sizeof(command), 0);
    recv(state->rtcp, &command, sizeof(command), 0);
    return NULL;
}

void *sender(void *arg)
{
    struct rtcp_command command;
    struct state *state = arg;
    char buf[1024];
    ssize_t size;
    ssize_t sent;
    ssize_t ret;

    while ((size = read(0, buf, sizeof(buf))) > 0)
        for (sent = 0; sent < size; sent += ret) {
            ret = send(state->sock, &buf[sent], size - sent, 0);
            if (ret < 0) {
                perror("send");
                exit(EXIT_FAILURE);
            }
        }
    if (size < 0) {
        perror("read");
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "Done writing.\n");
    command.type = RTCP_SHUTDOWN_WRITING;
    send(state->rtcp, &command, sizeof(command), 0);
    recv(state->rtcp, &command, sizeof(command), 0);
    ret = shutdown(state->sock, SHUT_WR);
    if (ret < 0) {
        perror("shutdown writing");
        exit(EXIT_FAILURE);
    }

    return NULL;
}

int main(int argc, char **argv)
{
    const char *local_host = NULL;
    const char *local_port = NULL;
    const char *remote_host = NULL;
    const char *remote_port = "echo";
    const char *shm_name = "/rtcp-client";
    int shm;
    int ret;
    int yes = 1;
    struct addrinfo hints;
    struct addrinfo *result;
    struct rtcp_command command;
    struct state *state;
    socklen_t len;
    pthread_t receiver_thread;
    pthread_t sender_thread;

    while ((ret = getopt(argc, argv, "H:P:h:p:?")) != -1)
        switch (ret) {
        case 'H':
            local_host = optarg;
            break;
        case 'P':
            local_port = optarg;
            break;
        case 'h':
            remote_host = optarg;
            break;
        case 'p':
            remote_port = optarg;
            break;
        default:
            fprintf(stderr, "Usage: %s [-H LOCALHOST] "
                    "[-P LOCALPORT] [-h REMOTEHOST] "
                    "[-p REMOTEPORT] [-r]\n", argv[0]);
            exit(EXIT_FAILURE);
        }

    /* Because this tool can be restarted, we need to ensure
     * that there is persistence in the connection info so
     * we don't confuse rtcp. Otherwise, we could ask rtcp
     * to create multiple connections, which causes badness.
     * So we store the connection info in a shared memory
     * region that persists beyond our own execution
     */
    shm = shm_open(shm_name, O_RDWR | O_CREAT, 0600);
    if (shm == -1) {
        perror("shm_open");
        exit(EXIT_FAILURE);
    }
    ret = ftruncate(shm, sizeof(*state));
    if (ret == -1) {
        perror("ftruncate");
        exit(EXIT_FAILURE);
    }
    state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE, MAP_SHARED,
                 shm, 0);
    if (state == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }
    close(shm);

    if (state->sock) {
        fprintf(stderr, "Recovering.\n");

        fprintf(stderr, "Establishing control connection.\n");
        setup_control_connection(state);

        fprintf(stderr, "Sending latest acknowledgment.\n");
        command.type = RTCP_ACKNOWLEDGE;
        command.ackA = htonl(state->ackA);
        send(state->rtcp, &command, sizeof(command), 0);

        fprintf(stderr, "Recovering connection.\n");
        state->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (state->sock == -1) {
            perror("socket");
            exit(EXIT_FAILURE);
        }
        ret = setsockopt(state->sock, SOL_SOCKET, SO_REUSEADDR, &yes,
                         sizeof(yes));
        if (ret == -1) {
            perror("setsockopt");
            exit(EXIT_FAILURE);
        }
        ret = bind(state->sock, (struct sockaddr *)&state->addrA,
                   sizeof(state->addrA));
        if (ret == -1) {
            perror("bind");
            exit(EXIT_FAILURE);
        }
        ret = connect(state->sock, (struct sockaddr *)&state->addrB,
                      sizeof(state->addrB));
        if (ret == -1) {
            perror("connect");
            exit(EXIT_FAILURE);
        }

        fprintf(stderr, "Querying sequence numbers.\n");
        command.type = RTCP_TELL;
        send(state->rtcp, &command, sizeof(command), 0);
        recv(state->rtcp, &command, sizeof(command), 0);
        fprintf(stderr, "The peer has acknowledged %"  PRIu32
                " bytes.\n",
                ntohl(command.ackB) - state->ackB);
    } else {
        fprintf(stderr, "Binding.\n");
        state->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (state->sock == -1) {
            perror("socket");
            exit(EXIT_FAILURE);
        }
        ret = setsockopt(state->sock, SOL_SOCKET, SO_REUSEADDR, &yes,
                         sizeof(yes));
        if (ret == -1) {
            perror("setsockopt");
            exit(EXIT_FAILURE);
        }

        if (local_port) {
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            ret = getaddrinfo(local_host, local_port, &hints,
                              &result);
            if (ret) {
                fprintf(stderr, "getaddrinfo: %s\n",
                        gai_strerror(ret));
                exit(EXIT_FAILURE);
            }
            ret = bind(state->sock, result->ai_addr,
                       result->ai_addrlen);
            if (ret) {
                perror("bind");
                exit(EXIT_FAILURE);
            }
            freeaddrinfo(result);
            len = sizeof(state->addrA);
            ret = getsockname(state->sock,
                              (struct sockaddr *)&state->addrA,
                              &len);
            if (ret == -1) {
                perror("getsockname");
                exit(EXIT_FAILURE);
            }
        }

        fprintf(stderr, "Resolving peer.\n");
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        ret = getaddrinfo(remote_host, remote_port, &hints, &result);
        if (ret) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
            exit(EXIT_FAILURE);
        }
        memcpy(&state->addrB, result->ai_addr, sizeof(state->addrB));
        freeaddrinfo(result);

        fprintf(stderr, "Establishing control connection.\n");
        setup_control_connection(state);

        fprintf(stderr, "Clearing RTCP's connection state.\n");
        command.type = RTCP_CLEAR;
        send(state->rtcp, &command, sizeof(command), 0);
        recv(state->rtcp, &command, sizeof(command), 0);

        fprintf(stderr, "Connecting.\n");
        ret = connect(state->sock, result->ai_addr, result->ai_addrlen);
        if (ret) {
            perror("connect");
            exit(EXIT_FAILURE);
        }

        fprintf(stderr, "Querying sequence numbers.\n");
        command.type = RTCP_TELL;
        send(state->rtcp, &command, sizeof(command), 0);
        recv(state->rtcp, &command, sizeof(command), 0);
        state->ackA = ntohl(command.ackA);
        state->ackB = ntohl(command.ackB);
    }

    fprintf(stderr, "Connected.\n");
    pthread_create(&receiver_thread, NULL, receiver, state);
    pthread_create(&sender_thread, NULL, sender, state);
    pthread_join(receiver_thread, NULL);
    pthread_join(sender_thread, NULL);

    fprintf(stderr, "Closing connection.\n");
    ret = close(state->sock);
    if (ret == -1) {
        perror("close");
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "Closing control connection.\n");
    ret = close(state->rtcp);
    if (ret == -1) {
        perror("close");
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "Removing persistent state.\n");
    ret = munmap(state, sizeof(*state));
    if (ret == -1) {
        perror("munmap");
        exit(EXIT_FAILURE);
    }
    ret = shm_unlink(shm_name);
    if (ret == -1) {
        perror("shm_unlink");
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "Done.\n");

    return EXIT_SUCCESS;
}
