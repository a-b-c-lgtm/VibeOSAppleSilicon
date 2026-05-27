#!/usr/bin/env python3
"""
scripts/https_proxy.py — host-side HTTP→HTTPS bridge for /bin/browser.

Why this exists
---------------
Our userspace HTTP client is plaintext only.  Adding TLS in-kernel
(or in userspace) is a real engineering project — RSA, AES-GCM,
certificate-chain validation, the works — and it's parked behind
a later chapter.  Until then, this script lets the OS hit any HTTPS site by
running a tiny reverse proxy on the *host*: the guest speaks HTTP
to the host, the host speaks HTTPS to the world.

How it works
------------
QEMU's user-mode networking exposes the host at the magic IP
10.0.2.2.  Run this script on the host:

    python3 scripts/https_proxy.py        # listens on 0.0.0.0:8080

Then inside the guest:

    /$ browser http://10.0.2.2:8080/news.ycombinator.com/
    /$ browser http://10.0.2.2:8080/example.com/
    /$ browser --gui http://10.0.2.2:8080/info.cern.ch/

The first path segment is the upstream host; everything after it
is the upstream path.  We translate:

    http://10.0.2.2:8080/news.ycombinator.com/news     ->
        https://news.ycombinator.com/news

We rewrite href= and src= attributes inside text/html responses so
that internal links keep flowing through the proxy.  Stylesheets
referenced via <link rel=stylesheet href="news.css"> are caught by
the same rewrite and end up as
    href="/news.ycombinator.com/news.css"
which the browser then resolves against the page URL via
resolve_url() — so they hit the proxy too.

Caveats
-------
- Only GET is implemented.  No POST / forms.
- No cookies, no auth, no caching, no compression.
- Some sites (Cloudflare, Google) refuse non-browser User-Agent
  strings.  We send a Firefox-shaped UA to keep most things happy.
- The rewrite is a regex on raw bytes — works for typical pages,
  blows up on weird quoting.  This is a debug tool, not Tor.

Stop with Ctrl-C.
"""
import http.server
import re
import socket
import socketserver
import sys
import urllib.error
import urllib.request

LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 8080
USER_AGENT  = (
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:123.0) "
    "Gecko/20100101 Firefox/123.0"
)
TIMEOUT_SEC = 15
MAX_BODY    = 4 * 1024 * 1024     # 4 MiB cap per response

# Match href="..." | href='...' | href=foo (unquoted).  Same for src=.
ATTR_RE = re.compile(
    rb'(\s(?:href|src|action)\s*=\s*)'
    rb'(?:"([^"]*)"|\'([^\']*)\'|([^\s>]+))',
    re.IGNORECASE,
)

# CSS url(...) and @import "..."/url(...).  We capture the URL
# portion only; the surrounding syntax is preserved on output.
CSS_URL_RE = re.compile(
    rb'url\(\s*(?:"([^"]*)"|\'([^\']*)\'|([^)\s]+))\s*\)',
    re.IGNORECASE,
)
CSS_IMPORT_RE = re.compile(
    rb'@import\s+(?:"([^"]*)"|\'([^\']*)\')',
    re.IGNORECASE,
)


def rewrite_url(raw: bytes, upstream_host: bytes,
                upstream_dir: bytes) -> bytes:
    """Rewrite a URL value so it stays inside the proxy.

    - Absolute https://OTHER/...  ->  /OTHER/...
    - Absolute http://OTHER/...   ->  /OTHER/...
    - Protocol-relative //OTHER/  ->  /OTHER/
    - Root-relative   /path       ->  /<upstream_host>/path
    - Bare relative   path        ->  /<upstream_host>/<dir>/path
      (so the in-guest browser doesn't have to know the upstream
       host; HN's `<link href="news.css?xxx">` becomes
       `/news.ycombinator.com/news.css?xxx`).
    - Anchors / mailto / javascript: -> unchanged
    """
    s = raw.strip()
    if not s:
        return raw
    # Anchor-only links etc. — leave as-is.
    if s[:1] in (b"#",):
        return raw
    low = s.lower()
    if low.startswith(b"mailto:") or low.startswith(b"javascript:") \
       or low.startswith(b"data:"):
        return raw
    if low.startswith(b"https://"):
        return b"/" + s[len(b"https://"):]
    if low.startswith(b"http://"):
        return b"/" + s[len(b"http://"):]
    if s.startswith(b"//"):
        return b"/" + s[2:]
    if s.startswith(b"/"):
        return b"/" + upstream_host + s
    # Bare relative.  Glue it onto the upstream directory so the
    # browser can resolve it against the proxy-shaped page URL
    # (which has no useful path of its own).
    return b"/" + upstream_host + upstream_dir + s


def rewrite_html(body: bytes, upstream_host: bytes,
                  upstream_dir: bytes) -> bytes:
    """Rewrite href/src/action attributes in an HTML body."""
    def sub(m):
        prefix = m.group(1)
        # exactly one of groups 2,3,4 is non-None
        url = m.group(2) or m.group(3) or m.group(4) or b""
        new = rewrite_url(url, upstream_host, upstream_dir)
        # Re-emit with double quotes for safety.
        return prefix + b'"' + new + b'"'
    return ATTR_RE.sub(sub, body)


