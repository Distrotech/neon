/* 
   HTTP Authentication routines
   Copyright (C) 1999-2003, Joe Orton <joe@manyfish.co.uk>

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


/* HTTP Authentication, as per RFC2617.
 * 
 * TODO:
 *  - Test auth-int support
 */

#include "config.h"

#include <sys/types.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h> /* for getpid() */
#endif

#ifdef WIN32
#include <windows.h> /* for GetCurrentThreadId() etc */
#endif

#ifdef NEON_SSL
#include <openssl/rand.h>
#endif

#include <time.h>

#include "ne_md5.h"
#include "ne_dates.h"
#include "ne_request.h"
#include "ne_auth.h"
#include "ne_string.h"
#include "ne_utils.h"
#include "ne_alloc.h"
#include "ne_uri.h"
#include "ne_i18n.h"

#ifdef HAVE_GSSAPI
#ifdef HAVE_GSSAPI_GSSAPI_H /* MIT */
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_generic.h>
#else /* Heimdal. */
#include <gssapi.h>
#endif
#endif

/* TODO: should remove this eventually. Need it for
 * ne_pull_request_body. */
#include "ne_private.h"

#define HOOK_SERVER_ID "http://webdav.org/neon/hooks/server-auth"
#define HOOK_PROXY_ID "http://webdav.org/neon/hooks/proxy-auth"

/* The authentication scheme we are using */
typedef enum {
    auth_scheme_basic,
    auth_scheme_digest,
    auth_scheme_gssapi
} auth_scheme;

typedef enum { 
    auth_alg_md5,
    auth_alg_md5_sess,
    auth_alg_unknown
} auth_algorithm;

/* Selected method of qop which the client is using */
typedef enum {
    auth_qop_none,
    auth_qop_auth,
    auth_qop_auth_int
} auth_qop;

/* A challenge */
struct auth_challenge {
    auth_scheme scheme;
    const char *realm;
    const char *nonce;
    const char *opaque;
    unsigned int stale:1; /* if stale=true */
    unsigned int got_qop:1; /* we were given a qop directive */
    unsigned int qop_auth:1; /* "auth" token in qop attrib */
    unsigned int qop_auth_int:1; /* "auth-int" token in qop attrib */
    auth_algorithm alg;
    struct auth_challenge *next;
};

static const struct auth_class {
    const char *id, *req_hdr, *resp_hdr, *resp_info_hdr, *fail_msg;
    int status_code, fail_code;
} ah_server_class = {
    HOOK_SERVER_ID,
    "Authorization", "WWW-Authenticate", "Authentication-Info",
    N_("Server was not authenticated correctly."), 401, NE_AUTH
}, ah_proxy_class = {
    HOOK_PROXY_ID,
    "Proxy-Authorization", "Proxy-Authenticate", "Proxy-Authentication-Info",
    N_("Proxy server was not authenticated correctly."), 407, NE_PROXYAUTH 
};

/* Authentication session state. */
typedef struct {
    ne_session *sess;

    /* Which context will auth challenges be accepted? */
    enum {
        AUTH_ANY, /* ignore nothing. */
        AUTH_CONNECT, /* only in response to a CONNECT request. */
        AUTH_NOTCONNECT /* only in non-CONNECT responsees */
    } context;
    
    /* Specifics for server/proxy auth. FIXME: need a better field
     * name! */
    const struct auth_class *spec;
    
    /* The scheme used for this authentication session */
    auth_scheme scheme;
    /* The callback used to request new username+password */
    ne_auth_creds creds;
    void *userdata;

    /*** Session details ***/

    /* The username and password we are using to authenticate with */
    char username[NE_ABUFSIZ];
    /* Whether we CAN supply authentication at the moment */
    unsigned int can_handle:1;
    /* This used for Basic auth */
    char *basic; 
#ifdef HAVE_GSSAPI
    /* This used for GSSAPI auth */
    char *gssapi_token;
#endif
    /* These all used for Digest auth */
    char *realm;
    char *nonce;
    char *cnonce;
    char *opaque;
    auth_qop qop;
    auth_algorithm alg;
    unsigned int nonce_count;
    /* The ASCII representation of the session's H(A1) value */
    char h_a1[33];

    /* Temporary store for half of the Request-Digest
     * (an optimisation - used in the response-digest calculation) */
    struct ne_md5_ctx stored_rdig;

    /* Details of server... needed to reconstruct absoluteURI's when
     * necessary */
    const char *host;
    const char *uri_scheme;
    unsigned int port;

    int attempt;
} auth_session;

struct auth_request {
    /*** Per-request details. ***/
    ne_request *request; /* the request object. */

    /* The method and URI we are using for the current request */
    const char *uri;
    const char *method;
    /* Whether we WILL supply authentication for this request or not */
    unsigned int will_handle:1;

    /* Used for calculation of H(entity-body) of the response */
    struct ne_md5_ctx response_body;

    /* Results of response-header callbacks */
    char *auth_hdr, *auth_info_hdr;
};

