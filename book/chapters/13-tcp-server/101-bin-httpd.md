# Chapter 101 — /bin/httpd: serve /mnt and /data over HTTP

**Status:** Stub. Tracking milestone 94.

The first end-user-visible TCP-server program. It listens
on port 8080, serves files out of `/mnt/` and `/data/`,
sets reasonable Content-Types, and handles only what's
needed.

## What this chapter adds

- `userspace/httpd/httpd.c` — single-threaded accept loop,
  one in-flight request at a time (multi-thread comes when
  pthreads land at chapter 90 — until then this is fine).
- HTTP/1.0 with `Connection: close`.
- Content-Type table for `.html`, `.css`, `.txt`, `.png`,
  `.jpg`, `.svg`.
- Range requests: out of scope.
- Listed as a service to auto-start at boot (or by the
  launcher).

## Prerequisites

- Chapter 100 — accept
- Chapter 66 — URL/HTTP parser (request parser is the same
  shape, easier to lift)
- Chapter 16 — VFS (we read from `/mnt/`)

## Plan

- 200/404/500 responses; everything else returns 501 Not
  Implemented.
- Logging to console: `[httpd] GET /mnt/foo.html → 200`.
- Path safety: reject `..` segments hard.

## What you'll learn

- How tiny a usable HTTP server can be.
- The minimum viable subset of HTTP/1.0 for static-file
  serving.
- The pleasant feeling of running both client and server on
  the same kernel.

## What this unlocks

- The end-to-end loop chapter (102).
- A baseline for any future "expose this via HTTP"
  feature (devmode dashboard, etc).
