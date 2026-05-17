/* main.c — kernel C entry point.
 *
 * Milestone 6: DTB-driven memory discovery + 4 KiB page allocator.
 * The previous milestones reserved 16 MiB of heap statically in
 * the linker script.  That is fine for a smoke test but does not
 * scale: real systems learn their physical-memory layout from the
 * device tree the firmware passed to them.
 *
 * QEMU's `-kernel ELF` boot path does NOT follow the Linux/aarch64
 * boot protocol — it loads the ELF and jumps to the entry point
 * with x0 = 0.  We work around this by loading an extracted DTB
 * via `-device loader,file=assets/virt.dtb,addr=0x44000000` and
 * having the kernel fall back to that hardcoded address when
 * dtb_phys arrives as zero.
 *
 * After this chapter:
 *   - kheap_init() takes its memory from pmem rather than a
 *     statically-reserved .heap section,
 *   - the heap can grow to whatever the DTB reports as available
 *     RAM (minus the kernel image, page tables, and the DTB
 *     itself, which are all carved out at init time).
 */

#include <stdint.h>
#include "serial.h"
#include "timer.h"
#include "walltime.h"
#include "heap.h"
#include "thread.h"
#include "fdt.h"
#include "pmem.h"
#include "pmem_refcount.h"
#include "page_cache.h"
#include "elf.h"
#include "embedded_user.h"
#include "vfs.h"
#include "osfs.h"
#include "osfs2.h"
#include "osfs2_cache.h"
#include "../arch/pmap.h"
#include "../arch/cpu.h"
#include "../device/gic.h"
#include "../device/virtio_blk.h"
#include "../device/virtio_gpu.h"
#include "../device/virtio_input.h"
#include "../device/virtio_tablet.h"
#include "../device/virtio_net.h"
#include "../device/virtio_snd.h"
#include "net.h"
#include "dhcp.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "dns.h"
#include "../device/blk_cache.h"
#include "../device/fb.h"
#include "../device/font.h"
#include "../device/text.h"
#include "console_in.h"
#include "wm.h"
#include "../arch/address_space.h"

/* Set to 1 to verify the panic path by deliberately faulting on an
 * unmapped address. */
#define DEMO_FAULT 0

/* Tick interval moved into timer.h so SYS_UPTIME_MS can read
 * it without dragging main.c into the syscall TU. */
#define HEARTBEAT_TICKS  10