static void clean_session(auth_session *sess) 
{
    sess->can_handle = 0;
    NE_FREE(sess->basic);
    NE_FREE(sess->nonce);
    NE_FREE(sess->cnonce);
    NE_FREE(sess->opaque);
    NE_FREE(sess->realm);
#ifdef HAVE_GSSAPI
    NE_FREE(sess->gssapi_token);
#endif
}

/* Returns client nonce string. */
static char *get_cnonce(void) 
{
    char ret[33];
    unsigned char data[256], tmp[16];
    struct ne_md5_ctx hash;

    ne_md5_init_ctx(&hash);

#ifdef NEON_SSL
    if (RAND_pseudo_bytes(data, sizeof data) >= 0)
	ne_md5_process_bytes(data, sizeof data, &hash);
    else {
#endif
    /* Fallback sources of random data: all bad, but no good sources
     * are available. */

    /* Uninitialized stack data; yes, happy valgrinders, this is
     * supposed to be here. */
    ne_md5_process_bytes(data, sizeof data, &hash);
    
#ifdef HAVE_GETTIMEOFDAY
    {
	struct timeval tv;
	if (gettimeofday(&tv, NULL) == 0)
	    ne_md5_process_bytes(&tv, sizeof tv, &hash);
    }
#else /* HAVE_GETTIMEOFDAY */
    {
	time_t t = time(NULL);
	ne_md5_process_bytes(&t, sizeof t, &hash);
    }
#endif
    {
#ifdef WIN32
	DWORD pid = GetCurrentThreadId();
#else
	pid_t pid = getpid();
#endif
	ne_md5_process_bytes(&pid, sizeof pid, &hash);
    }

#ifdef NEON_SSL
    }
#endif
    
    ne_md5_finish_ctx(&hash, tmp);
    ne_md5_to_ascii(tmp, ret);

    return ne_strdup(ret);
}

static int get_credentials(auth_session *sess, char *pwbuf) 
{
    return sess->creds(sess->userdata, sess->realm, sess->attempt++,
		       sess->username, pwbuf);
}

/* Examine a Basic auth challenge.
 * Returns 0 if an valid challenge, else non-zero. */
static int basic_challenge(auth_session *sess, struct auth_challenge *parms) 
{
    char *tmp, password[NE_ABUFSIZ];

    /* Verify challenge... must have a realm */
    if (parms->realm == NULL) {
	return -1;
    }

    NE_DEBUG(NE_DBG_HTTPAUTH, "Got Basic challenge with realm [%s]\n", 
	     parms->realm);

    clean_session(sess);
    
    sess->realm = ne_strdup(parms->realm);

    if (get_credentials(sess, password)) {
	/* Failed to get credentials */
	return -1;
    }

    sess->scheme = auth_scheme_basic;

    tmp = ne_concat(sess->username, ":", password, NULL);
    sess->basic = ne_base64(tmp, strlen(tmp));
    ne_free(tmp);

    /* Paranoia. */
    memset(password, 0, sizeof password);

    return 0;
}

/* Add Basic authentication credentials to a request */
static char *request_basic(auth_session *sess) 
{
    return ne_concat("Basic ", sess->basic, "\r\n", NULL);
}

#ifdef HAVE_GSSAPI
/* Add GSSAPI authentication credentials to a request */
static char *request_gssapi(auth_session *sess) 
{
    return ne_concat("GSS-Negotiate ", sess->gssapi_token, "\r\n", NULL);
}

static int get_gss_name(gss_name_t *server, auth_session *sess)
{
    unsigned int major_status, minor_status;
    gss_buffer_desc token = GSS_C_EMPTY_BUFFER;

    token.value = ne_concat("khttp@", sess->sess->server.hostname, NULL);
    token.length = strlen(token.value);

    major_status = gss_import_name(&minor_status, &token,
                                   GSS_C_NT_HOSTBASED_SERVICE,
                                   server);
    return GSS_ERROR(major_status) ? -1 : 0;
}

/* Examine a GSSAPI auth challenge; returns 0 if a valid challenge,
 * else non-zero. */
static int 
gssapi_challenge(auth_session *sess, struct auth_challenge *parms) 
{
    gss_ctx_id_t context;
    gss_name_t server_name;
    unsigned int major_status, minor_status;
    gss_buffer_desc input_token = GSS_C_EMPTY_BUFFER;
    gss_buffer_desc output_token = GSS_C_EMPTY_BUFFER;

    clean_session(sess);

    if (get_gss_name(&server_name, sess))
        return -1;

    major_status = gss_init_sec_context(&minor_status,
                                        GSS_C_NO_CREDENTIAL,
                                        &context,
                                        server_name,
                                        GSS_C_NO_OID,
                                        GSS_C_DELEG_FLAG,
                                        0,
                                        GSS_C_NO_CHANNEL_BINDINGS,
                                        &input_token,
                                        NULL,
                                        &output_token,
                                        NULL,
                                        NULL);
    if (GSS_ERROR(major_status)) {
        NE_DEBUG(NE_DBG_HTTPAUTH, "gss_init_sec_context failed.\n");
        return -1;
    }
    
    if (output_token.length == 0)
        return -1;

    sess->gssapi_token = ne_base64(output_token.value, output_token.length);

    NE_DEBUG(NE_DBG_HTTPAUTH, 
             "Base64 encoded GSSAPI challenge: %s.\n", sess->gssapi_token);
    sess->scheme = auth_scheme_gssapi;
    return 0;
}
#endif

