/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "../core/compat.h"

#define R360_XNCALLER_SYSAPP 2

#define R360_AF_INET 2
#define R360_SOCK_DGRAM 2
#define R360_IPPROTO_UDP 17
#define R360_SOL_SOCKET 0xffff
#define R360_SO_REUSEADDR 0x0004
#define R360_SO_INSECURE 0x5801
#define R360_FIONBIO 0x8004667eUL
#define R360_INVALID_SOCKET ((u32)~0u)
#define R360_SOCKET_ERROR (-1)
#define R360_WSAEWOULDBLOCK 10035
#define R360_WSAENOBUFS 10055

#define ORD_NetDll_socket 0x003
#define ORD_NetDll_closesocket 0x004
#define ORD_NetDll_ioctlsocket 0x006
#define ORD_NetDll_setsockopt 0x007
#define ORD_NetDll_bind 0x00b
#define ORD_NetDll_recvfrom 0x014
#define ORD_NetDll_sendto 0x018
#define ORD_NetDll_WSAGetLastError 0x01b

struct r360_in_addr {
	u32 s_addr;
};

struct r360_sockaddr {
	u16 sa_family;
	char sa_data[14];
};

struct r360_sockaddr_in {
	u16 sin_family;
	u16 sin_port;
	struct r360_in_addr sin_addr;
	char sin_zero[8];
};

typedef u32 (*r360_socket_t)(u32 caller, int family, int type, int protocol);
typedef int (*r360_closesocket_t)(u32 caller, u32 socket);
typedef int (*r360_ioctlsocket_t)(u32 caller, u32 socket, u32 command, u32 *argument);
typedef int (*r360_setsockopt_t)(u32 caller, u32 socket, int level, int option,
				 const char *value, int value_length);
typedef int (*r360_bind_t)(u32 caller, u32 socket, const struct r360_sockaddr *name,
			   int name_length);
typedef int (*r360_recvfrom_t)(u32 caller, u32 socket, char *buffer, int length, int flags,
			       struct r360_sockaddr *from, int *from_length);
typedef int (*r360_sendto_t)(u32 caller, u32 socket, const char *buffer, int length, int flags,
			     const struct r360_sockaddr *to, int to_length);
typedef int (*r360_wsa_last_error_t)(u32 caller);

struct r360_net_api {
	r360_socket_t socket;
	r360_closesocket_t closesocket;
	r360_ioctlsocket_t ioctlsocket;
	r360_setsockopt_t setsockopt;
	r360_bind_t bind;
	r360_recvfrom_t recvfrom;
	r360_sendto_t sendto;
	r360_wsa_last_error_t last_error;
};