static inline void irqs_enable(void)
{
    /* DAIF bit 1 = I (IRQ mask).  daifclr clears the named bits. */
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

/* ----------------------------------------------------------------
 * Milestone-53 net stack smoke test.
 *
 * Replaces the milestone-52 hand-rolled ARP test.  We now have
 * a real Ethernet/ARP/IPv4 stack in `kernel/core/net.{c,h}`, so
 * the test exercises THAT instead of poking the driver directly:
 *
 *   1. Configure the stack with the SLIRP defaults
 *      (10.0.2.15/24, gateway 10.0.2.2).
 *   2. Call `net_arp_resolve(gateway)`, which sends a broadcast
 *      ARP request and spins until an entry appears in the
 *      cache (or the budget runs out).
 *   3. Print the learned MAC.
 *
 * Once milestone 54 lands (ICMP echo) we'll go further and
 * actually `ping` the gateway over the new stack.
 * ---------------------------------------------------------------- */

/* SLIRP defaults: guest = 10.0.2.15/24, gateway = 10.0.2.2. */
static const uint8_t SLIRP_GUEST_IP[NET_IPV4_LEN] = { 10, 0, 2, 15 };
static const uint8_t SLIRP_GW_IP   [NET_IPV4_LEN] = { 10, 0, 2,  2 };
static const uint8_t SLIRP_NETMASK [NET_IPV4_LEN] = { 255, 255, 255, 0 };

/* Print one byte as exactly two lowercase hex digits, no "0x" prefix.
 * Local helper because serial_puthex() prints the full 64-bit
 * value, which is too noisy for printing IPv4 octets and MAC
 * bytes side-by-side. */
static void serial_puthex8(uint8_t v)
{
    static const char d[] = "0123456789abcdef";
    serial_putc(d[(v >> 4) & 0xF]);
    serial_putc(d[v & 0xF]);
}

static volatile int g_icmp_reply_seen = 0;
static void icmp_test_reply(const uint8_t src_ip[NET_IPV4_LEN],
                            uint16_t id, uint16_t seq)
{
    (void)src_ip; (void)id; (void)seq;
    g_icmp_reply_seen = 1;
}

static void net_self_test(void)
{
    if (!virtio_net_present()) return;

    /* Phase 1: bring up the dispatcher with no IPv4 config yet
     * so we can have a DHCP conversation from 0.0.0.0:68 \u2192
     * 255.255.255.255:67. */
    if (net_attach() < 0) {
        serial_puts("[net] attach failed\n");
        return;
    }

    /* Phase 2: try DHCP.  QEMU's SLIRP backend bundles a DHCP
     * server that hands out 10.0.2.15/24, gw 10.0.2.2.  If
     * that fails (host has no -netdev, or non-SLIRP backend
     * with no DHCP), fall through to the static config so the
     * rest of the boot path still works. */
    if (dhcp_acquire(200000000ULL) < 0) {
        serial_puts("[net] DHCP failed; falling back to static\n");
        if (net_set_ipv4_config(SLIRP_GUEST_IP, SLIRP_GW_IP,
                                SLIRP_NETMASK) < 0) {
            serial_puts("[net] static config failed too\n");
            return;
        }
    }

    /* Phase 3: prove the stack works end-to-end.  ARP-resolve
     * the gateway and report its MAC.  This is the milestone-53
     * self-test, run unchanged on whichever IP we ended up
     * with. */
    serial_puts("[net] self-test: ARP resolve gateway\n");
    uint8_t gw[NET_IPV4_LEN];
    net_get_config((uint8_t *)0, (uint8_t *)0, gw, (uint8_t *)0);
    uint8_t mac[NET_MAC_LEN];
    if (!net_arp_resolve(gw, mac, 50000000ULL)) {
        serial_puts("[net] self-test: ARP timeout (no host -netdev?)\n");
        return;
    }
    serial_puts("[net] self-test: gateway MAC=");
    for (int i = 0; i < NET_MAC_LEN; i++) {
        serial_puthex8(mac[i]);
        if (i < NET_MAC_LEN - 1) serial_putc(':');
    }
    serial_puts("\n[net] self-test: ARP cache populated (stack OK)\n");

    /* Phase 4 (M54): ping the gateway.  SLIRP's pseudo-host
     * answers ICMP echo requests addressed to itself, so this
     * exercises the full IPv4+ICMP TX/RX path through our
     * stack \u2014 not just ARP at L2. */
    serial_puts("[net] self-test: ICMP echo gateway\n");
    g_icmp_reply_seen = 0;
    icmp_set_echo_reply_callback(icmp_test_reply);
    if (icmp_send_echo(gw, 0xBEEF, 1) == 0) {
        for (uint64_t i = 0; i < 50000000ULL && !g_icmp_reply_seen; i++) {
            if ((i & 0xfffu) == 0) (void)net_poll();
            __asm__ volatile("" ::: "memory");
        }
        (void)net_poll();
    }
    icmp_set_echo_reply_callback((icmp_echo_reply_cb)0);
    if (g_icmp_reply_seen)
        serial_puts("[net] self-test: ICMP echo reply received (ping OK)\n");
    else
        serial_puts("[net] self-test: no ICMP reply (gateway may drop pings)\n");

    /* Phase 5 (M55): exercise TCP end-to-end against an HTTP
     * server the test harness brings up on the host's port
     * 8888.  SLIRP forwards 10.0.2.2:8888 to the host's
     * 127.0.0.1:8888.  When the harness isn't running, the SYN
     * times out cleanly and we just log "no TCP server". */
    serial_puts("[net] self-test: TCP connect to 10.0.2.2:8888\n");
    int cid = tcp_connect(gw, 8888);
    if (cid < 0) {
        serial_puts("[net] self-test: tcp_connect failed (no slot?)\n");
    } else {
        /* Wait for ESTABLISHED. */
        int established = 0;
        for (uint64_t i = 0; i < 50000000ULL; i++) {
            if ((i & 0xfffu) == 0) (void)net_poll();
            int s = tcp_state(cid);
            if (s == TCP_ESTABLISHED) { established = 1; break; }
            if (s == TCP_CLOSED)      { break; }
            __asm__ volatile("" ::: "memory");
        }
        if (!established) {
            serial_puts("[net] self-test: TCP SYN timeout (no listener)\n");
            (void)tcp_close(cid);
        } else {
            serial_puts("[net] self-test: TCP connection ESTABLISHED\n");
            static const char req[] =
                "GET / HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n";
            (void)tcp_send(cid, req, sizeof(req) - 1);
            /* Drain RX until peer EOF or budget exhausted. */
            uint32_t total = 0;
            uint8_t  scratch[256];
            for (uint64_t i = 0; i < 200000000ULL; i++) {
                if ((i & 0xfffu) == 0) (void)net_poll();
                int n = tcp_recv(cid, scratch, sizeof(scratch));
                if (n > 0) { total += (uint32_t)n; }
                else if (tcp_eof(cid)) { break; }
                __asm__ volatile("" ::: "memory");
            }
            serial_puts("[net] self-test: HTTP response bytes=");
            serial_puthex((uint64_t)total);
            serial_puts("\n");
            (void)tcp_close(cid);
            /* Brief drain to let the FIN exchange settle. */
            for (uint64_t i = 0; i < 5000000ULL; i++) {
                if ((i & 0xfffu) == 0) (void)net_poll();
                if (tcp_state(cid) == TCP_CLOSED) break;
                __asm__ volatile("" ::: "memory");
            }
            serial_puts("[net] self-test: TCP close complete\n");
        }
    }

    /* Phase 6 (M57): exercise DNS by resolving a fixed name.
     * SLIRP's built-in DNS server (10.0.2.3) forwards queries
     * to the host's resolver, so this is a real round-trip
     * through the host's DNS path.  We use "example.com" as a
     * stable, low-traffic test target documented for exactly
     * this kind of probe. */
    serial_puts("[net] self-test: DNS resolve example.com\n");
    uint8_t dip[4];
    if (dns_resolve("example.com", dip) == 0) {
        serial_puts("[net] self-test: DNS reply ip=");
        for (int i = 0; i < 4; i++) {
            uint8_t v = dip[i];
            char b[4]; int n = 0;
            if (v == 0) b[n++] = '0';
            else while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
            while (n--) serial_putc(b[n]);
            if (i < 3) serial_putc('.');
        }
        serial_puts("\n");
    } else {
        serial_puts("[net] self-test: DNS resolve failed (network restricted?)\n");
    }

    /* Phase 7 (chapter 103 / M92): passive open.
     *
     * Bring up a TCP listener on port 8088, then wait briefly
     * for an external SYN.  The test harness (scripts/
     * test_passive_open.py) starts QEMU with
     *
     *     -netdev user,id=n0,hostfwd=tcp::18088-:8088
     *
     * so SLIRP forwards host port 18088 -> guest port 8088.
     * The harness connects to localhost:18088, the kernel
     * accepts, and we log the 4-tuple so the harness can
     * pattern-match the success line.
     *
     * If the harness isn't running (the default run-graphical
     * boot), the accept loop times out cleanly and we just log
     * "no inbound connection" before moving on. */
    serial_puts("[net] self-test: TCP listen on port 8088\n");
    int lid = tcp_listen(8088);
    if (lid < 0) {
        serial_puts("[net] self-test: tcp_listen failed\n");
    } else {
        /* Wait up to ~30s wall-time for an inbound SYN.  The
         * harness on the host-side has to first observe the
         * "TCP listen" line above on the serial port, then dial
         * 127.0.0.1:18088 (SLIRP forwards that to us), and the
         * round-trip can take a beat.  Bigger budget than the
         * connect-side test (50M) for that reason. */
        int accepted = -1;
        for (uint64_t i = 0; i < 2000000000ULL; i++) {
            if ((i & 0xfffu) == 0) (void)net_poll();
            int child = tcp_accept(lid);
            if (child >= 0) { accepted = child; break; }
            __asm__ volatile("" ::: "memory");
        }
        if (accepted < 0) {
            serial_puts("[net] self-test: no inbound connection (TCP accept timeout)\n");
            (void)tcp_close(lid);
        } else {
            serial_puts("[net] self-test: TCP accepted cid=");
            serial_puthex((uint64_t)accepted);
            serial_puts("\n");

            /* Drain anything the peer sends until they close,
             * then close our side cleanly.  Bounded budget so
             * we always make forward progress even if the peer
             * forgets to close. */
            uint32_t total = 0;
            uint8_t  scratch[256];
            for (uint64_t i = 0; i < 200000000ULL; i++) {
                if ((i & 0xfffu) == 0) (void)net_poll();
                int n = tcp_recv(accepted, scratch, sizeof(scratch));
                if (n > 0) total += (uint32_t)n;
                else if (tcp_eof(accepted)) break;
                __asm__ volatile("" ::: "memory");
            }
            serial_puts("[net] self-test: TCP accept payload bytes=");
            serial_puthex((uint64_t)total);
            serial_puts("\n");
            (void)tcp_close(accepted);
            (void)tcp_close(lid);
            /* Brief drain so the FIN exchange settles. */
            for (uint64_t i = 0; i < 5000000ULL; i++) {
                if ((i & 0xfffu) == 0) (void)net_poll();
                __asm__ volatile("" ::: "memory");
            }
            serial_puts("[net] self-test: TCP passive close complete\n");
        }
    }
}

static void heap_demo(void)
{
    serial_puts("\n[heap] running split + coalesce smoke test\n");
    serial_puts("[heap] initial used = ");
    serial_puthex((uint64_t)kheap_used());
    serial_puts(", blocks = ");
    serial_puthex((uint64_t)kheap_block_count());
    serial_puts("\n");

    void *a = kmalloc(64);
    void *b = kmalloc(256);
    void *c = kmalloc(1024);

    serial_puts("[heap] after 3 allocs: used = ");
    serial_puthex((uint64_t)kheap_used());
    serial_puts(", blocks = ");
    serial_puthex((uint64_t)kheap_block_count());
    serial_puts("\n");

    /* Free middle then ends — the middle free creates a hole
     * surrounded by used blocks; freeing the ends should fully
     * coalesce back to a single free block (count = 1). */
    kfree(b);
    serial_puts("[heap] after kfree(middle): blocks = ");
    serial_puthex((uint64_t)kheap_block_count());
    serial_puts("\n");

    kfree(a);
    kfree(c);
    serial_puts("[heap] after kfree(all):    blocks = ");
    serial_puthex((uint64_t)kheap_block_count());
    serial_puts(", used = ");
    serial_puthex((uint64_t)kheap_used());
    serial_puts("\n\n");
}

/* Each worker spins on a counter without ever yielding.  The
 * timer ISR's call to schedule() is the only thing that gets
 * the other thread on the CPU. */
static void busy_worker(void *arg)
{
    uintptr_t iters = (uintptr_t)arg;
    const char *name = thread_current()->name;
    for (uintptr_t i = 0; i < iters; i++) {
        /* ~150 ms of CPU under HVF on M2 — comfortably more than
         * one 100 ms timer tick, so each iteration is guaranteed
         * to be sliced at least once.  The inline-asm volatile
         * empty insn keeps the loop alive against -O2. */
        for (volatile uint64_t spin = 0; spin < 60000000ULL; spin++) {
            __asm__ volatile("" ::: "memory");
        }
        serial_puts("[");
        serial_puts(name);
        serial_puts("] iter ");
        serial_puthex((uint64_t)i);
        serial_puts("\n");
    }
    serial_puts("[");
    serial_puts(name);
    serial_puts("] done\n");
}

static void preemption_demo(void)
{
    serial_puts("\n[thread] spawning two CPU-bound busy workers\n");
    serial_puts("[thread] (no yield calls — only the timer can swap them)\n");
    thread_create(busy_worker, (void *)(uintptr_t)4, "busy-A");
    thread_create(busy_worker, (void *)(uintptr_t)4, "busy-B");

    /* Reap them as they exit.  thread_wait blocks until any child
     * exits, returns -1 once we have no children left. */
    while (thread_wait(NULL) >= 0) { }
    serial_puts("[thread] all workers reaped\n\n");
}

/* Chapter 82 — OSFS-2 background flusher.
 *
 * The write-back cache lets userspace defer durability until
 * fsync(), but well-behaved apps shouldn't be the only safety
 * net: if the kernel panics or the user yanks power between
 * fsyncs, recently-written data is lost.  This thread bounds
 * the risk window by flushing every dirty cache slot at a fixed
 * cadence (currently 5 s).
 *
 * The flush is best-effort: a -1 from osfs2_cache_flush leaves
 * the offending slot dirty so the next iteration retries it.
 * We don't care about being fair to the cache vs. real work \u2014
 * a 5-second cadence on a single-CPU system contributes roughly
 * 0% wall-clock overhead even when every slot is dirty. */
#define OSFS2_FLUSH_INTERVAL_MS 5000ULL

static int g_osfs2_mounted = 0;

static void osfs2_flush_thread(void *arg)
{
    (void)arg;
    for (;;) {
        thread_sleep_ms(OSFS2_FLUSH_INTERVAL_MS);
        if (osfs2_cache_dirty_count() == 0) continue;
        if (osfs2_cache_flush() != 0) {
            serial_puts("[osfs2_cache] background flush hit -EIO\n");
        }
    }
}

/* Spawn the flusher only AFTER the preemption demo's thread_wait
 * loop has finished — preemption_demo reaps every child via
 * thread_wait(NULL) and would block forever on a never-exiting
 * flusher.  Calling this between preemption_demo() and
 * userspace_demo() is the right time: kernel demos done,
 * userspace processes not yet spawned. */
static void osfs2_flusher_start(void)
{
    if (!g_osfs2_mounted) return;
    thread_create(osfs2_flush_thread, NULL, "osfs2-flush");
    serial_puts("[osfs2_cache] background flusher spawned "
                "(every 5 s)\n");
}

/*
 * userspace_demo — milestone-9 smoke test.
 *
 * The boot thread is a kernel thread; it cannot call SYS_SPAWN
 * itself.  So we hand-roll the moral equivalent here for the
 * very first user program: look up /bin/init in the ramfs, parse
 * it as ELF, and spawn it as a user thread.  /bin/init then uses
 * SYS_SPAWN/SYS_WAIT to run the rest of the demo (hello, cat).
 *
 * Yields until /bin/init exits — this idle loop replaces the
 * older one-program-at-a-time helper from milestone 8.
 */
static void userspace_demo(void)
{
    uint8_t *data; size_t size;
    int rc = vfs_load("/bin/init", &data, &size);
    if (rc != 0) {
        serial_puts("[user] FATAL — /bin/init not found (rc=");
        serial_puthex((uint64_t)(int64_t)rc);
        serial_puts(")\n");
        return;
    }
    serial_puts("\n[user] loading /bin/init (");
    serial_puthex((uint64_t)size);
    serial_puts(" bytes)\n");

    struct address_space *as = address_space_create();
    if (!as) {
        serial_puts("[user] FATAL — address_space_create returned NULL\n");
        kfree(data);
        return;
    }

    struct user_image img;
    /* init takes no arguments — argv = { "init", NULL } so it sees
     * argc=1 and argv[0]="init". */
    static const char *const init_argv[] = { "init", NULL };
    int loaded = elf_load_user(data, size, as, init_argv, &img);
    kfree(data);
    if (loaded != 0) {
        serial_puts("[user] FATAL — elf_load_user failed\n");
        address_space_destroy(as);
        return;
    }
    serial_puts("[user] entry = ");
    serial_puthex(img.entry_va);
    serial_puts(", sp = ");
    serial_puthex(img.stack_top_va);
    serial_puts("\n");

    struct thread *u = user_thread_create(img.entry_va,
                                          img.stack_top_va, "init", as);
    if (!u) {
        serial_puts("[user] FATAL — user_thread_create returned NULL\n");
        address_space_destroy(as);
        return;
    }
    serial_puts("[user] spawned init pid ");
    serial_puthex((uint64_t)u->id);
    serial_puts("\n");

    /* Reap init when it exits.  thread_wait blocks the boot
     * thread until init transitions to EXITED. */
    int code = 0;
    int tid  = thread_wait(&code);
    serial_puts("[user] init (pid ");
    serial_puthex((uint64_t)tid);
    serial_puts(") exited code = ");
    serial_puthex((uint64_t)code);
    serial_puts("\n");
}

void kernel_main(uint64_t dtb_phys)
{
    serial_init();

    serial_puts("\n");
    serial_puts("============================================================\n");
    serial_puts("osdev aarch64 — milestone 38 (virtio-gpu framebuffer)\n");
    serial_puts("============================================================\n");

    serial_puts("dtb_phys = ");
    serial_puthex(dtb_phys);
    serial_puts("\n");

    serial_puts("initialising GIC v3 ... ");
    gic_init();
    serial_puts("ok\n");

    serial_puts("priming generic timer for 100 ms ticks ... ");
    timer_init(TICK_INTERVAL_MS);
    gic_set_priority(TIMER_CNTV_INTID, 0x80);
    gic_enable_irq(TIMER_CNTV_INTID);
    serial_puts("ok\n");

    /* QEMU's `-kernel ELF` boot path leaves x0 = 0; we load the
     * DTB ourselves at a known address via -device loader.  Fall
     * back to that constant if the firmware did not pass one. */
    uint64_t dtb_addr = dtb_phys ? dtb_phys : 0x44000000ULL;
    uint32_t dtb_size = 0;
    if (!fdt_validate((const void *)(uintptr_t)dtb_addr, &dtb_size)) {
        serial_puts("[fdt] FATAL — no valid DTB at ");
        serial_puthex(dtb_addr);
        serial_puts("\n");
        for (;;) __asm__ volatile("wfe");
    }
    serial_puts("[fdt] valid DTB at ");
    serial_puthex(dtb_addr);
    serial_puts(", totalsize = ");
    serial_puthex(dtb_size);
    serial_puts("\n");

    struct fdt_memory_map mem;
    fdt_read_memory((const void *)(uintptr_t)dtb_addr, &mem);
    if (mem.count == 0) {
        serial_puts("[fdt] FATAL — no /memory regions found\n");
        for (;;) __asm__ volatile("wfe");
    }
    for (size_t i = 0; i < mem.count; i++) {
        serial_puts("[fdt] memory[");
        serial_puthex((uint64_t)i);
        serial_puts("] base = ");
        serial_puthex(mem.regions[i].base);
        serial_puts(", size = ");
        serial_puthex(mem.regions[i].size);
        serial_puts("\n");
    }

    /* Chapter 95 — wall-clock subsystem.  Reads the PL031 RTC
     * once and pairs it with the current tick count.  Failure is
     * non-fatal: walltime_now_us still works and counts from
     * epoch (which makes timestamps look like 1970, an obvious
     * "no real RTC" signal to userspace). */
    walltime_init((const void *)(uintptr_t)dtb_addr);

    /* Install a Normal-Cacheable L1 block descriptor for every
     * 1 GiB chunk of RAM the DTB reported.  L1[1] (the 1–2 GiB
     * window) is already live from boot, so this loop is a no-op
     * for it; everything beyond is freshly mapped and immediately
     * usable thanks to the TLBI / ISB inside the helper. */
    const uint64_t GIB = 1ULL << 30;
    size_t mapped_blocks = 0;
    for (size_t i = 0; i < mem.count; i++) {
        uint64_t base = mem.regions[i].base & ~(GIB - 1);
        uint64_t end  = mem.regions[i].base + mem.regions[i].size;
        for (uint64_t pa = base; pa < end; pa += GIB) {
            pmap_install_ram_block_1gib(pa);
            mapped_blocks++;
        }
    }
    serial_puts("[pmap] installed ");
    serial_puthex((uint64_t)mapped_blocks);
    serial_puts(" x 1 GiB RAM block descriptor(s)\n");

    /* Carve out everything pmem must NOT hand out:
     *   1. The kernel image: [KERNEL_LOAD_ADDR, kernel_end), padded
     *      to page boundaries.  The `kernel_end` symbol is exported
     *      by the linker script.
     *   2. The DTB blob itself, padded to a page.
     *   3. The first 1 GiB of physical memory, which on virt is
     *      MMIO (GIC, PL011, etc.).  Our DTB technically reports
     *      RAM starting at 0x40000000, so this is implicit, but
     *      we add it defensively in case a future board has RAM
     *      down low. */
    extern uint8_t kernel_end[];
    uint64_t kimg_start = 0x40080000ULL;
    uint64_t kimg_end   = ((uint64_t)(uintptr_t)kernel_end + 0xFFFULL) & ~0xFFFULL;

    /* Top-of-RAM guard.  The topmost 4 KiB of physical RAM is
     * carved out so neither the heap nor any future contiguous
     * allocation can land at the very end of mapped memory.
     *
     * Why: a 16 KiB thread kernel stack whose frame_top sits AT
     * end-of-RAM (or just past it, courtesy of the +16-byte heap
     * header offset) makes save_context's `stp` issue 8-byte
     * stores that straddle the last mapped page.  On Apple HVF
     * those stores are silently absorbed (no fault) but the
     * matching ldp's during restore_context can fault
     * unpredictably depending on what's still cached.  The
     * symptoms are bizarre — half of the saved frame survives,
     * the other half doesn't, and the failing instruction is
     * never the first ldp past the boundary.  The cleanest fix
     * is structural: never hand out memory in the last page so
     * no allocation can extend across the boundary. */
    uint64_t ram_top    = mem.regions[0].base + mem.regions[0].size;
    uint64_t guard_base = (ram_top - PAGE_SIZE) & ~0xFFFULL;

    /* Bottom-of-RAM carveout.  We reserve the first 1 GiB of RAM
     * (covering the kernel image and DTB load address at
     * 0x44000000) so the freelist starts at a clean 1 GiB
     * boundary.  This is the structural counterpart to the
     * top-of-RAM guard: with it, pmem_init's low-first iteration
     * yields a perfectly contiguous freelist from 0x80000000
     * upward, no kernel-image gap to trip pmem_alloc_contig over.
     *
     * The wasted space (the unused tail of the low GiB beyond
     * kernel_end and DTB) is acceptable because the kernel image
     * itself is ~1 MiB; we lose ~1 GiB of "free pmem" but gain a
     * trivially-contig heap allocator and a low-end heap that's
     * guaranteed to be far from the top-of-RAM boundary. */
    struct pmem_carveout carve[] = {
        { .base = kimg_start,             .size = kimg_end - kimg_start },
        { .base = dtb_addr & ~0xFFFULL,   .size = (dtb_size + 0xFFFULL) & ~0xFFFULL },
        { .base = 0,                      .size = 0x80000000ULL },
        { .base = guard_base,             .size = PAGE_SIZE },
    };
    size_t free_pages = 0;
    pmem_init(&mem, carve, sizeof(carve)/sizeof(carve[0]), &free_pages);
    serial_puts("[pmem] usable pages = ");
    serial_puthex((uint64_t)free_pages);
    serial_puts(" (= ");
    serial_puthex((uint64_t)free_pages * 4);
    serial_puts(" KiB)\n");

    /* Grow a heap by stealing pages from pmem.  256 MiB is plenty
     * for the kernel-side bookkeeping of dozens of user processes,
     * each of which can later mmap up to 1 GiB of its own pages
     * straight out of pmem (those allocations bypass kheap entirely). */
    const size_t HEAP_BYTES = 256ULL * 1024 * 1024;
    const size_t HEAP_PAGES = HEAP_BYTES / PAGE_SIZE;
    uint64_t heap_base = pmem_alloc_contig(HEAP_PAGES);
    if (!heap_base) {
        serial_puts("[heap] FATAL — pmem_alloc_contig failed for heap\n");
        for (;;) __asm__ volatile("wfe");
    }
    serial_puts("initialising kernel heap (");
    serial_puthex((uint64_t)HEAP_BYTES);
    serial_puts(" bytes @ ");
    serial_puthex(heap_base);
    serial_puts(") ... ");
    kheap_init(heap_base, HEAP_BYTES);
    serial_puts("ok\n");

    /* Chapter 75 \u2014 init the per-frame refcount table used by the
     * COW fork.  Cover the entire DRAM extent reported by FDT so
     * any PA pmem might hand out is in-range.  Storage is ~2
     * bytes per 4 KiB frame (= 4 MiB for 8 GiB DRAM); paid out
     * of the kheap we just brought up. */
    {
        uint64_t lo = (uint64_t)-1, hi = 0;
        for (size_t i = 0; i < mem.count; i++) {
            uint64_t b = mem.regions[i].base;
            uint64_t e = b + mem.regions[i].size;
            if (b < lo) lo = b;
            if (e > hi) hi = e;
        }
        /* Round to page boundaries. */
        lo &= ~(uint64_t)(PAGE_SIZE - 1);
        hi  = (hi + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        pmem_refcount_init(lo, hi - lo);
    }

    serial_puts("initialising thread bookkeeping ... ");
    /* Chapter 89 — pre-register the boot CPU's struct cpu slot
     * and write TPIDR_EL1 so that thread_init() can use
     * cpu_current()->current to install the boot thread.  Without
     * this, cpu_current() dereferences a still-zero TPIDR_EL1.
     * smp_init_with_dtb later re-populates the same slot
     * harmlessly with identical values. */
    cpu_register_boot();
    thread_init();
    serial_puts("ok\n");

    /* Chapter 86 — wake the secondary CPUs.  Done before VFS so
     * the [smp] log block sits cleanly between the memory/heap
     * setup and the device probe section.  Secondaries park in
     * WFE forever for now; chapter 89 will give them real work. */
    smp_init_with_dtb((const void *)(uintptr_t)dtb_addr);

    serial_puts("initialising VFS ... ");
    vfs_init();
    serial_puts("ok\n");

    /* Chapter 90 \u2014 page cache.  Sits above blk_cache (and
     * above raw ramfs blob copies); fed lazily by the mmap
     * fault handler.  Init order: VFS first (we just announced
     * ramfs files), page cache second (so any later boot stage
     * that wants to mmap a ramfs file finds the cache ready). */
    page_cache_init();

    serial_puts("probing virtio-mmio bus for a block device ... ");
    if (virtio_blk_init() == 0) {
        serial_puts("ok\n");
        blk_cache_init();
        serial_puts("mounting OSFS-1 from disk ... ");
        if (osfs_init() == 0) {
            serial_puts("ok\n");
        } else {
            serial_puts("none\n");
        }
        /* Probe hd1 for OSFS-2 (writable, chapter 81+).  The
         * driver bails harmlessly if the second virtio-blk
         * device is absent or unformatted. */
        serial_puts("mounting OSFS-2 from hd1 ... ");
        if (osfs2_init() == 0) {
            serial_puts("ok\n");
            /* Chapter 82 — only init the write-back cache once we
             * know OSFS-2 actually mounted.  Without OSFS-2 the
             * cache would never be touched, so this is purely
             * cosmetic, but it keeps the boot log honest.
             *
             * Note: the background flusher thread is NOT spawned
             * here.  preemption_demo() reaps every child via
             * thread_wait(NULL) and would deadlock waiting for a
             * never-exiting flusher.  Instead the flusher is
             * spawned in osfs2_flusher_start() after the demo
             * loop completes (see below). */
            osfs2_cache_init();
            g_osfs2_mounted = 1;
        } else {
            serial_puts("none\n");
        }
    } else {
        serial_puts("none\n");
    }

    serial_puts("probing virtio-mmio bus for a GPU ... ");
    if (virtio_gpu_init() == 0) {
        serial_puts("ok\n");
        if (fb_init() == 0) {
            /* Chapter 102 -- initialise the TTF rasteriser now that
             * kmalloc is up and we know we have a framebuffer to
             * render into. font_init_ttf is a no-op if it fails,
             * leaving font_get_default returning the bitmap font;
             * the boot screen below uses font_get_default unchanged. */
            font_init_ttf();
            /* Paint the milestone-38 boot screen.  This is the first
             * graphical artifact the project produces; if it shows up
             * the whole [pmem -> virtio-gpu -> RAM-backed framebuffer
             * -> font blit -> RESOURCE_FLUSH] pipeline is working. */
            const struct fb_info *fb = fb_get_info();
            const struct bitmap_font *font = font_get_default();

            fb_clear(FB_COLOR(0x10, 0x14, 0x28));

            /* A title bar at the top — gives the boot screen the
             * shape of a desktop without dragging in a window
             * manager yet. */
            fb_fill_rect(0, 0, fb->width, 32, FB_COLOR(0x20, 0x30, 0x60));
            fb_fill_rect(0, 32, fb->width, 1, FB_COLOR(0x60, 0x80, 0xC0));

            text_draw_string(font, 12, 8, fb->width, 32,
                             "osdev / aarch64  -  milestone 38: virtio-gpu framebuffer",
                             FB_COLOR_WHITE, FB_COLOR_BLACK,
                             1, NULL, NULL);

            /* Body text. */
            uint32_t y = 64;
            text_draw_string(font, 24, y, fb->width, fb->height,
                             "Hello from a real virtio-gpu scanout.",
                             FB_COLOR_WHITE, FB_COLOR_BLACK,
                             1, NULL, &y);
            y += 24;
            text_draw_string(font, 24, y, fb->width, fb->height,
                             "Pixels are being pushed by RESOURCE_FLUSH after",
                             FB_COLOR(0xC0, 0xD0, 0xF0), FB_COLOR_BLACK,
                             1, NULL, &y);
            y += 18;
            text_draw_string(font, 24, y, fb->width, fb->height,
                             "each frame.  Serial console is still live; press",
                             FB_COLOR(0xC0, 0xD0, 0xF0), FB_COLOR_BLACK,
                             1, NULL, &y);
            y += 18;
            text_draw_string(font, 24, y, fb->width, fb->height,
                             "Ctrl-A X in the terminal (or close the window) to quit.",
                             FB_COLOR(0xC0, 0xD0, 0xF0), FB_COLOR_BLACK,
                             1, NULL, &y);

            /* Three colour swatches so we can verify the channel
             * order at a glance: R should be red, G should be green,
             * B should be blue.  If they come out swapped the
             * pack_color() math is wrong. */
            uint32_t sw_y = y + 32;
            fb_fill_rect(24,  sw_y,  64, 64, FB_COLOR_RED);
            fb_fill_rect(108, sw_y,  64, 64, FB_COLOR_GREEN);
            fb_fill_rect(192, sw_y,  64, 64, FB_COLOR_BLUE);
            fb_draw_rect(24,  sw_y,  64, 64, FB_COLOR_WHITE);
            fb_draw_rect(108, sw_y,  64, 64, FB_COLOR_WHITE);
            fb_draw_rect(192, sw_y,  64, 64, FB_COLOR_WHITE);
            text_draw_string(font, 24,  sw_y + 72, fb->width, fb->height,
                             "R", FB_COLOR_WHITE, FB_COLOR_BLACK, 1,
                             NULL, NULL);
            text_draw_string(font, 108, sw_y + 72, fb->width, fb->height,
                             "G", FB_COLOR_WHITE, FB_COLOR_BLACK, 1,
                             NULL, NULL);
            text_draw_string(font, 192, sw_y + 72, fb->width, fb->height,
                             "B", FB_COLOR_WHITE, FB_COLOR_BLACK, 1,
                             NULL, NULL);

            fb_present(0, 0, 0, 0);
        }
    } else {
        serial_puts("none (text mode only)\n");
    }

    serial_puts("probing virtio-mmio bus for an input device ... ");
    if (virtio_input_init() == 0) {
        serial_puts("ok (keyboard online)\n");
    } else {
        serial_puts("none (serial-only input)\n");
    }

    serial_puts("probing virtio-mmio bus for a tablet ... ");
    if (virtio_tablet_init() == 0) {
        serial_puts("ok (mouse online)\n");
    } else {
        serial_puts("none (no pointing device)\n");
    }

    serial_puts("probing virtio-mmio bus for a NIC ... ");
    if (virtio_net_init() == 0) {
        serial_puts("ok (network online)\n");
        net_self_test();
    } else {
        serial_puts("none (no network)\n");
    }

    serial_puts("probing virtio-mmio bus for a sound card ... ");
    if (virtio_snd_init() == 0) {
        serial_puts("ok (audio online)\n");
    } else {
        serial_puts("none (no audio output)\n");
    }

    serial_puts("initialising window manager ... ");
    wm_init();
    serial_puts("ok\n");

    serial_puts("unmasking IRQs in PSTATE ... ");
    irqs_enable();
    serial_puts("ok\n");

    heap_demo();
    preemption_demo();
    osfs2_flusher_start();
    userspace_demo();
    blk_cache_dump_stats("[blk_cache]");

#if DEMO_FAULT
    serial_puts("[DEMO_FAULT] dereferencing 0x80000000 (unmapped) ...\n");
    *(volatile uint32_t *)0x80000000UL = 0xCAFEBABE;
    serial_puts("UNREACHABLE — fault path is broken!\n");
#endif

    serial_puts("entering wfe loop; heartbeat every ");
    serial_puthex(HEARTBEAT_TICKS * TICK_INTERVAL_MS);
    serial_puts(" ms (Ctrl-A X to quit QEMU)\n\n");

    uint64_t last_heartbeat = 0;
    for (;;) {
        __asm__ volatile("wfe");
        uint64_t now = timer_ticks();
        if (now - last_heartbeat >= HEARTBEAT_TICKS) {
            last_heartbeat = now;
            serial_puts("[tick] count = ");
            serial_puthex(now);
            serial_puts("\n");
        }
    }
}
