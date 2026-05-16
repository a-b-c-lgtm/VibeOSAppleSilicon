# Chapter 100 — accept() and a server socket API

**Status:** Stub. Tracking milestone 93.

The kernel can passively accept connections (chapter 99).
This chapter exposes the server side to userspace through
a familiar syscall surface.

## What this chapter adds

- `SYS_SOCKET_LISTEN(port, backlog)` → fd for a listening
  socket.
- `SYS_SOCKET_ACCEPT(listen_fd, peer_ip_out, peer_port_out)`
  → fd for the accepted connection.
- `socket_listen` / `socket_accept` libc helpers.
- `/bin/echod` — a 50-line accept loop that echoes back
  whatever a client sends. Companion to httpget.

## Prerequisites

- Chapter 64 — socket syscalls
- Chapter 99 — listen state

## Plan

- The listening fd's "kind" is FD_SOCKET_LISTEN; reads on
  it return -EINVAL.
- accept blocks until the queue has at least one ESTABLISHED
  slot; non-blocking variant returns -EAGAIN.
- Peer address is output via in/out pointers, not the return
  value, matching POSIX.

## What you'll learn

- Why `accept` is a separate syscall instead of a flag on
  read.
- The "how blocking is described in a syscall" pattern
  (block-or-EAGAIN).

## What this unlocks

- /bin/httpd (chapter 101).
- Future: other tiny servers (a `/bin/dnsd` if we ever
  want to serve DNS).
