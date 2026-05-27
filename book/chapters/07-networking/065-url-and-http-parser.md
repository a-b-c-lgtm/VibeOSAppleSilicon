# Chapter 65 — URL and HTTP parser

[Chapter 64](064-dns-resolver.md) finished the network plumbing:
the kernel resolves names via SLIRP's DNS server, and `httpget`
can already speak `httpget example.com 80 /`. That command-line
form is fine for tests but it is not what a browser wants to
type. A browser wants URLs — `http://example.com/index.html` —
and it wants an HTTP response that is split into a status, a
header table, and a decoded body, not a raw byte stream.

This chapter ports the last piece of plumbing the browser
needs:

- A header-only **URL parser**, `userspace/libc/url.h`, that
  splits an absolute `http://` or `https://` URL into scheme,
  host, port, path-with-query, and fragment.
- A header-only **HTTP/1.1 response parser**,
  `userspace/libc/http.h`, that walks the status line, lowers
  header names in place for cheap lookups, and decodes the
  body for the three common framings (Content-Length,
  Transfer-Encoding: chunked, and Connection: close).
- A new **URL-form** for `/bin/httpget`: `httpget <url>`. The
  legacy three-argument form is preserved verbatim so the
  earlier socket tests keep grepping the same byte stream.
- One redirect hop (HTTP 3xx + Location:) chased
  iteratively, capped at one to prove the mechanism without
  inviting infinite-loop bait.

Two things this chapter does **not** do:

- TLS. `https://` URLs are recognised, then politely refused.
  We will revisit TLS in a much later chapter — it needs
  either a port of mbedTLS or a host-side bridge, and neither
  is on the critical path to a working browser over plaintext
  HTTP. The intent statement is in `httpget.c`:
  *"TLS is parked behind a future milestone."*
- HTTP/2 or HTTP/3. We are aiming at the simplest text-mode
  browser that can fetch HTML over HTTP/1.x; modern multiplexed
  protocols are out of scope for the book.

Two things that *did* fall out of this chapter as side bugs,
and are documented in detail below:

- A **TCP connection-table leak** that capped us at four
  open sockets per boot — fine for a single `curl`, fatal as
  soon as a redirect chain or a back-to-back fetch happened.
- A **panic-handler recursion** that erased the original
  fault information whenever `SP_EL1` itself was the thing
  that went wrong. The fix is a 4-KiB emergency stack the
  vector code switches to before doing any saving.

## Why parse URLs in userspace

There is a recurring temptation in hobby kernels to push every
new protocol into the kernel because *the syscalls are right
there*. A URL parser would be five hundred lines of `strncmp`
and a handful of bounds checks. We could absolutely jam it
behind `SYS_PARSE_URL` and call it a day.

We don't, for two reasons.

First, the URL parser has no privilege requirement. It does
not touch hardware, does not need an address space, does not
need to hold a lock against any other process. Putting it in
the kernel would mean a syscall round-trip per `url_parse()`
call (we'll do many during page load — every `<a href="...">`
gets one) for zero capability gain. The most expensive thing
we'd be saving the userspace caller from doing is decoding a
small struct on its own stack. That is an anti-optimisation.

