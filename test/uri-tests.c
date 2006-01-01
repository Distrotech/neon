/* 
   URI handling tests
   Copyright (C) 2001-2006, Joe Orton <joe@manyfish.co.uk>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
  
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
  
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include "config.h"

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "ne_uri.h"
#include "ne_alloc.h"

#include "tests.h"

static int simple(void)
{
    ne_uri p = {0};
    ON(ne_uri_parse("http://www.webdav.org/foo", &p));
    ON(strcmp(p.host, "www.webdav.org"));
    ON(strcmp(p.path, "/foo"));
    ON(strcmp(p.scheme, "http"));
    ON(p.port);
    ON(p.userinfo != NULL);
    ne_uri_free(&p);
    return 0;
}

static int simple_ssl(void)
{
    ne_uri p = {0};
    ON(ne_uri_parse("https://webdav.org/", &p));
    ON(strcmp(p.scheme, "https"));
    ON(p.port);
    ne_uri_free(&p);
    return OK;
}

static int no_path(void)
{
    ne_uri p = {0};
    ON(ne_uri_parse("https://webdav.org", &p));
    ON(strcmp(p.path, "/"));
    ne_uri_free(&p);
    return OK;
}

#define STR "/a�����/"
static int escapes(void)
{
    char *un, *esc;
    esc = ne_path_escape(STR);
    ON(esc == NULL);
    un = ne_path_unescape(esc);
    ON(un == NULL);
    ON(strcmp(un, STR));
    ne_free(un);
    ne_free(esc);
    ONN("unescape accepted invalid URI", 
        ne_path_unescape("/foo%zzbar") != NULL);
    /* no-escape path */
    esc = ne_path_escape("/foobar");
    ON(strcmp(esc, "/foobar"));
    ne_free(esc);       
    return OK;
}    

static int parents(void)
{
    static const struct {
	const char *path, *parent;
    } ps[] = {
	{ "/a/b/c", "/a/b/" },
	{ "/a/b/c/", "/a/b/" },
	{ "/alpha/beta", "/alpha/" },
	{ "/foo", "/" },
	{ "norman", NULL },
	{ "/", NULL },
	{ "", NULL },
	{ NULL, NULL }
    };
    int n;
    
    for (n = 0; ps[n].path != NULL; n++) {
	char *p = ne_path_parent(ps[n].path);
	if (ps[n].parent == NULL) {
	    ONV(p != NULL, ("parent of `%s' was `%s' not NULL", 
			    ps[n].path, p));
	} else {
	    ONV(p == NULL, ("parent of `%s' was NULL", ps[n].path));
	    ONV(strcmp(p, ps[n].parent),
		("parent of `%s' was `%s' not `%s'",
		 ps[n].path, p, ps[n].parent));
	    ne_free(p);
	}
    }

    return OK;
}

static int compares(void)
{
    const char *alpha = "/alpha";

    ON(ne_path_compare("/a", "/a/") != 0);
    ON(ne_path_compare("/a/", "/a") != 0);
    ON(ne_path_compare("/ab", "/a/") == 0);
    ON(ne_path_compare("/a/", "/ab") == 0);
    ON(ne_path_compare("/a/", "/a/") != 0);
    ON(ne_path_compare("/alpha/", "/beta/") == 0);
    ON(ne_path_compare("/alpha", "/b") == 0);
    ON(ne_path_compare("/alpha/", "/alphash") == 0);
    ON(ne_path_compare("/fish/", "/food") == 0);
    ON(ne_path_compare(alpha, alpha) != 0);
    ON(ne_path_compare("/a/b/c/d", "/a/b/c/") == 0);
    return OK;
}

/* Checks that a URI comparison of 'u1' and 'u2', which have differing
 * 'field', doesn't compare to equal; and that they are ordered
 * correctly. */
static int cmp_differ(const char *field,
		      const ne_uri *u1, const ne_uri *u2)
{
    ONV(ne_uri_cmp(u1, u2) == 0,
	("URIs with different %s were equal", field));

    ONV(ne_uri_cmp(u2, u1) == 0,
	("URIs with different %s were equal (reversed)", field));

    /* relies on strcmp return value being of equal magnitude when
     * arguments are reversed; not sure if this is portable
     * assumption. */
    ONV(ne_uri_cmp(u1, u2) + ne_uri_cmp(u2, u1) != 0,
	("relative ordering of URIs with different %s incorrect", field));

    return OK;
}

