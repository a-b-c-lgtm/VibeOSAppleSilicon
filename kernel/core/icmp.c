/*
 * kernel/core/icmp.c \u2014 ICMPv4 echo responder.
 *
 * Strict subset: we answer echo requests (RFC 792 type 8 \u2192 reply
 * type 0) and ignore everything else.  No outbound ping client
 * yet; that's a userspace `ping` once we have UDP-style
 * raw-socket plumbing.
 */

#include "icmp.h"
#include "net.h"
#include "../device/virtio_net.h"

static void *i_memcpy(void *d, const void *s, uint32_t n)
{
    uint8_t       *p = (uint8_t *)d;
    const uint8_t *q = (const uint8_t *)s;
    while (n--) *p++ = *q++;
    return d;
}

static icmp_echo_reply_cb g_reply_cb = (icmp_echo_reply_cb)0;

void icmp_set_echo_reply_callback(icmp_echo_reply_cb cb) { g_reply_cb = cb; }

void icmp_handle(const struct ipv4_hdr *ip,
                 const uint8_t *payload, uint32_t plen)
{
    if (plen < ICMP_ECHO_HDR_LEN) return;
    const struct icmp_echo_hdr *req =
        (const struct icmp_echo_hdr *)payload;

    /* Verify the inbound checksum.  ICMP's checksum is over the
     * ICMP header + data only \u2014 no IP pseudo-header (unlike UDP
     * and TCP). */
    uint16_t got = req->checksum;
    /* Build a local copy so we can null the checksum without
     * touching the device buffer. */
    uint8_t buf[1500];
    if (plen > sizeof(buf)) return;
    i_memcpy(buf, payload, plen);
    struct icmp_echo_hdr *tmp = (struct icmp_echo_hdr *)buf;
    tmp->checksum = 0;
    if (net_ipv4_checksum(buf, plen) != got) return;

    if (req->type == ICMP_TYPE_ECHO_REPLY) {
        if (g_reply_cb) {
            g_reply_cb(ip->src,
                       net_be16_to_cpu(req->id),
                       net_be16_to_cpu(req->seq));
        }
        return;
    }
    if (req->type != ICMP_TYPE_ECHO_REQUEST) return;

    /* Build the reply in-place: change the type to 0 and
     * recompute the checksum.  All other fields (code, id,
     * seq, payload) are echoed back verbatim. */
    tmp->type     = ICMP_TYPE_ECHO_REPLY;
    tmp->code     = 0;
    tmp->checksum = 0;
    tmp->checksum = net_ipv4_checksum(buf, plen);

    /* Send back to the original source.  The IP `src` field of
     * the inbound packet is the requester. */
    (void)net_ipv4_send(ip->src, IPV4_PROTO_ICMP, buf, plen);
}

int icmp_send_echo(const uint8_t dst_ip[NET_IPV4_LEN],
                   uint16_t id, uint16_t seq)
{
    struct icmp_echo_hdr h;
    h.type     = ICMP_TYPE_ECHO_REQUEST;
    h.code     = 0;
    h.checksum = 0;
    h.id       = net_cpu_to_be16(id);
    h.seq      = net_cpu_to_be16(seq);
    h.checksum = net_ipv4_checksum(&h, sizeof(h));
    return net_ipv4_send(dst_ip, IPV4_PROTO_ICMP, &h, sizeof(h));
}