/* Examine a digest challenge: return 0 if it is a valid Digest challenge,
 * else non-zero. */
static int digest_challenge(auth_session *sess, struct auth_challenge *parms) 
{
    struct ne_md5_ctx tmp;
    unsigned char tmp_md5[16];
    char password[NE_ABUFSIZ];

    /* Verify they've given us the right bits. */
    if (parms->alg == auth_alg_unknown ||
	((parms->alg == auth_alg_md5_sess) &&
	 !(parms->qop_auth || parms->qop_auth_int)) ||
	parms->realm==NULL || parms->nonce==NULL) {
	NE_DEBUG(NE_DBG_HTTPAUTH, "Invalid challenge.");
	return -1;
    }

    if (parms->stale) {
	/* Just a stale response, don't need to get a new username/password */
	NE_DEBUG(NE_DBG_HTTPAUTH, "Stale digest challenge.\n");
    } else {
	/* Forget the old session details */
	NE_DEBUG(NE_DBG_HTTPAUTH, "In digest challenge.\n");

	clean_session(sess);

	sess->realm = ne_strdup(parms->realm);

	/* Not a stale response: really need user authentication */
	if (get_credentials(sess, password)) {
	    /* Failed to get credentials */
	    return -1;
	}
    }
    sess->alg = parms->alg;
    sess->scheme = auth_scheme_digest;
    sess->nonce = ne_strdup(parms->nonce);
    sess->cnonce = get_cnonce();
    /* TODO: add domain handling. */
    if (parms->opaque != NULL) {
	sess->opaque = ne_strdup(parms->opaque); /* don't strip the quotes */
    }
    
    if (parms->got_qop) {
	/* What type of qop are we to apply to the message? */
	NE_DEBUG(NE_DBG_HTTPAUTH, "Got qop directive.\n");
	sess->nonce_count = 0;
	if (parms->qop_auth_int) {
	    sess->qop = auth_qop_auth_int;
	} else {
	    sess->qop = auth_qop_auth;
	}
    } else {
	/* No qop at all/ */
	sess->qop = auth_qop_none;
    }
    
    if (!parms->stale) {
	/* Calculate H(A1).
	 * tmp = H(unq(username-value) ":" unq(realm-value) ":" passwd)
	 */
	NE_DEBUG(NE_DBG_HTTPAUTH, "Calculating H(A1).\n");
	ne_md5_init_ctx(&tmp);
	ne_md5_process_bytes(sess->username, strlen(sess->username), &tmp);
	ne_md5_process_bytes(":", 1, &tmp);
	ne_md5_process_bytes(sess->realm, strlen(sess->realm), &tmp);
	ne_md5_process_bytes(":", 1, &tmp);
	ne_md5_process_bytes(password, strlen(password), &tmp);
	memset(password, 0, sizeof password); /* done with that. */
	ne_md5_finish_ctx(&tmp, tmp_md5);
	if (sess->alg == auth_alg_md5_sess) {
	    unsigned char a1_md5[16];
	    struct ne_md5_ctx a1;
	    char tmp_md5_ascii[33];
	    /* Now we calculate the SESSION H(A1)
	     *    A1 = H(...above...) ":" unq(nonce-value) ":" unq(cnonce-value) 
	     */
	    ne_md5_to_ascii(tmp_md5, tmp_md5_ascii);
	    ne_md5_init_ctx(&a1);
	    ne_md5_process_bytes(tmp_md5_ascii, 32, &a1);
	    ne_md5_process_bytes(":", 1, &a1);
	    ne_md5_process_bytes(sess->nonce, strlen(sess->nonce), &a1);
	    ne_md5_process_bytes(":", 1, &a1);
	    ne_md5_process_bytes(sess->cnonce, strlen(sess->cnonce), &a1);
	    ne_md5_finish_ctx(&a1, a1_md5);
	    ne_md5_to_ascii(a1_md5, sess->h_a1);
	    NE_DEBUG(NE_DBG_HTTPAUTH, "Session H(A1) is [%s]\n", sess->h_a1);
	} else {
	    ne_md5_to_ascii(tmp_md5, sess->h_a1);
	    NE_DEBUG(NE_DBG_HTTPAUTH, "H(A1) is [%s]\n", sess->h_a1);
	}
	
    }
    
    NE_DEBUG(NE_DBG_HTTPAUTH, "I like this Digest challenge.\n");

    return 0;
}

/* callback for ne_pull_request_body. */
static int digest_body(void *userdata, const char *buf, size_t count)
{
    struct ne_md5_ctx *ctx = userdata;
    ne_md5_process_bytes(buf, count, ctx);
    return 0;
}

/* Return Digest authentication credentials header value for the given
 * session. */