static int cmp(void)
{
    ne_uri alpha, beta;
    
    alpha.path = "/alpha";
    alpha.scheme = "http";
    alpha.host = "example.com";
    alpha.port = 80;

    beta = alpha; /* structure copy. */

    ONN("equal URIs not equal", ne_uri_cmp(&alpha, &beta) != 0);

    beta.path = "/beta";
    CALL(cmp_differ("path", &alpha, &beta));

    beta = alpha; beta.scheme = "https";
    CALL(cmp_differ("scheme", &alpha, &beta));
    
    beta = alpha; beta.port = 433;
    CALL(cmp_differ("port", &alpha, &beta));

    beta = alpha; beta.host = "fish.com";
    CALL(cmp_differ("host", &alpha, &beta));

    beta = alpha; beta.host = "EXAMPLE.CoM";
    ONN("hostname comparison not case-insensitive",
	ne_uri_cmp(&alpha, &beta) != 0);
    
    beta = alpha; beta.scheme = "HtTp";
    ONN("scheme comparison not case-insensitive",
	ne_uri_cmp(&alpha, &beta) != 0);
    
    beta = alpha; beta.path = ""; alpha.path = "/";
    ONN("empty abspath doesn't match '/'",
	ne_uri_cmp(&alpha, &beta) != 0);
    ONN("'/' doesn't match empty abspath",
	ne_uri_cmp(&beta, &alpha) != 0);

    beta = alpha; alpha.path = ""; beta.path = "/foo";
    ONN("empty abspath matched '/foo'", ne_uri_cmp(&alpha, &beta) == 0);
    ONN("'/foo' matched empty abspath ", ne_uri_cmp(&beta, &alpha) == 0);

    return OK;
}

static int children(void)
{
    ON(ne_path_childof("/a", "/a/b") == 0);
    ON(ne_path_childof("/a/", "/a/b") == 0);
    ON(ne_path_childof("/aa/b/c", "/a/b/c/d/e") != 0);
    ON(ne_path_childof("////", "/a") != 0);
    return OK;
}

static int slash(void)
{
    ON(ne_path_has_trailing_slash("/a/") == 0);
    ON(ne_path_has_trailing_slash("/a") != 0);
    {
	/* check the uri == "" case. */
	char *foo = "/";
	ON(ne_path_has_trailing_slash(&foo[1]));
    }
    return OK;
}

static int default_port(void)
{
    ONN("default http: port incorrect", ne_uri_defaultport("http") != 80);
    ONN("default https: port incorrect", ne_uri_defaultport("https") != 443);
    ONN("unspecified scheme: port incorrect", ne_uri_defaultport("ldap") != 0);
    return OK;
}