def rewrite_css(body: bytes, upstream_host: bytes,
                 upstream_dir: bytes) -> bytes:
    """Rewrite url(...) and @import inside a CSS body so embedded
    references (background images, fonts, nested stylesheets) keep
    routing through the proxy.  Generic — not site-specific."""
    def url_sub(m):
        url = m.group(1) or m.group(2) or m.group(3) or b""
        new = rewrite_url(url, upstream_host, upstream_dir)
        return b'url("' + new + b'")'
    def import_sub(m):
        url = m.group(1) or m.group(2) or b""
        new = rewrite_url(url, upstream_host, upstream_dir)
        return b'@import "' + new + b'"'
    body = CSS_URL_RE.sub(url_sub, body)
    body = CSS_IMPORT_RE.sub(import_sub, body)
    return body


class Handler(http.server.BaseHTTPRequestHandler):
    # Use HTTP/1.0 so we don't have to deal with chunked encoding on
    # the response side.
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("[proxy] %s\n" % (fmt % args))

    def do_GET(self):
        # Path looks like "/<host>/<rest...>" — split off the host.
        p = self.path.lstrip("/")
        if not p or "/" not in p + "/":
            self.send_error(400, "expected /<host>/<path>")
            return
        slash = p.find("/")
        if slash < 0:
            host = p
            rest = "/"
        else:
            host = p[:slash]
            rest = p[slash:] or "/"
        if not host:
            self.send_error(400, "missing host segment")
            return

        upstream = "https://" + host + rest
        self.log_message("GET %s -> %s", self.path, upstream)

        req = urllib.request.Request(upstream, headers={
            "User-Agent":      USER_AGENT,
            "Accept":          "*/*",
            "Accept-Encoding": "identity",      # no gzip
        })
        try:
            r = urllib.request.urlopen(req, timeout=TIMEOUT_SEC)
        except urllib.error.HTTPError as e:
            body = (e.read() or b"")[:MAX_BODY]
            self.send_response(e.code)
            self.send_header("Content-Type",
                             e.headers.get("Content-Type", "text/plain"))
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        except (urllib.error.URLError, socket.timeout, OSError) as e:
            self.send_error(502, "upstream error: %s" % e)
            return

        body = r.read(MAX_BODY + 1)
        if len(body) > MAX_BODY:
            self.log_message("upstream body > %d bytes; truncating", MAX_BODY)
            body = body[:MAX_BODY]

        ctype = r.headers.get("Content-Type", "application/octet-stream")
        # Strip charset for the rewrite check.
        ctype_main = ctype.split(";", 1)[0].strip().lower()

        # Rewrite hrefs/srcs in HTML so internal navigation stays in-proxy.
        if ctype_main in ("text/html", "application/xhtml+xml"):
            # Compute the upstream path's directory (everything up
            # to and including the last '/' in the request path,
            # query string stripped).  Used to anchor bare-relative
            # rewrites like href="news.css?xxx".
            #
            #   /news.ycombinator.com/         -> dir = /
            #   /news.ycombinator.com/news     -> dir = /
            #   /news.ycombinator.com/a/b.html -> dir = /a/
            qmark = rest.find("?")
            path_only = rest if qmark < 0 else rest[:qmark]
            slash_pos = path_only.rfind("/")
            updir = path_only[:slash_pos + 1] if slash_pos >= 0 else "/"
            body = rewrite_html(body,
                                host.encode("ascii", "ignore"),
                                updir.encode("ascii", "ignore"))
        elif ctype_main == "text/css":
            qmark = rest.find("?")
            path_only = rest if qmark < 0 else rest[:qmark]
            slash_pos = path_only.rfind("/")
            updir = path_only[:slash_pos + 1] if slash_pos >= 0 else "/"
            body = rewrite_css(body,
                                host.encode("ascii", "ignore"),
                                updir.encode("ascii", "ignore"))

        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        # Tell the guest's parser it's a stream.  Connection: close
        # is implicit at HTTP/1.0.
        self.end_headers()
        self.wfile.write(body)


class ThreadedServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    port = LISTEN_PORT
    if len(sys.argv) >= 2:
        try: port = int(sys.argv[1])
        except ValueError: pass
    srv = ThreadedServer((LISTEN_HOST, port), Handler)
    sys.stderr.write(
        "[proxy] listening on http://%s:%d\n"
        "[proxy] in the guest:  browser http://10.0.2.2:%d/<host>/<path>\n"
        "[proxy] e.g.:          browser http://10.0.2.2:%d/news.ycombinator.com/\n"
        % (LISTEN_HOST, port, port, port)
    )
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        sys.stderr.write("\n[proxy] shutting down\n")
        srv.server_close()


if __name__ == "__main__":
    main()
