/* 
   neon SSL/TLS support using OpenSSL
   Copyright (C) 2002-2003, Joe Orton <joe@manyfish.co.uk>
   Portions are:
   Copyright (C) 1999-2000 Tommi Komulainen <Tommi.Komulainen@iki.fi>

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

#include "config.h"

#include <sys/types.h>

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <stdio.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pkcs12.h>
#include <openssl/x509v3.h>
#include <openssl/rand.h>

#include "ne_ssl.h"
#include "ne_string.h"
#include "ne_session.h"
#include "ne_i18n.h"

#include "ne_private.h"
#include "ne_privssl.h"

/* OpenSSL 0.9.6 compatibility */
#if OPENSSL_VERSION_NUMBER < 0x0090700fL
#define PKCS12_unpack_authsafes M_PKCS12_unpack_authsafes
#define PKCS12_unpack_p7data M_PKCS12_unpack_p7data
/* cast away lack of const-ness */
#define OBJ_cmp(a,b) OBJ_cmp((ASN1_OBJECT *)(a), (ASN1_OBJECT *)(b))
#endif

struct ne_ssl_dname_s {
    X509_NAME *dn;
};

struct ne_ssl_certificate_s {
    ne_ssl_dname subj_dn, issuer_dn;
    X509 *subject;
    ne_ssl_certificate *issuer;
    char *identity;
};

struct ne_ssl_client_cert_s {
    PKCS12 *p12;
    int decrypted; /* non-zero if successfully decrypted. */
    ne_ssl_certificate cert;
    EVP_PKEY *pkey;
    char *friendly_name;
};

char *ne_ssl_readable_dname(const ne_ssl_dname *name)
{
    int n, flag = 0;
    ne_buffer *dump = ne_buffer_create();
    const ASN1_OBJECT * const cname = OBJ_nid2obj(NID_commonName),
	* const email = OBJ_nid2obj(NID_pkcs9_emailAddress);

    for (n = X509_NAME_entry_count(name->dn); n > 0; n--) {
	X509_NAME_ENTRY *ent = X509_NAME_get_entry(name->dn, n-1);
	
        /* Skip commonName or emailAddress except if there is no other
         * attribute in dname. */
	if ((OBJ_cmp(ent->object, cname) && OBJ_cmp(ent->object, email)) ||
            (!flag && n == 1)) {
	    if (flag)
		ne_buffer_append(dump, ", ", 2);
	    ne_buffer_append(dump, ent->value->data, ent->value->length);
	    flag = 1;
	}
    }

    return ne_buffer_finish(dump);
}

int ne_ssl_dname_cmp(const ne_ssl_dname *dn1, const ne_ssl_dname *dn2)
{
    return X509_NAME_cmp(dn1->dn, dn2->dn);
}

void ne_ssl_clicert_free(ne_ssl_client_cert *cc)
{
    if (cc->p12)
        PKCS12_free(cc->p12);
    if (cc->decrypted) {
        if (cc->cert.identity) ne_free(cc->cert.identity);
        EVP_PKEY_free(cc->pkey);
        X509_free(cc->cert.subject);
    }
    if (cc->friendly_name) ne_free(cc->friendly_name);
    ne_free(cc);
}

/* Map a server cert verification into a string. */
static void verify_err(ne_session *sess, int failures)
{
    struct {
	int bit;
	const char *str;
    } reasons[] = {
	{ NE_SSL_NOTYETVALID, N_("certificate is not yet valid") },
	{ NE_SSL_EXPIRED, N_("certificate has expired") },
	{ NE_SSL_IDMISMATCH, N_("certificate issued for a different hostname") },
	{ NE_SSL_UNTRUSTED, N_("issuer is not trusted") },
	{ 0, NULL }
    };
    int n, flag = 0;

    strcpy(sess->error, _("Server certificate verification failed: "));

    for (n = 0; reasons[n].bit; n++) {
	if (failures & reasons[n].bit) {
	    if (flag) strncat(sess->error, ", ", sizeof sess->error);
	    strncat(sess->error, _(reasons[n].str), sizeof sess->error);
	    flag = 1;
	}
    }

}

/* Format an ASN1 time to a string. 'buf' must be at least of size
 * 'NE_SSL_VDATELEN'. */