static char *request_digest(auth_session *sess, struct auth_request *req) 
{
    struct ne_md5_ctx a2, rdig;
    unsigned char a2_md5[16], rdig_md5[16];
    char a2_md5_ascii[33], rdig_md5_ascii[33];
    char nc_value[9] = {0};
    const char *qop_value; /* qop-value */
    ne_buffer *ret;

    /* Increase the nonce-count */
    if (sess->qop != auth_qop_none) {
	sess->nonce_count++;
	ne_snprintf(nc_value, 9, "%08x", sess->nonce_count);
	NE_DEBUG(NE_DBG_HTTPAUTH, "Nonce count is %u, nc is [%s]\n", 
		 sess->nonce_count, nc_value);
    }

    qop_value = sess->qop == auth_qop_auth_int ? "auth-int" : "auth";

    /* Calculate H(A2). */
    ne_md5_init_ctx(&a2);
    ne_md5_process_bytes(req->method, strlen(req->method), &a2);
    ne_md5_process_bytes(":", 1, &a2);
    ne_md5_process_bytes(req->uri, strlen(req->uri), &a2);
    
    if (sess->qop == auth_qop_auth_int) {
	struct ne_md5_ctx body;
	char tmp_md5_ascii[33];
	unsigned char tmp_md5[16];
	
	ne_md5_init_ctx(&body);

	/* Calculate H(entity-body): pull in the request body from
	 * where-ever it is coming from, and calculate the digest. */
	
	NE_DEBUG(NE_DBG_HTTPAUTH, "Digesting request body...\n");
	ne_pull_request_body(req->request, digest_body, &body);
	NE_DEBUG(NE_DBG_HTTPAUTH, "Digesting request body done.\n");
		
	ne_md5_finish_ctx(&body, tmp_md5);
	ne_md5_to_ascii(tmp_md5, tmp_md5_ascii);

	NE_DEBUG(NE_DBG_HTTPAUTH, "H(entity-body) is [%s]\n", tmp_md5_ascii);
	
	/* Append to A2 */
	ne_md5_process_bytes(":", 1, &a2);
	ne_md5_process_bytes(tmp_md5_ascii, 32, &a2);
    }
    ne_md5_finish_ctx(&a2, a2_md5);
    ne_md5_to_ascii(a2_md5, a2_md5_ascii);
    NE_DEBUG(NE_DBG_HTTPAUTH, "H(A2): %s\n", a2_md5_ascii);

    NE_DEBUG(NE_DBG_HTTPAUTH, "Calculating Request-Digest.\n");
    /* Now, calculation of the Request-Digest.
     * The first section is the regardless of qop value
     *     H(A1) ":" unq(nonce-value) ":" */
    ne_md5_init_ctx(&rdig);

    /* Use the calculated H(A1) */
    ne_md5_process_bytes(sess->h_a1, 32, &rdig);

    ne_md5_process_bytes(":", 1, &rdig);
    ne_md5_process_bytes(sess->nonce, strlen(sess->nonce), &rdig);
    ne_md5_process_bytes(":", 1, &rdig);
    if (sess->qop != auth_qop_none) {
	/* Add on:
	 *    nc-value ":" unq(cnonce-value) ":" unq(qop-value) ":"
	 */
	NE_DEBUG(NE_DBG_HTTPAUTH, "Have qop directive, digesting: [%s:%s:%s]\n",
		 nc_value, sess->cnonce, qop_value);
	ne_md5_process_bytes(nc_value, 8, &rdig);
	ne_md5_process_bytes(":", 1, &rdig);
	ne_md5_process_bytes(sess->cnonce, strlen(sess->cnonce), &rdig);
	ne_md5_process_bytes(":", 1, &rdig);
	/* Store a copy of this structure (see note below) */
	sess->stored_rdig = rdig;
	ne_md5_process_bytes(qop_value, strlen(qop_value), &rdig);
	ne_md5_process_bytes(":", 1, &rdig);
    } else {
	/* Store a copy of this structure... we do this because the
	 * calculation of the rspauth= field in the Auth-Info header 
	 * is the same as this digest, up to this point. */
	sess->stored_rdig = rdig;
    }
    /* And finally, H(A2) */
    ne_md5_process_bytes(a2_md5_ascii, 32, &rdig);
    ne_md5_finish_ctx(&rdig, rdig_md5);
    ne_md5_to_ascii(rdig_md5, rdig_md5_ascii);
    
    ret = ne_buffer_create();

    ne_buffer_concat(ret, 
		     "Digest username=\"", sess->username, "\", "
		     "realm=\"", sess->realm, "\", "
		     "nonce=\"", sess->nonce, "\", "
		     "uri=\"", req->uri, "\", "
		     "response=\"", rdig_md5_ascii, "\", "
		     "algorithm=\"", sess->alg == auth_alg_md5 ? "MD5" : "MD5-sess", "\"", 
		     NULL);
    
    if (sess->opaque != NULL) {
	ne_buffer_concat(ret, ", opaque=\"", sess->opaque, "\"", NULL);
    }

    if (sess->qop != auth_qop_none) {
	/* Add in cnonce and nc-value fields */
	ne_buffer_concat(ret, ", cnonce=\"", sess->cnonce, "\", "
			 "nc=", nc_value, ", "
			 "qop=\"", qop_value, "\"", NULL);
    }

    ne_buffer_zappend(ret, "\r\n");
    
    NE_DEBUG(NE_DBG_HTTPAUTH, "Digest request header is %s\n", ret->data);

    return ne_buffer_finish(ret);
}

