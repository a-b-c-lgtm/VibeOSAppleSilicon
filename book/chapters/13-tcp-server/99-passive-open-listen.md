# Chapter 99 — Passive open: LISTEN, SYN_RECEIVED, the backlog

**Status:** Stub. Tracking milestone 92.

Chapter 63 deferred passive-open TCP. This chapter
implements it: a server can `tcp_listen(port)` and the
state machine handles the three-way handshake from the
other side.

## What this chapter adds

- New TCP states: `LISTEN`, `SYN_RECEIVED`.
- Per-port listening conn (separate from connected conns).
- Backlog queue: pending `SYN_RECEIVED` slots, default
  cap = 8, with the SYN-flood-resistant
  "drop oldest half-open under pressure" policy.
- `tcp_accept(listen_cid)` returns a fully ESTABLISHED
  conn id.

## Prerequisites

- Chapter 63 — TCP client + state machine

## Plan

- Find-conn-for-pkt extension: if no exact 4-tuple match,
  is there a listener on the local port? If so, branch
  into "create SYN_RECEIVED slot, send SYN+ACK."
- ACK on the SYN+ACK promotes SYN_RECEIVED → ESTABLISHED
  and pushes onto the listener's accept queue.
- Resource caps: a listener counts toward TCP_CONN_CAP;
  pending half-opens too.

## What you'll learn

- Why SYN floods exist as an attack class and how a backlog
  queue defends.
- The "active vs passive" symmetry — same state machine,
  different starting point.

## What this unlocks

- A kernel-side `accept()` syscall (chapter 100).
- Eventually `/bin/httpd` (chapter 101).
