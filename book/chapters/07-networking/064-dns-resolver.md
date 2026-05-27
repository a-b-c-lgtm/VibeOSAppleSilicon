# Chapter 64 — DNS resolver

[Chapter 63](063-socket-syscalls-and-httpget.md) wired sockets
into userspace, but `httpget` could only talk to literal IPv4
addresses: `httpget 10.0.2.2 8888 /`. Real network programs
take *names*. Before we can build a browser we need the
ability to ask "what's the IP for `example.com`?" and get an
answer. That's what this chapter adds.

By the end of this chapter:

- The kernel captures the DNS server address from the DHCP
  OFFER (option 6) and stores it alongside the IP, gateway,
  and netmask.
- A new module `kernel/core/dns.{c,h}` builds a one-shot DNS
  query, sends it over UDP/53, parses the reply (including
  RFC-1035 name compression on the wire), and returns the
  first A-record IPv4 it finds.
- A new syscall `SYS_RESOLVE = 63` exposes that capability
  to userspace via a thin libc wrapper `resolve()`.
- `httpget` falls back to `resolve()` when its first argument
  isn't a dotted quad — `httpget example.com 80 /` now works.
- Two harnesses, `scripts/test_dns.py` (kernel-side) and
  `scripts/test_httpget_dns.py` (userspace-side), prove the
  whole path round-trips against SLIRP's built-in DNS server.

## Why DNS, and why now

