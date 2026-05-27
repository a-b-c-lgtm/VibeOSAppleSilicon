/*
 * userspace/libc/origin.h -- chapter 121 same-origin policy helpers.
 *
 * "Origin" is the trust boundary the SOP guards: a URL's
 * <scheme, host, port> triple.  Two URLs share an origin if and
 * only if all three are byte-equal (with host folded to lower
 * case, port defaulted from scheme).  That's it.  No "registered
 * domain", no public-suffix list, no eTLD+1 -- those are tricks
 * real browsers use to cluster cookies across subdomains and we
 * deliberately don't have any of them (see chapter 120's note on
 * host-only cookies).
 *
 * This header is the second consumer of url.h (the first being
 * the browser fetch path).  It's deliberately tiny: one struct,
 * one parse helper that wraps url_parse, one equality test.
 *
 * Why a separate header
 * ---------------------
 * The SOP origin compare is needed in three places by chapter
 * 110a:
 *
 *   - browser.c form_submit_url_at -- block cross-origin
 *     relative-action submits.
 *   - browser CLI --check-sop debug subcommand.
 *   - tests (via --check-sop, no direct linkage).
 *
 * Future chapters (a pocket JS DOM, the iframe sandbox we'll
 * never write, etc) will reach for the same compare.  Putting
 * it in libc keeps each binary's copy of the policy honest --
 * the rule lives in one file, all callers share it.
 */

#ifndef OSDEV_LIBC_ORIGIN_H
#define OSDEV_LIBC_ORIGIN_H

#include "url.h"
#include "freestanding.h"

struct origin {
    int      scheme;          /* URL_SCHEME_HTTP / URL_SCHEME_HTTPS */
    uint16_t port;            /* always explicit, scheme default if absent */
    char     host[URL_HOST_MAX];
};

/* Lower-case an ASCII byte in place. */
static inline char origin_tolower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Parse `url` into an origin.  Returns 0 on success, -1 if the
 * URL isn't http(s) (e.g. a /mnt/... local path).  Local paths
 * deliberately fail to parse so callers can treat them as "not
 * subject to SOP" -- you loaded the file yourself, you trust it. */
static inline int origin_parse(const char *url, struct origin *out)
{
    struct url u;
    if (!url || !out) return -1;
    if (url_parse(url, &u) != 0) return -1;
    out->scheme = u.scheme;
    out->port   = u.port;
    /* Hostnames are case-insensitive per RFC; fold to lower so
     * "Example.COM" and "example.com" compare equal. */
    size_t i = 0;
    for (; u.host[i] && i + 1 < sizeof(out->host); i++)
        out->host[i] = origin_tolower(u.host[i]);
    out->host[i] = '\0';
    return 0;
}

/* Returns 1 if both URLs share an origin (scheme + host + port
 * all equal), 0 otherwise.  If either URL fails to parse as
 * http(s), returns 0 -- "not the same origin" is the safe
 * default when one side is opaque. */
static inline int origin_eq(const char *url_a, const char *url_b)
{
    struct origin a, b;
    if (origin_parse(url_a, &a) != 0) return 0;
    if (origin_parse(url_b, &b) != 0) return 0;
    if (a.scheme != b.scheme) return 0;
    if (a.port   != b.port)   return 0;
    for (size_t i = 0; i < sizeof(a.host); i++) {
        if (a.host[i] != b.host[i]) return 0;
        if (a.host[i] == 0) break;
    }
    return 1;
}

/* Does `action` (the raw attribute string from <form action=...>)
 * look like an author-declared absolute URL?  Returns 1 for
 * "http://...", "https://...", or "//host..." (protocol-relative
 * with an explicit authority).  Returns 0 for everything else
 * (relative paths, fragment-only, query-only, empty string).
 *
 * This is the "author intent" half of the SOP block rule: a
 * cross-origin destination is OK *only* if the author wrote it
 * that way, on purpose, in the markup.  A relative path that
 * silently resolves cross-origin (think <base href> tricks, or
 * a chapter-111 JS mutation of the action attribute) is the
 * thing we block. */
static inline int origin_action_is_absolute(const char *action)
{
    if (!action || !action[0]) return 0;
    /* http:// or https:// (case-insensitive scheme) */
    if ((action[0] == 'h' || action[0] == 'H') &&
        (action[1] == 't' || action[1] == 'T') &&
        (action[2] == 't' || action[2] == 'T') &&
        (action[3] == 'p' || action[3] == 'P')) {
        if (action[4] == ':' && action[5] == '/' && action[6] == '/') return 1;
        if ((action[4] == 's' || action[4] == 'S') &&
            action[5] == ':' && action[6] == '/' && action[7] == '/') return 1;
    }
    /* "//host/path" protocol-relative. */
    if (action[0] == '/' && action[1] == '/') return 1;
    return 0;
}

#endif /* OSDEV_LIBC_ORIGIN_H */