/* Parse line of comma-separated key-value pairs.  If 'ischall' == 1,
 * then also return a leading space-separated token, as *value == NULL.
 * Otherwise, if return value is 0, *key and *value will be non-NULL.
 * If return value is non-zero, parsing has ended. */
static int tokenize(char **hdr, char **key, char **value, int ischall)
{
    char *pnt = *hdr;
    enum { BEFORE_EQ, AFTER_EQ, AFTER_EQ_QUOTED } state = BEFORE_EQ;
    
    if (**hdr == '\0')
	return 1;

    *key = NULL;

    do {
	switch (state) {
	case BEFORE_EQ:
	    if (*pnt == '=') {
		if (*key == NULL)
		    return -1;
		*pnt = '\0';
		*value = pnt + 1;
		state = AFTER_EQ;
	    } else if (*pnt == ' ' && ischall && *key != NULL) {
		*value = NULL;
		*pnt = '\0';
		*hdr = pnt + 1;
		return 0;
	    } else if (*key == NULL && strchr(" \r\n\t", *pnt) == NULL) {
		*key = pnt;
	    }
	    break;
	case AFTER_EQ:
	    if (*pnt == ',') {
		*pnt = '\0';
		*hdr = pnt + 1;
		return 0;
	    } else if (*pnt == '\"') {
		state = AFTER_EQ_QUOTED;
	    }
	    break;
	case AFTER_EQ_QUOTED:
	    if (*pnt == '\"') {
		state = AFTER_EQ;
	    }
	    break;
	}
    } while (*++pnt != '\0');
    
    if (state == BEFORE_EQ && ischall && *key != NULL) {
	*value = NULL;
    }

    *hdr = pnt;

    /* End of string: */
    return 0;
}

/* Pass this the value of the 'Authentication-Info:' header field, if
 * one is received.
 * Returns:
 *    0 if it gives a valid authentication for the server 
 *    non-zero otherwise (don't believe the response in this case!).
 */