static void asn1time_to_string(ASN1_TIME *tm, char *buf)
{
    BIO *bio;
    
    strncpy(buf, _("[invalid date]"), NE_SSL_VDATELEN-1);
    
    bio = BIO_new(BIO_s_mem());
    if (bio) {
	if (ASN1_TIME_print(bio, tm))
	    BIO_read(bio, buf, NE_SSL_VDATELEN-1);
	BIO_free(bio);
    }
}

void ne_ssl_cert_validity(const ne_ssl_certificate *cert,
                          char *from, char *until)
{
    ASN1_TIME *notBefore = X509_get_notBefore(cert->subject);
    ASN1_TIME *notAfter = X509_get_notAfter(cert->subject);
    
    if (from) asn1time_to_string(notBefore, from);
    if (until) asn1time_to_string(notAfter, until);
}

/* Return non-zero if hostname from certificate (cn) matches hostname
 * used for session (hostname). TODO: could do more advanced wildcard
 * matching using fnmatch() here, if fnmatch is present. */
static int match_hostname(char *cn, const char *hostname)
{
    const char *dot;
    NE_DEBUG(NE_DBG_SSL, "Match %s on %s...\n", cn, hostname);
    dot = strchr(hostname, '.');
    if (dot == NULL) {
	char *pnt = strchr(cn, '.');
	/* hostname is not fully-qualified; unqualify the cn. */
	if (pnt != NULL) {
	    *pnt = '\0';
	}
    }
    else if (strncmp(cn, "*.", 2) == 0) {
	hostname = dot + 1;
	cn += 2;
    }
    return !strcasecmp(cn, hostname);
}

/* Check certificate identity.  Returns zero if identity matches; 1 if
 * identity does not match, or <0 if the certificate had no identity.
 * If 'identity' is non-NULL, store the malloc-allocated identity in
 * *identity. */
static int check_identity(const char *hostname, X509 *cert, char **identity)
{
    STACK_OF(GENERAL_NAME) *names;
    int match = 0, found = 0;
    
    names = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
    if (names) {
	/* Got a subject alt. name extension. */
	int n;

	for (n = 0; n < sk_GENERAL_NAME_num(names) && !match; n++) {
	    GENERAL_NAME *nm = sk_GENERAL_NAME_value(names, n);
	    
	    /* only care about this if it is a DNS name. */
	    if (nm->type == GEN_DNS) {
		char *name = ne_strndup(nm->d.ia5->data, nm->d.ia5->length);
                if (identity && !found) *identity = ne_strdup(name);
		match = match_hostname(name, hostname);
		ne_free(name);
		found = 1;
	    }
	}
        /* free the whole stack. */
        sk_GENERAL_NAME_pop_free(names, GENERAL_NAME_free);
    }
    
    /* Check against the commonName if no DNS alt. names were found,
     * as per RFC2818. */
    if (!found) {
	X509_NAME *subj = X509_get_subject_name(cert);
	X509_NAME_ENTRY *entry;
	ASN1_STRING *str;
	int idx = -1, lastidx;
	char *name;

	/* find the most specific commonName attribute. */
	do {
	    lastidx = idx;
	    idx = X509_NAME_get_index_by_NID(subj, NID_commonName, lastidx);
	} while (idx >= 0);
	
	if (lastidx < 0)
	    return -1;

	/* extract the string from the entry */
	entry = X509_NAME_get_entry(subj, lastidx);
	str = X509_NAME_ENTRY_get_data(entry);

	name = ne_strndup(str->data, str->length);
        if (identity) *identity = ne_strdup(name);
	match = match_hostname(name, hostname);
	ne_free(name);
    }

    NE_DEBUG(NE_DBG_SSL, "Identity match: %s\n", match ? "good" : "bad");
    return match ? 0 : 1;
}

/* Populate an ne_ssl_certificate structure from an X509 object. */
static ne_ssl_certificate *populate_cert(ne_ssl_certificate *cert, X509 *x5)
{
    cert->subj_dn.dn = X509_get_subject_name(x5);
    cert->issuer_dn.dn = X509_get_issuer_name(x5);
    cert->issuer = NULL;
    cert->subject = x5;
    /* Retrieve the cert identity; pass a dummy hostname to match. */
    cert->identity = NULL;
    check_identity("", x5, &cert->identity);
    return cert;
}

