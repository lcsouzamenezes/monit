/*
 * Copyright (C) Tildeslash Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU Affero General Public License in all respects
 * for all of the code used other than OpenSSL.
 */

#include "config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#include "net.h"
#include "ssl.h"
#include "monit.h"
#include "socket.h"

// libmonit
#include "exceptions/assert.h"
#include "util/Str.h"
#include "system/Net.h"



/**
 * Implementation of the socket interface.
 *
 * @file
 */


/* ------------------------------------------------------------- Definitions */


#define TYPE_LOCAL   0
#define TYPE_ACCEPT  1
// One TCP frame data size
#define RBUFFER_SIZE 1500

struct Socket_T {
        int port;
        int type;
        Socket_Family family;
        int socket;
        char *host;
        Port_T Port;
        int timeout; // milliseconds
        int connection_type;
        ssl_connection *ssl;
        ssl_server_connection *sslserver;
        int length;
        int offset;
        unsigned char buffer[RBUFFER_SIZE + 1];
} __attribute__((__packed__));


/* --------------------------------------------------------------- Private */


/*
 * Fill the internal buffer. If an error occurs or if the read
 * operation timed out -1 is returned.
 * @param S A Socket object
 * @param timeout The number of milliseconds to wait for data to be read
 * @return TRUE (the length of data read) or -1 if an error occured
 */
static int fill(Socket_T S, int timeout) {
        int n;
        S->offset = 0;
        S->length = 0;
        if (S->type == SOCK_DGRAM)
                timeout = 500;
        if (S->ssl) {
                n = recv_ssl_socket(S->ssl, S->buffer + S->length, RBUFFER_SIZE-S->length, timeout);
        } else {
                n = (int)sock_read(S->socket, S->buffer + S->length,  RBUFFER_SIZE-S->length, timeout);
        }
        if (n > 0) {
                S->length += n;
        }  else if (n < 0) {
                return -1;
        } else if (! (errno == EAGAIN || errno == EWOULDBLOCK)) // Peer closed connection
                return -1;
        return n;
}


/* ------------------------------------------------------------------ Public */


Socket_T socket_new(const char *host, int port, int type, Socket_Family family, int use_ssl, int timeout) {
        Ssl_T ssl = {.use_ssl = use_ssl, .version = SSL_VERSION_AUTO};
        return socket_create_t(host, port, type, family, ssl, timeout);
}


Socket_T socket_create(void *port) {
        ASSERT(port);
        Port_T p = port;
        int socket = -1;
        switch (p->family) {
                case Socket_Unix:
                        socket = create_unix_socket(p->pathname, p->type, p->timeout);
                        break;
                case Socket_Ip:
                case Socket_Ip4:
                case Socket_Ip6:
                        socket = create_socket(p->hostname, p->port, p->type, p->family, p->timeout);
                        break;
                default:
                        LogError("socket_create: Invalid socket family %d\n", p->family);
                        return NULL;
        }
        if (socket >= 0) {
                Socket_T S;
                NEW(S);
                S->socket = socket;
                S->type = p->type;
                S->family = p->family;
                S->port = p->port;
                S->timeout = p->timeout;
                S->connection_type = TYPE_LOCAL;
                S->host = Str_dup(p->family == Socket_Unix ? LOCALHOST : p->hostname);
                if (p->SSL.use_ssl && ! socket_switch2ssl(S, p->SSL)) {
                        socket_free(&S);
                        return NULL;
                }
                S->Port = port;
                return S;
        }
        LogError("socket_create: Could not create socket -- %s\n", STRERROR);
        return NULL;
}