static int verify_response(struct auth_request *req, auth_session *sess,
			   const char *value) 
{
    char *hdr, *pnt, *key, *val;
    auth_qop qop = auth_qop_none;
    char *nextnonce = NULL, /* for the nextnonce= value */
	*rspauth = NULL, /* for the rspauth= value */
	*cnonce = NULL, /* for the cnonce= value */
	*nc = NULL, /* for the nc= value */
	*qop_value = NULL;
    unsigned int nonce_count;
    int okay;
    
    if (!req->will_handle) {
	/* Ignore it */
	return 0;
    }
    
    if (sess->scheme != auth_scheme_digest) {
	NE_DEBUG(NE_DBG_HTTPAUTH, "Found Auth-Info header not in response "
		 " to Digest credentials - dodgy.\n");
	return -1;
    }

    pnt = hdr = ne_strdup(value);
    
    NE_DEBUG(NE_DBG_HTTPAUTH, "Auth-Info header: %s\n", value);

    while (tokenize(&pnt, &key, &val, 0) == 0) {
	val = ne_shave(val, "\"");
	NE_DEBUG(NE_DBG_HTTPAUTH, "Pair: [%s] = [%s]\n", key, val);
	if (strcasecmp(key, "qop") == 0) {
	    qop_value = val;
	    if (strcasecmp(val, "auth-int") == 0) {
		qop = auth_qop_auth_int;
	    } else if (strcasecmp(val, "auth") == 0) {
		qop = auth_qop_auth;
	    } else {
		qop = auth_qop_none;
	    }
	} else if (strcasecmp(key, "nextnonce") == 0) {
	    nextnonce = val;
	} else if (strcasecmp(key, "rspauth") == 0) {
	    rspauth = val;
	} else if (strcasecmp(key, "cnonce") == 0) {
	    cnonce = val;
	} else if (strcasecmp(key, "nc") == 0) { 
	    nc = val;
	    if (sscanf(val, "%x", &nonce_count) != 1) {
		NE_DEBUG(NE_DBG_HTTPAUTH, "Couldn't find nonce count.\n");
	    } else {
		NE_DEBUG(NE_DBG_HTTPAUTH, "Got nonce_count: %u\n", nonce_count);
	    }
	}
    }

    /* Presume the worst */
    okay = -1;

    if ((qop != auth_qop_none) && (qop_value != NULL)) {
	if ((rspauth == NULL) || (cnonce == NULL) || (nc == NULL)) {
	    NE_DEBUG(NE_DBG_HTTPAUTH, "Missing rspauth, cnonce or nc with qop.\n");
	} else { /* Have got rspauth, cnonce and nc */
	    if (strcmp(cnonce, sess->cnonce) != 0) {
		NE_DEBUG(NE_DBG_HTTPAUTH, "Response cnonce doesn't match.\n");
	    } else if (nonce_count != sess->nonce_count) { 
		NE_DEBUG(NE_DBG_HTTPAUTH, "Response nonce count doesn't match.\n");
	    } else {
		/* Calculate and check the response-digest value.
		 * joe: IMO the spec is slightly ambiguous as to whether
		 * we use the qop which WE sent, or the qop which THEY
		 * sent...  */
		struct ne_md5_ctx a2;
		unsigned char a2_md5[16], rdig_md5[16];
		char a2_md5_ascii[33], rdig_md5_ascii[33];

		NE_DEBUG(NE_DBG_HTTPAUTH, "Calculating response-digest.\n");

		/* First off, H(A2) again. */
		ne_md5_init_ctx(&a2);
		ne_md5_process_bytes(":", 1, &a2);
		ne_md5_process_bytes(req->uri, strlen(req->uri), &a2);
		if (qop == auth_qop_auth_int) {
		    unsigned char heb_md5[16];
		    char heb_md5_ascii[33];
		    /* Add on ":" H(entity-body) */
		    ne_md5_finish_ctx(&req->response_body, heb_md5);
		    ne_md5_to_ascii(heb_md5, heb_md5_ascii);
		    ne_md5_process_bytes(":", 1, &a2);
		    ne_md5_process_bytes(heb_md5_ascii, 32, &a2);
		    NE_DEBUG(NE_DBG_HTTPAUTH, "Digested [:%s]\n", heb_md5_ascii);
		}
		ne_md5_finish_ctx(&a2, a2_md5);
		ne_md5_to_ascii(a2_md5, a2_md5_ascii);
		
		/* We have the stored digest-so-far of 
		 *   H(A1) ":" unq(nonce-value) 
		 *        [ ":" nc-value ":" unq(cnonce-value) ] for qop
		 * in sess->stored_rdig, to save digesting them again.
		 *
		 */
		if (qop != auth_qop_none) {
		    /* Add in qop-value */
		    NE_DEBUG(NE_DBG_HTTPAUTH, "Digesting qop-value [%s:].\n", 
			     qop_value);
		    ne_md5_process_bytes(qop_value, strlen(qop_value), 
					 &sess->stored_rdig);
		    ne_md5_process_bytes(":", 1, &sess->stored_rdig);
		}
		/* Digest ":" H(A2) */
		ne_md5_process_bytes(a2_md5_ascii, 32, &sess->stored_rdig);
		/* All done */
		ne_md5_finish_ctx(&sess->stored_rdig, rdig_md5);
		ne_md5_to_ascii(rdig_md5, rdig_md5_ascii);

		NE_DEBUG(NE_DBG_HTTPAUTH, "Calculated response-digest of: "
			 "[%s]\n", rdig_md5_ascii);
		NE_DEBUG(NE_DBG_HTTPAUTH, "Given response-digest of:      "
			 "[%s]\n", rspauth);

		/* And... do they match? */
		okay = (strcasecmp(rdig_md5_ascii, rspauth) == 0)?0:-1;
		NE_DEBUG(NE_DBG_HTTPAUTH, "Matched: %s\n", okay?"nope":"YES!");
	    }
	}
    } else {
	NE_DEBUG(NE_DBG_HTTPAUTH, "No qop directive, auth okay.\n");
	okay = 0;
    }

    /* Check for a nextnonce */
    if (nextnonce != NULL) {
	NE_DEBUG(NE_DBG_HTTPAUTH, "Found nextnonce of [%s].\n", nextnonce);
	if (sess->nonce != NULL)
	    ne_free(sess->nonce);
	sess->nonce = ne_strdup(nextnonce);
    }

    ne_free(hdr);

    return okay;
}

/* Passed the value of a "(Proxy,WWW)-Authenticate: " header field.
 * Returns 0 if valid challenge was accepted; non-zero if no valid
 * challenge was found. */