/* Return a linked list of certificate objects from an OpenSSL chain. */
static ne_ssl_certificate *make_chain(STACK_OF(X509) *chain)
{
    int n, count = sk_X509_num(chain);
    ne_ssl_certificate *top = NULL, *current = NULL;
    
    NE_DEBUG(NE_DBG_SSL, "Chain depth: %d\n", count);

    for (n = 0; n < count; n++) {
        ne_ssl_certificate *cert = ne_malloc(sizeof *cert);
        populate_cert(cert, X509_dup(sk_X509_value(chain, n)));
#if NE_DEBUGGING
        if (ne_debug_mask & NE_DBG_SSL) {
            fprintf(ne_debug_stream, "Cert #%d:\n", n);
            X509_print_fp(ne_debug_stream, cert->subject);
        }
#endif
        if (top == NULL) {
            current = top = cert;
        } else {
            current->issuer = cert;
            current = cert;
        }
    }

    return top;
}

/* Verifies an SSL server certificate. */
static int check_certificate(ne_session *sess, SSL *ssl, ne_ssl_certificate *chain)
{
    X509 *cert = chain->subject;
    ASN1_TIME *notBefore = X509_get_notBefore(cert);
    ASN1_TIME *notAfter = X509_get_notAfter(cert);
    int ret, failures = 0;
    long result;

    /* check expiry dates */
    if (X509_cmp_current_time(notBefore) >= 0)
	failures |= NE_SSL_NOTYETVALID;
    else if (X509_cmp_current_time(notAfter) <= 0)
	failures |= NE_SSL_EXPIRED;

    /* Check certificate was issued to this server. */
    ret = check_identity(sess->server.hostname, cert, NULL);
    if (ret < 0) {
        ne_set_error(sess, _("Server certificate was missing commonName "
                             "attribute in subject name"));
        return NE_ERROR;
    } else if (ret > 0) failures |= NE_SSL_IDMISMATCH;

    /* get the result of the cert verification out of OpenSSL */
    result = SSL_get_verify_result(ssl);

    NE_DEBUG(NE_DBG_SSL, "Verify result: %ld = %s\n", result,
	     X509_verify_cert_error_string(result));

    switch (result) {
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
	/* TODO: and probably more result codes here... */
	failures |= NE_SSL_UNTRUSTED;
	break;
	/* ignore these, since we've already noticed them: */
    case X509_V_ERR_CERT_NOT_YET_VALID:
    case X509_V_ERR_CERT_HAS_EXPIRED:
        /* cert was trusted: */
    case X509_V_OK:
	break;
    default:
	/* TODO: tricky to handle the 30-odd failure cases OpenSSL
	 * presents here (see x509_vfy.h), and present a useful API to
	 * the application so it in turn can then present a meaningful
	 * UI to the user.  The only thing to do really would be to
	 * pass back the error string, but that's not localisable.  So
	 * just fail the verification here - better safe than
	 * sorry. */
	ne_set_error(sess, _("Certificate verification error: %s"),
		     X509_verify_cert_error_string(result));
	return NE_ERROR;
    }

    if (failures == 0) {
        /* verified OK! */
        ret = NE_OK;
    } else {
        /* Set up the error string. */
	verify_err(sess, failures);
        ret = NE_ERROR;
        /* Allow manual override */
        if (sess->ssl_verify_fn && 
            sess->ssl_verify_fn(sess->ssl_verify_ud, failures, chain) == 0)
            ret = NE_OK;
    }

    return ret;
}

/* Duplicate a client certificate, which must be in the decrypted state. */
static ne_ssl_client_cert *dup_client_cert(const ne_ssl_client_cert *cc)
{
    ne_ssl_client_cert *newcc = ne_calloc(sizeof *newcc);
    
    newcc->decrypted = 1;
    newcc->pkey = cc->pkey;
    if (cc->friendly_name)
        newcc->friendly_name = ne_strdup(cc->friendly_name);

    populate_cert(&newcc->cert, cc->cert.subject);

    cc->cert.subject->references++;
    cc->pkey->references++;
    return newcc;
}