The browser we're building will consume URLs of the form
`http://host/path`. The host is almost never a dotted quad —
it's a name. Until now we've side-stepped that by pinning
addresses (`10.0.2.2` for the host machine, `10.0.2.3` for
SLIRP's DNS endpoint), but the moment we want to fetch
content from a real site, we need a resolver. Without one,
every HTTP request would have to be paired with a manually
looked-up IP. That's not a browser, that's `nc`.

DNS is a small protocol — the query and reply each fit in one
UDP packet for the common case. The wire format is from
RFC 1035 and is unchanged since 1987. The only mildly tricky
piece is *name compression*: a name in an answer can be a
back-pointer into the question section. We handle it
correctly without ever actually following the pointer (we
only need to skip past names, not parse them).

## SLIRP's DNS path

QEMU's user-mode networking stack (SLIRP) provides a built-in
DNS server at `10.0.2.3` that forwards queries to the host's
resolver. So when our guest sends a UDP/53 packet to
`10.0.2.3`, SLIRP intercepts it, looks up the name on the
host (typically through `/etc/resolv.conf` → systemd-resolved
or `mDNSResponder`), and constructs a reply.

Crucially, the DHCP OFFER from SLIRP includes option 6
(Domain Name Server) pointing at `10.0.2.3`. Until this
chapter our DHCP parser ignored that option — we extracted
the IP, gateway, lease time, and netmask, and threw away the
rest. The first piece of work was to harvest option 6
into a new global, `g_dns`, in `kernel/core/net.c`.

## DHCP option 6 capture

In `kernel/core/dhcp.c`, the option parser already knows the
generic TLV walking pattern. We added one new case:

```c
case DHCP_OPT_DNS:        /* code 6 */
    if (len >= 4 && !g_have_dns) {
        d_memcpy(g_offered_dns, p, 4);
        g_have_dns = 1;
    }
    break;
```

Then, after the lease is finalised and `net_set_ipv4_config`
has installed the IP/gateway/mask, we call:

```c
if (g_have_dns) net_set_dns(g_offered_dns);
```

`net_set_dns` lives in `kernel/core/net.c`:

```c
void net_set_dns(const uint8_t dns_ip[NET_IPV4_LEN])
{
    n_memcpy(g_dns, dns_ip, NET_IPV4_LEN);
    serial_puts("[net] dns=");
    print_ipv4(g_dns);
    serial_puts("\n");
}
```

Booting the kernel now logs:

```
[net] up: ip=10.0.2.15 gw=10.0.2.2 mask=255.255.255.0
[net] dns=10.0.2.3
```

That `[net] dns=` line is the test harness's signal that
option 6 was captured. Without it, `dns_resolve()` would
have nowhere to send queries.

## DNS wire format, just enough

A DNS message has a 12-byte fixed header followed by four
variable sections — questions, answers, authorities, and
additionals. The header layout (network byte order
throughout):

```
  +---------------------+
  |  ID (2)             |   transaction ID — match req/resp
  +---------------------+
  |  Flags (2)          |   QR | Opcode | AA | TC | RD | RA | Z | RCODE
  +---------------------+
  |  QDCOUNT (2)        |   number of questions
  +---------------------+
  |  ANCOUNT (2)        |   number of answers
  +---------------------+
  |  NSCOUNT (2)        |   number of authority RRs
  +---------------------+
  |  ARCOUNT (2)        |   number of additional RRs
  +---------------------+
```

A *name* is a sequence of length-prefixed labels terminated
by a zero byte: `example.com` becomes `\x07example\x03com\x00`.
Each label can be at most 63 bytes; the whole name at most
255 octets including length bytes.

A *question* is `<name> <QTYPE:2> <QCLASS:2>`. We only ever
ask for QTYPE=A (1) and QCLASS=IN (1).

An *answer* (resource record) is
`<name> <TYPE:2> <CLASS:2> <TTL:4> <RDLENGTH:2> <RDATA>`. For
an A record, `RDLENGTH` is 4 and `RDATA` is the 4-byte IPv4
address.

The wrinkle: in answers the `<name>` is almost always
*compressed* — a 2-byte sequence where the top two bits are
`11` and the remaining 14 bits are an offset into the message
to the actual name. This saves bytes in the common case where
the answer name matches the question name.

We don't *need* to expand compressed names; we just need to
*skip past* them safely. Our skip routine reads one byte:

- If the high two bits are `11`, it's a 2-byte pointer →
  advance by 2 and stop (we never follow).
- If the byte is zero, the name is over → advance by 1 and
  stop.
- Otherwise it's a label-length byte → advance by `1 + len`
  and continue.

A hop counter prevents pathological inputs from looping.

## `dns_resolve(name, out_ip)`

The synchronous resolver in `kernel/core/dns.c` is small. It:

1. Refuses bare IPs and empty strings (the libc layer will
   handle dotted-quad parsing; the kernel resolver assumes a
   real name).
2. Picks a 16-bit transaction ID and a high ephemeral source
   port (49152 + (cntvct mod 16384)).
3. Builds the query in a 512-byte stack buffer.
4. Calls `udp_bind(my_port, dns_rx)` so the UDP layer knows
   where to deliver the reply.
5. Fires the packet at the DNS server's UDP/53.
6. Spins, calling `net_poll()` every iteration, until the
   reply arrives or a 3-second budget expires.
7. Parses the answer section for the first A/IN record with
   `RDLENGTH == 4`, copies the four bytes into `out_ip`.
8. Calls `udp_bind(my_port, NULL)` to release the port.

The spin loop reuses the pattern we established in the previous chapter:
**`yield()` does not pump the NIC**. Any kernel code waiting
for a network event must call `net_poll()` itself.

We also avoid the kernel scheduler's coarse 100 ms tick by
computing the timeout against `cntvct_el0` directly:

```c
uint64_t freq;   __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
uint64_t start;  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
uint64_t budget = freq * 3;     /* 3 seconds */
while (1) {
    (void)net_poll();
    if (g_resp_len) break;
    uint64_t now;  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    if (now - start > budget) break;
}
```

This works on both the 24 MHz HVF timer and the 62.5 MHz
silicon timer — the comparison is in cycles, not ticks.

## Avoiding the `memset` trap, again

`dns.c` has a 512-byte response buffer and a few medium-sized
locals. Per [the recurring lesson](../../../book/INDEX.md),
GCC will emit calls to `memset`/`memcpy` for `={0}` initialisers
on large stack objects when it deems a runtime mem* call
cheaper than inline. In a freestanding kernel those symbols
don't exist, so the link fails.

The defensive pattern at the top of `dns.c`:

```c
static void *d_memset(void *dst, int c, uint64_t n) { ... }
static void *d_memcpy(void *dst, const void *src, uint64_t n) { ... }

__attribute__((used)) static void dns_static_init(void)
{
    /* Reference d_memset/d_memcpy so -ffunction-sections + LTO
     * doesn't drop them; if GCC emits a hidden memset call, the
     * linker can resolve it from this TU. */
    char buf[1] = { 0 };
    d_memset(buf, 0, 1);
    d_memcpy(buf, buf, 1);
}
```

We never call `memset` directly in this file — only `d_memset`.
That keeps the trap from firing.

## The `SYS_RESOLVE` syscall

Userspace exposure is one syscall:

```c
SYS_RESOLVE = 63,    /* (const char *name, uint32_t *out_ip4_be)
                       -> 0 / -errno */
```

The kernel handler copies up to 255 bytes of name from user
memory (one byte at a time, stopping at the NUL), calls
`dns_resolve()`, packs the four-byte IP into a single
big-endian `uint32_t`, and copies it back. The same packed
encoding is what `SYS_SOCKET_CONNECT` accepts, so the typical
sequence is:

```c
uint32_t ip_be;
if (resolve("example.com", &ip_be) == 0)
    fd = socket_connect(ip_be, 80);
```

The libc wrapper in `userspace/libc/syscall.h`:

```c
static inline int resolve(const char *name, uint32_t *out_ip4_be)
{
    return (int)_svc2(SYS_RESOLVE, (long)name, (long)out_ip4_be);
}
```

## `httpget` learns hostnames

The change to `httpget.c` is two lines of intent and a dozen
lines of plumbing. The previous code bailed on a non-dotted
input:

```c
if (parse_dotted(argv[1], &ip_be) < 0) {
    printf("httpget: invalid ip '%s'\n", argv[1]); return 1;
}
```

Now it falls through to `resolve()`:

```c
if (parse_dotted(argv[1], &ip_be) < 0) {
    int rc = resolve(argv[1], &ip_be);
    if (rc < 0) { printf("httpget: cannot resolve '%s' (%d)\n", argv[1], rc); return 1; }
    printf("httpget: resolved %s -> %u.%u.%u.%u\n", argv[1], ...);
}
```

That's it. From the shell:

```
/$ httpget example.com 80 /
httpget: resolved example.com -> 23.215.0.138
[httpget] connecting...
... (HTTP body, or a connection-refused error if the host
     blocks outbound port 80) ...
```

## Tests

[scripts/test_dns.py](../../../scripts/test_dns.py) checks
the kernel-side path:

1. Boot the kernel.
2. Confirm the `[net] dns=10.0.2.3` line appears (proves
   DHCP option 6 was captured).
3. Wait for the kernel's TCP self-test phase to finish so
   logs don't tangle.
4. Look for the new Phase-6 line `[net] self-test: DNS reply
   ip=A.B.C.D` and assert the address is a non-zero dotted
   quad.

[scripts/test_httpget_dns.py](../../../scripts/test_httpget_dns.py)
checks the userspace path:

1. Boot the kernel and wait for the kernel-side DNS phase to
   succeed.
2. Wait for the shell prompt.
3. Type `httpget example.com 80 /`.
4. Look for the `httpget: resolved example.com -> A.B.C.D`
   line that the userspace tool prints after `resolve()`
   returns successfully.

Both tests pass on every boot we've measured against SLIRP
under HVF on Apple Silicon. They will fail in offline CI; the
right reaction there is to skip them, not to "fix" the
resolver.

## Why not `LISTEN`/`ACCEPT` first?

The classic networking-chapter order is *active connect →
passive listen → DNS*. We're skipping the middle step
deliberately. The next several chapters build a web browser,
which is a pure client. The first time we'll need a server in
this OS is when we add a remote-debugger or filesystem-share
layer — at which point we can revisit `bind`, `listen`, and
`accept` with real users in mind. For now, every additional
piece of TCP machinery would be code we wrote and didn't run.

## What's next

Chapter 65 will tackle URL parsing and a real HTTP/1.1
response parser, so we can fetch HTML instead of guessing
where the body starts. From there the remaining chapters in
Part VII walk up the browser stack: HTML tokenisation, a CSS
table layout, a headless renderer, and finally a windowed
browser running on top of our window manager.