static int auth_challenge(auth_session *sess, const char *value) 
{
    char *pnt, *key, *val, *hdr;
    struct auth_challenge *chall = NULL, *challenges = NULL;
    int success;

    pnt = hdr = ne_strdup(value); 

    NE_DEBUG(NE_DBG_HTTPAUTH, "Got new auth challenge: %s\n", value);

    /* The header value may be made up of one or more challenges.  We
     * split it down into attribute-value pairs, then search for
     * schemes in the pair keys. */

    while (!tokenize(&pnt, &key, &val, 1)) {

	if (val == NULL) {
	    /* We have a new challenge */
	    NE_DEBUG(NE_DBG_HTTPAUTH, "New challenge for scheme [%s]\n", key);
	    chall = ne_calloc(sizeof *chall);

	    chall->next = challenges;
	    challenges = chall;
	    /* Initialize the challenge parameters */
	    /* Which auth-scheme is it (case-insensitive matching) */
	    if (strcasecmp(key, "basic") == 0) {
		NE_DEBUG(NE_DBG_HTTPAUTH, "Basic scheme.\n");
		chall->scheme = auth_scheme_basic;
	    } else if (strcasecmp(key, "digest") == 0) {
		NE_DEBUG(NE_DBG_HTTPAUTH, "Digest scheme.\n");
		chall->scheme = auth_scheme_digest;
#ifdef HAVE_GSSAPI		
	    } else if (strcasecmp(key, "gss-negotiate") == 0) {
		NE_DEBUG(NE_DBG_HTTPAUTH, "GSSAPI scheme.\n");
		chall->scheme = auth_scheme_gssapi;
#endif
	    } else {
		NE_DEBUG(NE_DBG_HTTPAUTH, "Unknown scheme.\n");
		ne_free(chall);
		challenges = NULL;
		break;
	    }
	    continue;
	} else if (chall == NULL) {
	    /* If we haven't got an auth-scheme, and we're
	     * haven't yet found a challenge, skip this pair.
	     */
	    continue;
	}

	/* Strip quotes of value. */
	val = ne_shave(val, "\"'");

	NE_DEBUG(NE_DBG_HTTPAUTH, "Got pair: [%s] = [%s]\n", key, val);

	if (strcasecmp(key, "realm") == 0) {
	    chall->realm = val;
	} else if (strcasecmp(key, "nonce") == 0) {
	    chall->nonce = val;
	} else if (strcasecmp(key, "opaque") == 0) {
	    chall->opaque = val;
	} else if (strcasecmp(key, "stale") == 0) {
	    /* Truth value */
	    chall->stale = (strcasecmp(val, "true") == 0);
	} else if (strcasecmp(key, "algorithm") == 0) {
	    if (strcasecmp(val, "md5") == 0) {
		chall->alg = auth_alg_md5;
	    } else if (strcasecmp(val, "md5-sess") == 0) {
		chall->alg = auth_alg_md5_sess;
	    } else {
		chall->alg = auth_alg_unknown;
	    }
	} else if (strcasecmp(key, "qop") == 0) {
            /* iterate over each token in the value */
            do {
                const char *tok = ne_shave(ne_token(&val, ','), " \t");
                
                if (strcasecmp(tok, "auth") == 0) {
                    chall->qop_auth = 1;
                } else if (strcasecmp(tok, "auth-int") == 0 ) {
                    chall->qop_auth_int = 1;
                }
            } while (val);
	}
    }

    NE_DEBUG(NE_DBG_HTTPAUTH, "Finished parsing parameters.\n");

    /* Did we find any challenges */
    if (challenges == NULL) {
	ne_free(hdr);
	return -1;
    }
    
    success = 0;

#ifdef HAVE_GSSAPI
    NE_DEBUG(NE_DBG_HTTPAUTH, "Looking for GSSAPI.\n");
    /* Try a GSSAPI challenge */
    for (chall = challenges; chall != NULL; chall = chall->next) {
	if (chall->scheme == auth_scheme_gssapi) {
	    if (!gssapi_challenge(sess, chall)) {
		success = 1;
		break;
	    }
	}
    }
#endif

    /* Try a digest challenge */
    if (!success) {
        NE_DEBUG(NE_DBG_HTTPAUTH, "Looking for Digest challenges.\n");
        for (chall = challenges; chall != NULL; chall = chall->next) {
        	if (chall->scheme == auth_scheme_digest) {
        	    if (!digest_challenge(sess, chall)) {
        		success = 1;
        		break;
        	    }
        	}
        }
    }

    if (!success) {
	NE_DEBUG(NE_DBG_HTTPAUTH, 
		 "No good Digest challenges, looking for Basic.\n");
	for (chall = challenges; chall != NULL; chall = chall->next) {
	    if (chall->scheme == auth_scheme_basic) {
		if (!basic_challenge(sess, chall)) {
		    success = 1;
		    break;
		}
	    }
	}

	if (!success) {
	    /* No good challenges - record this in the session state */
	    NE_DEBUG(NE_DBG_HTTPAUTH, "Did not understand any challenges.\n");
	}

    }
    
    /* Remember whether we can now supply the auth details */
    sess->can_handle = success;

    while (challenges != NULL) {
	chall = challenges->next;
	ne_free(challenges);
	challenges = chall;
    }

    ne_free(hdr);

    return !success;
}

/* The body reader callback. */
static void auth_body_reader(void *cookie, const char *block, size_t length)
{
    struct ne_md5_ctx *ctx = cookie;
    NE_DEBUG(NE_DBG_HTTPAUTH, 
	     "Digesting %" NE_FMT_SIZE_T " bytes of response body.\n", length);
    ne_md5_process_bytes(block, length, ctx);
}

static void ah_create(ne_request *req, void *session, const char *method,
		      const char *uri)
{
    auth_session *sess = session;
    int is_connect = strcmp(method, "CONNECT") == 0;

    if (sess->context == AUTH_ANY ||
        (is_connect && sess->context == AUTH_CONNECT) ||
        (!is_connect && sess->context == AUTH_NOTCONNECT)) {
        struct auth_request *areq = ne_calloc(sizeof *areq);
        
        NE_DEBUG(NE_DBG_HTTPAUTH, "ah_create, for %s\n", sess->spec->resp_hdr);
        
        areq->method = method;
        areq->uri = uri;
        areq->request = req;
        
        ne_add_response_header_handler(req, sess->spec->resp_hdr,
                                       ne_duplicate_header, &areq->auth_hdr);
        
	
        ne_add_response_header_handler(req, sess->spec->resp_info_hdr,
                                       ne_duplicate_header, 
                                       &areq->auth_info_hdr);
        
        sess->attempt = 0;
        
        ne_set_request_private(req, sess->spec->id, areq);
    }
}