/* Callback invoked when the SSL server requests a client certificate.  */
static int provide_client_cert(SSL *ssl, X509 **cert, EVP_PKEY **pkey)
{
    ne_ssl_context *ctx = SSL_get_app_data(ssl);
    ne_session *sess = SSL_CTX_get_app_data(ctx->ctx);

    if (!sess->client_cert && sess->ssl_provide_fn) {
	ne_ssl_dname **dnames = NULL;
        int n, count = 0;
	STACK_OF(X509_NAME) *ca_list = SSL_get_client_CA_list(ssl);

        if (ca_list == NULL) {
            /* no list of acceptable CA names provided. */
            dnames = NULL;
            count = 0;
        } else {
            count = sk_X509_NAME_num(ca_list);

            dnames = ne_malloc(count * sizeof(ne_ssl_dname *));
            
            for (n = 0; n < count; n++) {
                dnames[n] = ne_malloc(sizeof(ne_ssl_dname));
                dnames[n]->dn = sk_X509_NAME_value(ca_list, n);
            }
        }

	NE_DEBUG(NE_DBG_SSL, "Calling client certificate provider...\n");
	sess->ssl_provide_fn(sess->ssl_provide_ud, sess, 
                             (const ne_ssl_dname *const *)dnames, count);
        if (count) {
            for (n = 0; n < count; n++)
                ne_free(dnames[n]);
            ne_free(dnames);
        }
    }

    if (sess->client_cert) {
        ne_ssl_client_cert *const cc = sess->client_cert;
	NE_DEBUG(NE_DBG_SSL, "Supplying client certificate.\n");
	cc->pkey->references++;
	cc->cert.subject->references++;
	*cert = cc->cert.subject;
	*pkey = cc->pkey;
	return 1;
    } else {
	NE_DEBUG(NE_DBG_SSL, "No client certificate supplied.\n");
	return 0;
    }
}

void ne_ssl_set_clicert(ne_session *sess, const ne_ssl_client_cert *cc)
{
    sess->client_cert = dup_client_cert(cc);
}

ne_ssl_context *ne_ssl_context_create(void)
{
    ne_ssl_context *ctx = ne_malloc(sizeof *ctx);
    ctx->ctx = SSL_CTX_new(SSLv23_client_method());
    ctx->sess = NULL;
    /* set client cert callback. */
    SSL_CTX_set_client_cert_cb(ctx->ctx, provide_client_cert);
    /* enable workarounds for buggy SSL server implementations */
    SSL_CTX_set_options(ctx->ctx, SSL_OP_ALL);
    return ctx;
}

void ne_ssl_context_destroy(ne_ssl_context *ctx)
{
    SSL_CTX_free(ctx->ctx);
    if (ctx->sess)
        SSL_SESSION_free(ctx->sess);
    ne_free(ctx);
}

/* For internal use only. */
int ne_negotiate_ssl(ne_request *req)
{
    ne_session *sess = ne_get_session(req);
    ne_ssl_context *ctx = sess->ssl_context;
    ne_ssl_socket *sock;
    STACK_OF(X509) *chain;

    NE_DEBUG(NE_DBG_SSL, "Doing SSL negotiation.\n");

    /* Rather a hack: link the ssl_context back to the ne_session, so
     * provide_client_cert can get to the ne_session. */
    SSL_CTX_set_app_data(ctx->ctx, sess);

    if (ne_sock_connect_ssl(sess->socket, ctx)) {
	if (ctx->sess) {
	    /* remove cached session. */
	    SSL_SESSION_free(ctx->sess);
	    ctx->sess = NULL;
	}
	ne_set_error(sess, _("SSL negotiation failed: %s"),
		     ne_sock_error(sess->socket));
	return NE_ERROR;
    }	
    
    sock = ne_sock_sslsock(sess->socket);

    chain = SSL_get_peer_cert_chain(sock->ssl);
    if (chain == NULL || sk_X509_num(chain) == 0) {
	ne_set_error(sess, _("SSL server did not present certificate"));
	return NE_ERROR;
    }

    if (sess->server_cert) {
	if (X509_cmp(sk_X509_value(chain, 0), sess->server_cert->subject)) {
	    /* This could be a MITM attack: fail the request. */
	    ne_set_error(sess, _("Server certificate changed: "
				 "connection intercepted?"));
	    return NE_ERROR;
	} 
	/* certificate has already passed verification: no need to
	 * verify it again. */
    } else {
	/* new connection: create the chain. */
        ne_ssl_certificate *cert = make_chain(chain);

	if (check_certificate(sess, sock->ssl, cert)) {
	    NE_DEBUG(NE_DBG_SSL, "SSL certificate checks failed: %s\n",
		     sess->error);
	    ne_ssl_cert_free(cert);
	    return NE_ERROR;
	}
	/* remember the chain. */
        sess->server_cert = cert;
    }
    
    if (!ctx->sess) {
	/* store the session. */
	ctx->sess = SSL_get1_session(sock->ssl);
    }

    if (sess->notify_cb) {
	sess->notify_cb(sess->notify_ud, ne_conn_secure,
                        SSL_get_version(sock->ssl));
    }

    return NE_OK;
}