Second, the kernel's syscall ABI is the place where backwards
compatibility hurts the most. Every header file we add to
`userspace/libc/` is a thing we can change in a single
commit; every `SYS_*` number we hand out is a thing every
binary on disk now depends on. A URL/HTTP parser is exactly
the kind of thing that gains features as the browser grows
("oh, we need to support `mailto:` after all", "oh, we need
to fold whitespace in chunked extension fields") and we want
to be able to evolve it without an ABI bump.

So url.h and http.h follow the same single-translation-unit
convention as `printf.h` and `malloc.h`: include from one
`.c` per binary, no link step needed. It keeps the libc
philosophy consistent (header-only, no global state, no
allocator dependencies that aren't already in the program)
and it means each binary inlines exactly the parser
fragments it actually uses.

## The URL parser

`url.h` accepts the subset of RFC 3986 the browser will
actually emit: absolute URLs with the form

```
scheme "://" host [ ":" port ] [ path ] [ "?" query ] [ "#" fragment ]
```

with `scheme` restricted to `http` or `https` (case-insensitive
per RFC), `host` being either a DNS name or a dotted-quad IPv4
address, and bracketed-IPv6 hosts (`[::1]`) explicitly rejected
because the rest of the network stack is IPv4-only.

The result type is plain enough to print at a glance:

```c
struct url {
    int      scheme;        /* URL_SCHEME_HTTP or URL_SCHEME_HTTPS */
    uint16_t port;          /* 80 or 443 if URL omitted ":port" */
    char     host[URL_HOST_MAX];   /* DNS or dotted-quad, NUL-terminated */
    char     path[URL_PATH_MAX];   /* "/" + path + "?" + query */
    char     frag[URL_FRAG_MAX];   /* fragment without the '#' */
};
```

Two design choices worth calling out:

- **Path and query are joined.** The HTTP/1.1 request-line is
  `GET <path-and-query> HTTP/1.1\r\n`; almost no client wants
  these fields apart. Storing them already-joined removes a
  printf call from the hot path (every fetch) and removes a
  way for the caller to forget the `?`. The fragment, by
  contrast, is *never* sent on the wire — the browser uses
  it only to scroll to an anchor — so we split it out.
- **Caller-owned, fixed-size buffers.** No allocation in the
  parser. The buffers live in `struct url`, which the caller
  puts on its stack or in its heap. This matches the rest of
  our libc (we have `malloc()`, but its uses are explicit and
  auditable) and means a malformed URL can never cause a
  partial allocation that the caller has to remember to free.

The parser itself is a small state machine: chew off `http://`
or `https://`, then host (rejecting `@` for userinfo and `[`
for IPv6), then optional `:port` with a 65535 cap, then
either a real path or a synthesised `"/"`, then optional
fragment. The whole thing is ~150 lines of straightforward
parsing with one helper (`url_copy_n()`) that copies a slice
into a fixed buffer and rejects truncation as a hard error.
We treat truncation as fatal because a path that doesn't fit
in 1 KiB is overwhelmingly more likely to be a bug or an
attack than a real request, and a browser would refuse it
anyway.

## The HTTP/1.1 response parser

`http.h` is the larger of the two headers (~310 lines) but it
is the same shape: a struct, an in-place state machine, no
allocation.

```c
struct http_response {
    int          status;            /* 200, 302, 404, ... */
    int          minor_version;     /* 0 or 1 */
    const char  *reason;            /* "OK", "Not Found", ... */
    size_t       reason_len;
    struct http_header headers[HTTP_HEADERS_MAX];   /* HTTP_HEADERS_MAX = 64 */
    size_t       header_count;
    char        *body;
    size_t       body_len;
};
```

The contract is:

1. Caller drains the response from the socket into a
   contiguous buffer. `httpget` does this with a doubling
   `malloc`-grown buffer (`drain_to_buf()`) until `read()`
   returns 0.
2. Caller passes that buffer (writable, since we're going to
   in-place lower-case header names) and its length to
   `http_parse(buf, n, &resp)`.
3. On success, every slice in `resp` points into `buf`.
   `buf` must outlive `resp`.

Two implementation details earn their keep here.

**In-place lowercasing of header names.** RFC 7230 says HTTP
headers are case-insensitive. The naive way to look one up is
`strncasecmp` per comparison, which is a function call and a
character-by-character lower-then-compare on every byte. Our
parser lowers each header name as it walks past, in place —
the byte that was `'C'` in `Content-Type` is `'c'` after the
parser returns. Lookup by name then becomes a plain `memcmp`
against an already-lowercase literal string. The lookup
helper `http_get_header(r, "content-type", &len)` is the only
place callers ever interact with this convention.

**Chunked decoding in place.** A chunked body interleaves
hex-prefixed chunk sizes with payload, separated by CRLF.
Decoding it is a classic two-cursor copy: a read cursor walks
the original buffer, parses each `<hex>\r\n`, copies that
many bytes down to the write cursor, skips the trailing
CRLF, and stops at the zero-length chunk. Because the
write cursor is always at or behind the read cursor, no
allocation is needed and the body slice the parser hands
back is contiguous. We swallow trailers (the optional
header-like lines after the zero chunk) without inspecting
them.

The body framing decision is the only branchy part of the
parser:

```text
if Transfer-Encoding contains "chunked":
    decode chunked, body_len := decoded length
else if Content-Length: N is set:
    body_len := min(N, bytes-after-headers)
else:
    body_len := bytes-after-headers      // Connection: close framing
```

The "Connection: close" branch is the one that lets the
parser work with the pre-existing `httpget` flow that drains
the socket to EOF before parsing. If we later move to a
streaming parser (read partial response, parse headers,
incrementally consume body) we keep this same decision tree,
just done online.

## The URL form of `/bin/httpget`

The new entry point is small enough to read end-to-end:

```c
static int fetch_one(const char *raw_url, char **out_redirect)
{
    struct url u;
    if (url_parse(raw_url, &u) < 0) { /* error */ return -1; }
    if (url_is_tls(&u))              { /* refuse */ return -1; }

    uint32_t ip_be = 0;
    if (parse_dotted(u.host, &ip_be) < 0)
        if (resolve(u.host, &ip_be) < 0) return -1;

    int fd = socket_connect(ip_be, u.port);
    if (fd < 0) return -1;

    /* build "GET <path> HTTP/1.1\r\nHost: ...\r\n..." in heap buf */
    /* write to fd, drain to malloc'd buf, http_parse() */

    /* print "[httpget] HTTP/1.x NNN reason (content-type, body=N)" */
    /* if 3xx with Location: capture it as *out_redirect */
    /* dump body */

    return resp.status;
}
```

The `out_redirect` channel is how `fetch_one` tells the
caller "you might want to fetch this next." `fetch_url`,
the public wrapper, runs at most two iterations: original URL,
then one redirect. We cap at one because:

- It is enough to demonstrate the mechanism end-to-end and
  to satisfy real sites that 301-redirect HTTP to HTTPS (we
  print the redirected URL even though we won't follow it
  into TLS).
- An iterative loop with no upper bound is a *gift* to a
  pathological server. A browser wants a small finite cap
  (Chrome: 20). We are choosing a smaller one because we are
  primarily testing the chase mechanism, not building
  redirect-chain compatibility.
- It keeps the code straight-line. Each hop frees the
  previous hop's URL copy before allocating the next.

The legacy form is preserved verbatim:

```text
httpget <host-or-ip> <port> [path]      # legacy form
```

so the existing socket tests (`test_httpget.py`, `test_dns.py`,
`test_httpget_dns.py`) keep working without change. Argument
count picks the form — `argc == 2` is a URL, `argc >= 3` is the
legacy form.

## A user-stack discipline note

`fetch_one` was the first function in the project where we hit
a real userspace stack-pressure problem. The natural way to
write it is to put `struct url u` (≈ 1.5 KiB) and
`struct http_response resp` (≈ 2 KiB) on the local stack,
plus a 1.5 KiB request buffer, plus the ~1.4 KiB pointer to
the captured redirect target. Add the saved-x29/x30 chain and
the userspace stack frame for one fetch creeps past 7 KiB.
Our user threads run on 16 KiB stacks (see
`THREAD_STACK_SIZE` in [Chapter 10](../03-time-and-concurrency/010-threads-and-context-switch.md));
chaining two fetches plus an SVC and an IRQ frame eats
non-trivially into that.

The fix in `fetch_one` is to malloc each large local rather
than spilling it on the stack: `req`, `resp`, the captured
`next` redirect target. The `struct url` stays on the stack
(it lives for the whole function and we only have one), but
everything bigger gets a pointer. The header to take away is:
*if a function can be called recursively-like (here, via
the redirect loop), and any of its locals exceed 1 KiB, those
locals belong on the heap*. We will reuse this discipline in
the HTML/CSS/layout chapters — DOM nodes, style rules, line
boxes are all heap-allocated for exactly the same reason.

## Bug #1 — the four-connection TCP cap

On the very first three-fetch test
(`scripts/test_m58_repeat.py`, three back-to-back
`httpget` invocations against a tiny local server), the
second or third fetch would hang for several seconds and
then fail to connect.

The smoking gun was in `kernel/core/tcp.c`:

```c
#define TCP_CONN_CAP   4
static struct tcp_conn g_conns[TCP_CONN_CAP];
```

We had four TCP control blocks for the entire system. They
were allocated carefully and freed in `socket_close`,
but the DNS-resolver boot self-test plus the TCP boot HTTP self-test
already consumed two before init even ran. A single fetch
took one. A redirect chase took two simultaneously (because
we close *after* parsing, not before issuing the next
connect). A three-test loop ran us out before anything
dramatic could happen.

The fix is a one-line change:

```c
#define TCP_CONN_CAP  16
```

Sixteen is enough to comfortably overlap a half-dozen page
loads and still have headroom for the boot self-tests.
Properly fixing it would mean moving the table to a heap
allocation and growing on demand, but our use case is "at
most a handful of in-flight TCP connections" — the static
table is the simplest thing that works and is easy to bump
again when needed. The chapter's lesson here is the
diagnostic one: **a leak in a fixed-capacity resource pool
looks exactly like a hang under repeated use.** Always
inspect free-pool counts before assuming a network-side
problem.

## Bug #2 — panic-handler recursion

The far more interesting bug was a kernel panic that started
appearing intermittently after a successful httpget run. The
fault address was *suspicious*:

```
ESR_EL1  = 0x96000045  (Data Abort, EL1)
FAR_EL1  = 0x240000020
ELR_EL1  = ...something inside svc_entry...
```

`0x240000000` is the top of mapped RAM in our project (eight
1-GiB blocks of normal memory, see
[Chapter 7](../02-memory/007-physical-memory-and-device-tree.md)).
The faulting address is *just past the last mapped page*.
That means whatever instruction faulted was not chasing a
bad pointer — it was indexing off the end of `SP_EL1` itself.

The instruction was, predictably,
`ldr x30, [sp, #240]` inside the kernel's exception entry
preamble: the bit that restores the saved x30 register from
the saved-context frame on the way out of an SVC. If `SP_EL1`
is one or two cache lines before `0x240000000`, then `sp+240`
walks straight off the end of mapped RAM.

The first interesting thing about this panic was that *the
register dump was lying to us*. The values shown for x0, x1,
… x30 weren't from the original fault. They were from the
panic handler's own re-entry. Because the C panic printer
itself runs on the kernel stack, when the original fault
left `SP_EL1` already broken, `panic_entry` ran
`save_context` to spill registers — and spilled them into
the same broken stack. The fault recursed inside the panic
path, then again, until something stuck and we printed
whatever happened to be in the registers at the *third*
entry to the vector.

The fix lives in `kernel/arch/vectors.S`:

```asm
.balign 8
panic_entry:
    /* Switch to a dedicated emergency stack BEFORE save_context. */
    msr     spsel, #1
    mov     x18, sp                       /* preserve original SP_EL1 */
    adrp    x16, panic_emerg_stack_top
    add     x16, x16, :lo12:panic_emerg_stack_top
    mov     sp, x16

    mov     x19, x0                       /* preserve vector ID */
    save_context                          /* now safe; spills to scratch */
    mov     x0, x19
    mov     x1, sp
    bl      kernel_panic_from_vector

.section .bss.panic_emerg
.balign 16
panic_emerg_stack:
    .skip 4096
.global panic_emerg_stack_top
panic_emerg_stack_top:
.section .text.vectors
```

The panic handler now switches to a 4-KiB stack reserved
exclusively for it, in a dedicated `.bss.panic_emerg` section
of the kernel image. Because that stack lives within the
kernel image's BSS region (well below `0x240000000`), no
fault while spilling registers can reach it. The original
`SP_EL1` is preserved in `x18` so the dump still tells the
truth about what the kernel was using when it died.

This is one of those fixes that costs nothing on the happy
path (one extra `mov`, one extra `msr`) and costs a chapter
on the unhappy path. **Lesson: every exception handler that
might be entered with a bad stack must switch stacks before
it does anything else.** The general AArch64 idiom for this
is `SP_EL1` for kernel work, `SP_EL0` (or a dedicated
emergency `.bss` region) for the panic path; we picked the
emergency-section approach because we don't want the panic
to compete with userspace for SP_EL0 either.

After the emergency stack landed, the original panic
became impossible to reproduce. We don't yet know whether
the underlying fault was a code bug we accidentally fixed
during the diagnostic blizzard, or a timing-sensitive race
that the new instrumentation simply hides. Either way, the
emergency-stack defence is permanent: even if a similar
fault returns, the next panic dump will be honest.

## Bug #3 — heap-bottom matters

A more pedestrian bug: the kernel heap allocator was searching
for free blocks from the *highest* address downward. This chapter
exposed it because once httpget ran, we had a string of small
allocations (URL struct, request buffer, response struct,
chunked-decode work) that were all asking for blocks one to
four KiB in size. Allocating from the high end fragmented the
heap quickly: each allocation cleaved a fresh 4-KiB slab off
the top of the free region, and the free list grew but never
condensed.

The fix is a one-line change in `kernel/core/heap.c` to walk
the free list from the lowest address up. Coalescing then
works because adjacent freed blocks are close together in the
list, and the heap reaches steady state after a few hundred
allocations instead of growing without bound.

This is a file-it-and-forget bug, but it is worth recording
as a heuristic: **any first-fit allocator should walk
low-to-high.** The high-to-low walk is fine for stacks (LIFO
is exactly what stacks want) but pessimal for malloc-shaped
workloads where many small allocations have varied lifetimes.

## What it looks like

A clean run with `scripts/test_m58_repeat.py` against a tiny
local HTTP server (`127.0.0.1:8889`, returning
`M58-PLAIN-MARKER\n` for `/m58`):

```text
/$ httpget http://10.0.2.2:8889/m58
[httpget] HTTP/1.0 200 OK (text/plain, body=17 bytes)
M58-PLAIN-MARKER
[sys_exit] thread '/bin/httpget' exited with code 0x00000000
/$ httpget http://10.0.2.2:8889/m58
[httpget] HTTP/1.0 200 OK (text/plain, body=17 bytes)
M58-PLAIN-MARKER
[sys_exit] thread '/bin/httpget' exited with code 0x00000000
/$ httpget http://10.0.2.2:8889/m58
[httpget] HTTP/1.0 200 OK (text/plain, body=17 bytes)
M58-PLAIN-MARKER
[sys_exit] thread '/bin/httpget' exited with code 0x00000000
```

A redirect-chasing run against a server that 302s `/old` to
`/new`:

```text
/$ httpget http://10.0.2.2:8889/old
[httpget] HTTP/1.1 302 Found (text/html, body=0 bytes)
[httpget] following redirect -> http://10.0.2.2:8889/new
[httpget] HTTP/1.1 200 OK (text/plain, body=4 bytes)
new
```

A chunked-encoding run against a server that emits
`Transfer-Encoding: chunked` with a few small chunks:

```text
/$ httpget http://10.0.2.2:8889/chunked
[httpget] HTTP/1.1 200 OK (text/plain, body=12 bytes)
chunkedchunk
```

And the polite refusal of HTTPS:

```text
/$ httpget https://example.com/
httpget: https:// is not yet supported in this build.
        TLS is parked behind a future chapter.
```

## What's next

The browser begins in [Chapter 66](../08-browser/066-html-tokenizer.md):
the HTML tokenizer. The contract from this chapter's side is clean:
the browser will get bytes-and-content-type from `httpget`'s
parser path (or its successor — we'll likely fold these
helpers into `/bin/browser` directly so the browser doesn't
shell out), and the tokenizer will turn those bytes into a
stream of start-tags, end-tags, attributes, comments, doctypes,
and character data. From there it is parser → DOM → CSS →
layout → paint → render to a window. We have every
underlying piece — sockets, names, malloc, GUI, fonts, the
window manager — and the next six chapters are about
stitching them together into something that resembles a
browser.

The wire-up of URL parser, HTTP parser, and fetch helper
will reappear almost verbatim inside the browser binary,
just rebound to a streaming reader instead of drain-to-EOF.
That is the real reason this is its own chapter: the chapters that
follow assume URLs and HTTP responses are *cheap*, and that
assumption is only true because url.h and http.h are small,
allocation-free, header-only, and don't cross any syscall.