static void ah_pre_send(ne_request *r, void *cookie, ne_buffer *request)
{
    auth_session *sess = cookie;
    struct auth_request *req = ne_get_request_private(r, sess->spec->id);

    if (!sess->can_handle || !req) {
	NE_DEBUG(NE_DBG_HTTPAUTH, "Not handling session.\n");
    } else {
	char *value;

	NE_DEBUG(NE_DBG_HTTPAUTH, "Handling.");
	req->will_handle = 1;

	if (sess->qop == auth_qop_auth_int) {
	    /* Digest mode / qop=auth-int: take an MD5 digest of the
	     * response body. */
	    ne_md5_init_ctx(&req->response_body);
	    ne_add_response_body_reader(req->request, ne_accept_always, 
					auth_body_reader, &req->response_body);
	}
	
	switch(sess->scheme) {
	case auth_scheme_basic:
	    value = request_basic(sess);
	    break;
	case auth_scheme_digest:
	    value = request_digest(sess, req);
	    break;
#ifdef HAVE_GSSAPI	    
	case auth_scheme_gssapi:
	    value = request_gssapi(sess);
	    break;
#endif
	default:
	    value = NULL;
	    break;
	}

	if (value != NULL) {
	    ne_buffer_concat(request, sess->spec->req_hdr, ": ", value, NULL);
	    ne_free(value);
	}

    }

}

#define SAFELY(x) ((x) != NULL?(x):"null")

static int ah_post_send(ne_request *req, void *cookie, const ne_status *status)
{
    auth_session *sess = cookie;
    struct auth_request *areq = ne_get_request_private(req, sess->spec->id);
    int ret = NE_OK;

    if (!areq) return NE_OK;

    NE_DEBUG(NE_DBG_HTTPAUTH, 
	     "ah_post_send (#%d), code is %d (want %d), %s is %s\n",
	     sess->attempt, status->code, sess->spec->status_code, 
	     sess->spec->resp_hdr, SAFELY(areq->auth_hdr));
    if (areq->auth_info_hdr != NULL && 
	verify_response(areq, sess, areq->auth_info_hdr)) {
	NE_DEBUG(NE_DBG_HTTPAUTH, "Response authentication invalid.\n");
	ne_set_error(sess->sess, _(sess->spec->fail_msg));
	ret = NE_ERROR;
    } else if (status->code == sess->spec->status_code && 
	       areq->auth_hdr != NULL) {
	NE_DEBUG(NE_DBG_HTTPAUTH, "Got challenge with code %d.\n", status->code);
	if (!auth_challenge(sess, areq->auth_hdr)) {
	    ret = NE_RETRY;
	} else {
	    clean_session(sess);
	    ret = sess->spec->fail_code;
	}
    }

    NE_FREE(areq->auth_info_hdr);
    NE_FREE(areq->auth_hdr);
    
    return ret;
}

static void ah_destroy(ne_request *req, void *session)
{
    auth_session *sess = session;
    struct auth_request *areq = ne_get_request_private(req, sess->spec->id);
    if (areq) ne_free(areq);
}

static void free_auth(void *cookie)
{
    auth_session *sess = cookie;

    clean_session(sess);
    ne_free(sess);
}

static void auth_register(ne_session *sess, int isproxy,
			  const struct auth_class *ahc, const char *id, 
			  ne_auth_creds creds, void *userdata) 
{
    auth_session *ahs = ne_calloc(sizeof *ahs);

    ahs->creds = creds;
    ahs->userdata = userdata;
    ahs->sess = sess;
    ahs->spec = ahc;

    if (strcmp(ne_get_scheme(sess), "https") == 0)
        ahs->context = isproxy ? AUTH_CONNECT : AUTH_NOTCONNECT;
    else
        ahs->context = AUTH_ANY;
    
    /* Register hooks */
    ne_hook_create_request(sess, ah_create, ahs);
    ne_hook_pre_send(sess, ah_pre_send, ahs);
    ne_hook_post_send(sess, ah_post_send, ahs);
    ne_hook_destroy_request(sess, ah_destroy, ahs);
    ne_hook_destroy_session(sess, free_auth, ahs);

    ne_set_session_private(sess, id, ahs);
}

void ne_set_server_auth(ne_session *sess, ne_auth_creds creds, void *userdata)
{
    auth_register(sess, 0, &ah_server_class, HOOK_SERVER_ID, creds, userdata);
}

void ne_set_proxy_auth(ne_session *sess, ne_auth_creds creds, void *userdata)
{
    auth_register(sess, 1, &ah_proxy_class, HOOK_PROXY_ID, creds, userdata);
}

void ne_forget_auth(ne_session *sess)
{
    auth_session *as;
    if ((as = ne_get_session_private(sess, HOOK_SERVER_ID)) != NULL)
	clean_session(as);
    if ((as = ne_get_session_private(sess, HOOK_PROXY_ID)) != NULL)
	clean_session(as);
}

