/*
 * Copyright (c) 2021, Xilinx Inc. and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/stat.h>
#include "graphClient.h"
#include <signal.h>
#include <setjmp.h>
#include <dfx-mgr/print.h>
#include <dfx-mgr/assert.h>

#define MAX_FD 25
struct message message;
int sock_fd;

ssize_t
sock_fd_write(int sock, void *buf, ssize_t buflen, int *fd, int fdcount)
{
    ssize_t     size;
    struct msghdr   msg;
    struct iovec    iov;
    union {
        struct cmsghdr  cmsghdr;
        char control[CMSG_SPACE(sizeof (int) * MAX_FD)];
    } cmsgu;
    struct cmsghdr  *cmsg;

    iov.iov_base = buf;
    iov.iov_len = buflen;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_flags = 0;

    if (fd[0] != -1) {
        msg.msg_control = cmsgu.control;
        msg.msg_controllen = sizeof(cmsgu.control);

        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_len = CMSG_LEN(MAX_FD * sizeof (int));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;

        memset(CMSG_DATA(cmsg), '\0', MAX_FD * sizeof(int));
        memcpy(CMSG_DATA(cmsg), fd, fdcount * sizeof(int));
    } else {
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
    }

    size = sendmsg(sock, &msg, 0);

    if (size < 0)
        perror ("sendmsg");
    return size;
}

ssize_t sock_fd_read(int sock, struct message *buf, int *fd, int *fdcount)
{
	ssize_t size = 0;
    struct msghdr   msg;
    struct iovec    iov;
    union {
        struct cmsghdr  cmsghdr;
        char        control[CMSG_SPACE(sizeof (int) * MAX_FD)];
    } cmsgu;
    struct cmsghdr  *cmsg;
    iov.iov_base = buf;
    iov.iov_len = 1024*4;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
	msg.msg_flags = 0;
    msg.msg_control = cmsgu.control;
    msg.msg_controllen = sizeof(cmsgu.control);
    size = recvmsg (sock, &msg, 0);
    if (size < 0) {
        perror ("recvmsg");
        exit(1);
    }
	*fdcount = buf->fdcount;
    cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
        if (cmsg->cmsg_level != SOL_SOCKET) {
            fprintf (stderr, "invalid cmsg_level %d\n",
                 cmsg->cmsg_level);
            exit(1);
        }
        if (cmsg->cmsg_type != SCM_RIGHTS) {
            fprintf (stderr, "invalid cmsg_type %d\n",
                 cmsg->cmsg_type);
            exit(1);
        }
	}

    memcpy(fd, CMSG_DATA(cmsg), sizeof(int)*(*fdcount));
    return size;
}

int initSocket(socket_t* gs){
	int status;
	
	if ((gs->sock_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0)) == -1){
		error("socket error !!");
		return -1;
	}
	
	memset(&gs->socket_address, 0, sizeof(struct sockaddr_un));
	gs->socket_address.sun_family = AF_UNIX;
	strncpy(gs->socket_address.sun_path, SERVER_SOCKET, sizeof(gs->socket_address.sun_path) - 1);
	status = connect(gs->sock_fd, (const struct sockaddr *) &gs->socket_address, sizeof(struct sockaddr_un));
	if (status < 0){
		error("connect failed");
		return -1;
	}
	return 0;
}

int graphClientSubmit(socket_t *gs, char* json, int size, int *fd, int *fdcount){
	struct message send_message, recv_message;
	memset (&send_message, '\0', sizeof(struct message));
	send_message.id = GRAPH_INIT;
	send_message.size = size;
	send_message.fdcount = 0;
	memcpy(send_message.data, json, size);
	//INFO("\n");
	if (write (gs->sock_fd, &send_message, HEADERSIZE + send_message.size) == -1){
		//INFO("Write error occured !!");
		//return -1;
		error("write");
	}
	memset (&recv_message, '\0', sizeof(struct message));
	size = sock_fd_read(gs->sock_fd, &recv_message, fd, fdcount);
	if (size <= 0)
		return -1;
	//INFO("\n");
	return 0;
}

int graphClientFinalise(socket_t *gs, char* json, int size){	
	struct message send_message, recv_message;
	memset (&send_message, '\0', sizeof(struct message));
	send_message.id = GRAPH_FINALISE;
	send_message.size = size;
	send_message.fdcount = 0;
	memcpy(send_message.data, json, size);
	if (write (gs->sock_fd, &send_message, HEADERSIZE + send_message.size) == -1)
		error ("write");
	memset (&recv_message, '\0', sizeof(struct message));
	size = read (gs->sock_fd, &recv_message, sizeof (struct message));
	if (size <= 0)
		return -1;
	return 0;
}