const ne_ssl_dname *ne_ssl_cert_issuer(const ne_ssl_certificate *cert)
{
    return &cert->issuer_dn;
}

const ne_ssl_dname *ne_ssl_cert_subject(const ne_ssl_certificate *cert)
{
    return &cert->subj_dn;
}

const ne_ssl_certificate *ne_ssl_cert_signedby(const ne_ssl_certificate *cert)
{
    return cert->issuer;
}

const char *ne_ssl_cert_identity(const ne_ssl_certificate *cert)
{
    return cert->identity;
}

void ne_ssl_ctx_trustcert(ne_ssl_context *ctx, const ne_ssl_certificate *cert)
{
    X509_STORE *store = SSL_CTX_get_cert_store(ctx->ctx);
    
    X509_STORE_add_cert(store, cert->subject);
}

void ne_ssl_trust_default_ca(ne_session *sess)
{
    X509_STORE *store = SSL_CTX_get_cert_store(sess->ssl_context->ctx);
    
    X509_STORE_set_default_paths(store);
}

/* Find a friendly name in a PKCS12 structure the hard way, without
 * decrypting the parts which are encrypted.. */
static char *find_friendly_name(PKCS12 *p12)
{
    STACK_OF(PKCS7) *safes = PKCS12_unpack_authsafes(p12);
    int n, m;
    char *name = NULL;

    if (safes == NULL) return NULL;
    
    /* Iterate over the unpacked authsafes: */
    for (n = 0; n < sk_PKCS7_num(safes) && !name; n++) {
        PKCS7 *safe = sk_PKCS7_value(safes, n);
        STACK_OF(PKCS12_SAFEBAG) *bags;
    
        /* Only looking for unencrypted authsafes. */
        if (OBJ_obj2nid(safe->type) != NID_pkcs7_data) continue;

        bags = PKCS12_unpack_p7data(safe);
        if (!bags) continue;

        /* Iterate through the bags, picking out a friendly name */
        for (m = 0; m < sk_PKCS12_SAFEBAG_num(bags) && !name; m++) {
            PKCS12_SAFEBAG *bag = sk_PKCS12_SAFEBAG_value(bags, m);
            name = PKCS12_get_friendlyname(bag);
        }
    
        sk_PKCS12_SAFEBAG_pop_free(bags, PKCS12_SAFEBAG_free);
    }

    sk_PKCS7_pop_free(safes, PKCS7_free);
    return name;
}

ne_ssl_client_cert *ne_ssl_clicert_read(const char *filename)
{
    PKCS12 *p12;
    FILE *fp;
    X509 *cert;
    EVP_PKEY *pkey;
    ne_ssl_client_cert *cc;

    fp = fopen(filename, "rb");
    if (fp == NULL)
        return NULL;

    p12 = d2i_PKCS12_fp(fp, NULL);

    fclose(fp);
    
    if (p12 == NULL) {
        ERR_clear_error();
        return NULL;
    }

    /* Try parsing with no password. */
    if (PKCS12_parse(p12, NULL, &pkey, &cert, NULL) == 1) {
        /* Success - no password needed for decryption. */
        unsigned int len = 0;
        unsigned char *name = X509_alias_get0(cert, &len);
        
        cc = ne_calloc(sizeof *cc);
        cc->pkey = pkey;
        cc->decrypted = 1;
        if (name && len)
            cc->friendly_name = ne_strndup((char *)name, len);
        populate_cert(&cc->cert, cert);
        PKCS12_free(p12);
        return cc;
    } else {
        /* Failed to parse the file */
        int err = ERR_get_error();
        ERR_clear_error();
        if (ERR_GET_LIB(err) == ERR_LIB_PKCS12 &&
            ERR_GET_REASON(err) == PKCS12_R_MAC_VERIFY_FAILURE) {
            /* Decryption error due to bad password. */
            cc = ne_calloc(sizeof *cc);
            cc->friendly_name = find_friendly_name(p12);
            cc->p12 = p12;
            return cc;
        } else {
            /* Some parse error, give up. */
            PKCS12_free(p12);
            return NULL;
        }
    }
}

