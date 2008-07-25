/* 
   HTTP Request Handling
   Copyright (C) 1999-2008, Joe Orton <joe@manyfish.co.uk>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.
   
   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA

*/

/* THIS IS NOT A PUBLIC INTERFACE. You CANNOT include this header file
 * from an application.  */
 
#ifndef NE_PRIVATE_H
#define NE_PRIVATE_H

#include "ne_request.h"
#include "ne_socket.h"
#include "ne_ssl.h"

struct host_info {
    char *hostname;
    unsigned int port;
    ne_sock_addr *address; /* if non-NULL, result of resolving 'hostname'. */
    /* current network address obtained from 'address' being used. */
    const ne_inet_addr *current;
    char *hostport; /* URI hostport segment */
};

/* Store every registered callback in a generic container, and cast
 * the function pointer when calling it.  */
struct hook {
    void (*fn)(void);
    void *userdata;
    const char *id; /* non-NULL for accessors. */
    struct hook *next;
};

#define HAVE_HOOK(st,func) (st->hook->hooks->func != NULL)
#define HOOK_FUNC(st, func) (*st->hook->hooks->func)

/* Session support. */
struct ne_session_s {
    /* Connection information */
    ne_socket *socket;

    /* non-zero if connection has been established. */
    int connected;
    
    /* non-zero if connection has persisted beyond one request. */
    int persisted;

    int is_http11; /* >0 if connected server is known to be
		    * HTTP/1.1 compliant. */

    char *scheme;
    struct host_info server, proxy;

    /* application-provided address list */
    const ne_inet_addr **addrlist;
    size_t numaddrs, curaddr;

    /* Local address to which sockets should be bound. */
    const ne_inet_addr *local_addr;

    /* Settings */
    int use_proxy; /* do we have a proxy server? */
    int use_ssl; /* whether a secure connection is required */
    int in_connect; /* doing a proxy CONNECT */

    int flags[NE_SESSFLAG_LAST];

    ne_progress progress_cb;
    void *progress_ud;

    ne_notify_status notify_cb;
    void *notify_ud;

    int rdtimeout, cotimeout; /* read, connect timeouts. */

    struct hook *create_req_hooks, *pre_send_hooks, *post_send_hooks,
        *post_headers_hooks, *destroy_req_hooks, *destroy_sess_hooks, 
        *close_conn_hooks, *private;

    char *user_agent; /* full User-Agent: header field */

#ifdef NE_HAVE_SSL
    ne_ssl_client_cert *client_cert;
    ne_ssl_certificate *server_cert;
    ne_ssl_context *ssl_context;
    int ssl_cc_requested; /* set to non-zero if a client cert was
                           * requested during initial handshake, but
                           * none could be provided. */
#endif

    /* Server cert verification callback: */
    ne_ssl_verify_fn ssl_verify_fn;
    void *ssl_verify_ud;
    /* Client cert provider callback: */
    ne_ssl_provide_fn ssl_provide_fn;
    void *ssl_provide_ud;

    ne_session_status_info status;

    /* Error string */
    char error[512];
};

/* Pushes block of 'count' bytes at 'buf'. Returns non-zero on
 * error. */
typedef int (*ne_push_fn)(void *userdata, const char *buf, size_t count);

/* Do the SSL negotiation. */
int ne__negotiate_ssl(ne_session *sess);

/* Set the session error appropriate for SSL verification failures. */
void ne__ssl_set_verify_err(ne_session *sess, int failures);

/* Return non-zero if hostname from certificate (cn) matches hostname
 * used for session (hostname); follows RFC2818 logic.  cn is modified
 * in-place. */
int ne__ssl_match_hostname(char *cn, const char *hostname);

#endif /* HTTP_PRIVATE_H */