Socket_T socket_create_t(const char *host, int port, int type, Socket_Family family, Ssl_T ssl, int timeout) {
        ASSERT(host);
        ASSERT(timeout > 0);
        if (ssl.use_ssl)
                ASSERT(type == SOCKET_TCP);
        else
                ASSERT(type == SOCKET_TCP || type == SOCKET_UDP);
        timeout = timeout * 1000; // Internally milliseconds is used
        int proto = type == SOCKET_UDP ? SOCK_DGRAM : SOCK_STREAM;
        int s = create_socket(host, port, proto, family, timeout);
        if (s != -1) {
                Socket_T S;
                NEW(S);
                S->socket = s;
                S->port = port;
                S->type = proto;
                S->family = family;
                S->timeout = timeout;
                S->host = Str_dup(host);
                S->connection_type = TYPE_LOCAL;
                if (ssl.use_ssl && ! socket_switch2ssl(S, ssl)) {
                        socket_free(&S);
                        return NULL;
                }
                return S;
        }
        return NULL;
}


Socket_T socket_create_a(int socket, const char *remote_host, int port, void *sslserver) {
        ASSERT(socket >= 0);
        ASSERT(remote_host);
        Socket_T S;
        NEW(S);
        S->port = port;
        S->socket = socket;
        S->type = SOCK_STREAM;
        S->family = Socket_Ip4; //FIXME: we use this with IPv4 HTTP engine currently => we don't need to identify the socket family at this point and hardcoded to IPv4 - change to support IPv6 when HTTP GUI will support it
        S->timeout = NET_TIMEOUT; // milliseconds
        S->host = Str_dup(remote_host);
        S->connection_type = TYPE_ACCEPT;
        if (sslserver) {
                S->sslserver = sslserver;
                if (! (S->ssl = insert_accepted_ssl_socket(S->sslserver)) || ! embed_accepted_ssl_socket(S->ssl, S->socket)) {
                        socket_free(&S);
                        return NULL;
                }
        }
        return S;
}


void socket_free(Socket_T *S) {
        ASSERT(S && *S);
#ifdef HAVE_OPENSSL
        if ((*S)->ssl && (*S)->ssl->handler)
        {
                if ((*S)->connection_type == TYPE_LOCAL) {
                        close_ssl_socket((*S)->ssl);
                        delete_ssl_socket((*S)->ssl);
                } else if ((*S)->connection_type == TYPE_ACCEPT && (*S)->sslserver) {
                        close_accepted_ssl_socket((*S)->sslserver, (*S)->ssl);
                }
        }
        else
#endif
        {
                Net_shutdown((*S)->socket, SHUT_RDWR);
                Net_close((*S)->socket);
        }
        FREE((*S)->host);
        FREE(*S);
}


/* ------------------------------------------------------------ Properties */


void socket_setTimeout(Socket_T S, int timeout) {
        ASSERT(S);
        S->timeout = timeout;
}


int socket_getTimeout(Socket_T S) {
        ASSERT(S);
        return S->timeout;
}


int socket_is_ready(Socket_T S) {
        ASSERT(S);
        switch (S->type) {
                case SOCK_STREAM:
                        return check_socket(S->socket);
                case SOCK_DGRAM:
                        return check_udp_socket(S->socket);
                default:
                        break;
        }
        return FALSE;
}


int socket_is_secure(Socket_T S) {
        ASSERT(S);
        return (S->ssl != NULL);
}


int socket_is_udp(Socket_T S) {
        ASSERT(S);
        return (S->type == SOCK_DGRAM);
}


int socket_get_socket(Socket_T S) {
        ASSERT(S);
        return S->socket;
}


int socket_get_type(Socket_T S) {
        ASSERT(S);
        return S->type;
}


void *socket_get_Port(Socket_T S) {
        ASSERT(S);
        return S->Port;
}


int socket_get_remote_port(Socket_T S) {
        ASSERT(S);
        return S->port;
}


const char *socket_get_remote_host(Socket_T S) {
        ASSERT(S);
        return S->host;
}


int socket_get_local_port(Socket_T S) {
        ASSERT(S);
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        if (getsockname(S->socket, (struct sockaddr *)&addr, &addrlen) == 0) {
                if (addr.ss_family == AF_INET)
                        return ntohs(((struct sockaddr_in *)&addr)->sin_port);
#ifdef IPV6
                else if (addr.ss_family == AF_INET6)
                        return ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
#endif
        }
        return -1;

}