static int parse(void)
{
    static const struct test_uri {
	const char *uri, *scheme, *host;
	unsigned int port;
	const char *path, *userinfo, *query, *fragment;
    } uritests[] = {
        { "http://webdav.org/norman", "http", "webdav.org", 0, "/norman", NULL, NULL, NULL },
        { "http://webdav.org:/norman", "http", "webdav.org", 0, "/norman", NULL, NULL, NULL },
        { "https://webdav.org/foo", "https", "webdav.org", 0, "/foo", NULL, NULL, NULL },
        { "http://webdav.org:8080/bar", "http", "webdav.org", 8080, "/bar", NULL, NULL, NULL },
        { "http://a/b", "http", "a", 0, "/b", NULL, NULL, NULL },
        { "http://webdav.org/bar:fish", "http", "webdav.org", 0, "/bar:fish", NULL, NULL, NULL },
        { "http://webdav.org", "http", "webdav.org", 0, "/", NULL, NULL, NULL },
        { "http://webdav.org/fish@food", "http", "webdav.org", 0, "/fish@food", NULL, NULL, NULL },

        /* query/fragments */
        { "http://foo/bar?alpha", "http", "foo", 0, "/bar", NULL, "alpha", NULL },
        { "http://foo/bar?alpha#beta", "http", "foo", 0, "/bar", NULL, "alpha", "beta" },
        { "http://foo/bar#alpha?beta", "http", "foo", 0, "/bar", NULL, NULL, "alpha?beta" },
        { "http://foo/bar#beta", "http", "foo", 0, "/bar", NULL, NULL, "beta" },
        { "http://foo/bar?#beta", "http", "foo", 0, "/bar", NULL, "", "beta" },
        { "http://foo/bar?alpha?beta", "http", "foo", 0, "/bar", NULL, "alpha?beta", NULL },

        /* Examples from RFC3986�1.1.2: */
        { "ftp://ftp.is.co.za/rfc/rfc1808.txt", "ftp", "ftp.is.co.za", 0, "/rfc/rfc1808.txt", NULL, NULL, NULL },
        { "http://www.ietf.org/rfc/rfc2396.txt", "http", "www.ietf.org", 0, "/rfc/rfc2396.txt", NULL, NULL, NULL },
        { "ldap://[2001:db8::7]/c=GB?objectClass?one", "ldap", "[2001:db8::7]", 0, "/c=GB", NULL, "objectClass?one", NULL },
        { "mailto:John.Doe@example.com", "mailto", NULL, 0, "John.Doe@example.com", NULL, NULL, NULL }, 
        { "news:comp.infosystems.www.servers.unix", "news", NULL, 0, "comp.infosystems.www.servers.unix", NULL, NULL, NULL },
        { "tel:+1-816-555-1212", "tel", NULL, 0, "+1-816-555-1212", NULL, NULL, NULL },
        { "telnet://192.0.2.16:80/", "telnet", "192.0.2.16", 80, "/", NULL, NULL, NULL },
        { "urn:oasis:names:specification:docbook:dtd:xml:4.1.2", "urn", NULL, 0, 
          "oasis:names:specification:docbook:dtd:xml:4.1.2", NULL}, 

        /* userinfo */
        { "ftp://jim:bob@jim.com", "ftp", "jim.com", 0, "/", "jim:bob", NULL, NULL },
        { "ldap://fred:bloggs@fish.com/foobar", "ldap", "fish.com", 0, 
          "/foobar", "fred:bloggs", NULL, NULL },
        /* IPv6 hex strings allowed even if IPv6 not supported. */
        { "http://[::1]/foo", "http", "[::1]", 0, "/foo", NULL, NULL, NULL },
        { "http://[a:a:a:a::0]/foo", "http", "[a:a:a:a::0]", 0, "/foo", NULL, NULL, NULL },
        { "http://[::1]:8080/bar", "http", "[::1]", 8080, "/bar", NULL, NULL, NULL },
        { "ftp://[feed::cafe]:555", "ftp", "[feed::cafe]", 555, "/", NULL, NULL, NULL },

        /* URI-references: */
        { "//foo.com/bar", NULL, "foo.com", 0, "/bar", NULL, NULL, NULL },
        { "//foo.com", NULL, "foo.com", 0, "/", NULL, NULL, NULL },
        { "//[::1]/foo", NULL, "[::1]", 0, "/foo", NULL, NULL, NULL },
        { "/bar", NULL, NULL, 0, "/bar", NULL, NULL, NULL }, /* path-absolute */
        { "foo/bar", NULL, NULL, 0, "foo/bar", NULL, NULL, NULL }, /* path-noscheme */

	{ NULL }
    };
    int n;

    for (n = 0; uritests[n].uri != NULL; n++) {
	ne_uri res;
	const struct test_uri *e = &uritests[n];
	ONV(ne_uri_parse(e->uri, &res) != 0,
	    ("%s: parse failed", e->uri));
	ONV(res.port != e->port,
	    ("%s: parsed port was %d not %d", e->uri, res.port, e->port));
	ONCMP(e->scheme, res.scheme, e->uri, "scheme");
	ONCMP(e->host, res.host, e->uri, "host");
	ONV(strcmp(res.path, e->path),
	    ("%s: parsed path was %s not %s", e->uri, res.path, e->path));
        ONCMP(e->userinfo, res.userinfo, e->uri, "userinfo");
        ONCMP(e->query, res.query, e->uri, "query");
        ONCMP(e->fragment, res.fragment, e->uri, "fragment");
	ne_uri_free(&res);
    }

    return OK;
}

static int failparse(void)
{
    static const char *uris[] = {
	"",
	"http://[::1/",
        "http://foo/bar asda",
        "http://fish/[foo]/bar",
	NULL
    };
    int n;
    
    for (n = 0; uris[n] != NULL; n++) {
	ne_uri p;
	ONV(ne_uri_parse(uris[n], &p) == 0,
	    ("`%s' did not fail to parse", uris[n]));
	ne_uri_free(&p);
    }

    return 0;
}

static int unparse(void)
{
    const char *uris[] = {
	"http://foo.com/bar",
	"https://bar.com/foo/wishbone",
	"http://www.random.com:8000/",
	"http://[::1]:8080/",
	"ftp://ftp.foo.bar/abc/def",
	"http://a/b?c#d",
	"http://a/b?c",
	"http://a/b#d",
	NULL
    };
    int n;

    for (n = 0; uris[n] != NULL; n++) {
	ne_uri parsed;
	char *unp;

	ONV(ne_uri_parse(uris[n], &parsed),
	    ("failed to parse %s", uris[n]));
	
        if (parsed.port == 0)
            parsed.port = ne_uri_defaultport(parsed.scheme);

	unp = ne_uri_unparse(&parsed);

	ONV(strcmp(unp, uris[n]),
	    ("unparse got %s from %s", unp, uris[n]));
	
        ne_uri_free(&parsed);
	ne_free(unp);
    }

    return OK;    
}

ne_test tests[] = {
    T(simple),
    T(simple_ssl),
    T(no_path),
    T(escapes),
    T(parents),
    T(compares),
    T(cmp),
    T(children),
    T(slash),
    T(default_port),
    T(parse),
    T(failparse),
    T(unparse),
    T(NULL)
};