int ne_ssl_clicert_encrypted(const ne_ssl_client_cert *cc)
{
    return !cc->decrypted;
}

int ne_ssl_clicert_decrypt(ne_ssl_client_cert *cc, const char *password)
{
    X509 *cert;
    EVP_PKEY *pkey;

    if (PKCS12_parse(cc->p12, password, &pkey, &cert, NULL) != 1) {
        ERR_clear_error();
        return -1;
    }
    
    PKCS12_free(cc->p12);
    populate_cert(&cc->cert, cert);
    cc->pkey = pkey;
    cc->decrypted = 1;
    cc->p12 = NULL;
    return 0;
}

const ne_ssl_certificate *ne_ssl_clicert_owner(const ne_ssl_client_cert *cc)
{
    return &cc->cert;
}

const char *ne_ssl_clicert_name(ne_ssl_client_cert *ccert)
{
    return ccert->friendly_name;
}

ne_ssl_certificate *ne_ssl_cert_read(const char *filename)
{
    FILE *fp = fopen(filename, "r");
    X509 *cert;

    if (fp == NULL)
        return NULL;

    cert = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);

    if (cert == NULL) {
        NE_DEBUG(NE_DBG_SSL, "d2i_X509_fp failed: %s\n", 
                 ERR_reason_error_string(ERR_get_error()));
        ERR_clear_error();
        return NULL;
    }

    return populate_cert(ne_calloc(sizeof(struct ne_ssl_certificate_s)), cert);
}

int ne_ssl_cert_write(const ne_ssl_certificate *cert, const char *filename)
{
    FILE *fp = fopen(filename, "w");

    if (fp == NULL) return -1;

    if (PEM_write_X509(fp, cert->subject) != 1) {
        fclose(fp);
        return -1;
    }
    
    if (fclose(fp) != 0)
        return -1;

    return 0;
}

void ne_ssl_cert_free(ne_ssl_certificate *cert)
{
    X509_free(cert->subject);
    if (cert->issuer)
        ne_ssl_cert_free(cert->issuer);
    if (cert->identity)
        ne_free(cert->identity);
    ne_free(cert);
}

int ne_ssl_cert_cmp(const ne_ssl_certificate *c1, const ne_ssl_certificate *c2)
{
    return X509_cmp(c1->subject, c2->subject);
}

/* The certificate import/export format is the base64 encoding of the
 * raw DER; PEM without the newlines and wrapping. */

ne_ssl_certificate *ne_ssl_cert_import(const char *data)
{
    unsigned char *der, *p;
    size_t len;
    X509 *x5;
    
    /* decode the base64 to get the raw DER representation */
    len = ne_unbase64(data, &der);
    if (len == 0) return NULL;

    p = der;
    x5 = d2i_X509(NULL, &p, len); /* p is incremented */
    ne_free(der);
    if (x5 == NULL) {
        ERR_clear_error();
        return NULL;
    }

    return populate_cert(ne_calloc(sizeof(struct ne_ssl_certificate_s)), x5);
}

char *ne_ssl_cert_export(const ne_ssl_certificate *cert)
{
    int len;
    unsigned char *der, *p;
    
    /* find the length of the DER encoding. */
    len = i2d_X509(cert->subject, NULL);

    p = der = ne_malloc(len);
    i2d_X509(cert->subject, &p); /* p is incremented */

    p = ne_base64(der, len);
    ne_free(der);
    return p;
}

#if SHA_DIGEST_LENGTH != 20
# error SHA digest length is not 20 bytes
#endif

int ne_ssl_cert_digest(const ne_ssl_certificate *cert, char *digest)
{
    unsigned char sha1[EVP_MAX_MD_SIZE];
    unsigned int len, j;
    char *p;

    if (!X509_digest(cert->subject, EVP_sha1(), sha1, &len) || len != 20) {
        ERR_clear_error();
        return -1;
    }
    
    for (j = 0, p = digest; j < 20; j++) {
        *p++ = NE_HEX2ASC((sha1[j] >> 4) & 0x0f);
        *p++ = NE_HEX2ASC(sha1[j] & 0x0f);
        *p++ = ':';
    }

    *--p = '\0';
    return 0;
}