const char *socket_get_local_host(Socket_T S, char *host, int hostlen) {
        ASSERT(S);
        ASSERT(host);
        ASSERT(hostlen);
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        if (! getsockname(S->socket, (struct sockaddr *)&addr, &addrlen)) {
                int status = getnameinfo((struct sockaddr *)&addr, addrlen, host, hostlen, NULL, 0, 0);
                if (status) {
                        LogError("Cannot translate address to hostname -- %s\n", status == EAI_SYSTEM ? STRERROR : gai_strerror(status));
                        *host = 0;
                }
        }
        return NULL;
}


void socket_setError(Socket_T S, const char *error, ...) {
        assert(S);
        assert(error);
        va_list ap;
        va_start(ap, error);
        vsnprintf((char*)S->buffer, RBUFFER_SIZE, error, ap);
        va_end(ap);
}


const char *socket_getError(Socket_T S) {
        assert(S);
        return (const char *)S->buffer;
}


/* ---------------------------------------------------------------- Public */


int socket_switch2ssl(Socket_T S, Ssl_T ssl)  {
        assert(S);
        if (! (S->ssl = new_ssl_connection(ssl.clientpemfile, ssl.version)))
                return FALSE;
        if (! embed_ssl_socket(S->ssl, S->socket))
                return FALSE;
        if (ssl.certmd5 && ! check_ssl_md5sum(S->ssl, ssl.certmd5)) {
                LogError("md5sum of certificate does not match!\n");
                return FALSE;
        }
        return TRUE;
}


int socket_print(Socket_T S, const char *m, ...) {
        int n;
        va_list ap;
        char *buf = NULL;
        ASSERT(S);
        ASSERT(m);
        va_start(ap, m);
        buf = Str_vcat(m, ap);
        va_end(ap);
        n = socket_write(S, buf, strlen(buf));
        FREE(buf);
        return n;
}


int socket_write(Socket_T S, void *b, size_t size) {
        ssize_t n = 0;
        void *p = b;
        ASSERT(S);
        while (size > 0) {
                if (S->ssl) {
                        n = send_ssl_socket(S->ssl, p, size, S->timeout);
                } else {
                        if (S->type == SOCK_DGRAM)
                                n = udp_write(S->socket,  p, size, S->timeout);
                        else
                                n = sock_write(S->socket,  p, size, S->timeout);
                }
                if (n <= 0) break;
                p += n;
                size -= n;

        }
        if (n < 0) {
                /* No write or a partial write is an error */
                return -1;
        }
        return  (int)(p - b);
}


int socket_read_byte(Socket_T S) {
        ASSERT(S);
        if (S->offset >= S->length)
                if (fill(S, S->timeout) <= 0)
                        return -1;
        return S->buffer[S->offset++];
}


int socket_read(Socket_T S, void *b, int size) {
        int c;
        unsigned char *p = b;
        ASSERT(S);
        while ((size-- > 0) && ((c = socket_read_byte(S)) >= 0))
                *p++ = c;
        return (int)((long)p - (long)b);
}


char *socket_readln(Socket_T S, char *s, int size) {
        int c;
        unsigned char *p = (unsigned char *)s;
        ASSERT(S);
        while (--size && ((c = socket_read_byte(S)) > 0)) { // Stop when \0 is read
                *p++ = c;
                if (c == '\n')
                        break;
        }
        *p = 0;
        if (*s)
                return s;
        return NULL;
}


void socket_reset(Socket_T S) {
        ASSERT(S);
        /* Throw away any pending incomming data */
        while (fill(S, 0) > 0)
                ;
        S->offset = 0;
        S->length = 0;
}


int socket_shutdown_write(Socket_T S) {
        ASSERT(S);
        return (shutdown(S->socket, 1) == 0);
}


int socket_set_tcp_nodelay(Socket_T S) {
        int on = 1;
        return (setsockopt(S->socket, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == 0);
}

