---
title: Freelist mangling
parent: Design
nav_order: 4
permalink: /design/freelist-mangling/
lede: >-
  Cookie-encoded free pointers at every tier (P5.4, P5.13, P5.13b) -- and
  why libumem keeps forward-only mangling rather than snmalloc's
  involution encoding.
---

A classic allocator stores its free-list links *inside the freed user
buffer*. That is cheap, but it means a single heap overflow into a freed
buffer can rewrite a link and make the next allocation of that size return
an attacker-chosen address. libumem's defense is to **mangle** every stored
free pointer with a per-process cookie so a forged raw address does not
decode to anything useful.

## The three tiers, each mangled

Free pointers live at three tiers of the [caching
hierarchy]({{ '/design/magazine-depot-ptc/' | relative_url }}), and each was
hardened in turn:

- **Slab/magazine free links (P5.4).** The in-buffer links are mangled with
  an `AT_RANDOM` cookie XORed with the slot address, plus alignment and
  slab-containment validation that *reports* rather than dereferences a bad
  link. A one-buffer overflow that used to yield an arbitrary-address
  allocation now fails the check.
- **Per-thread cache bin slots (P5.13).** v3.3.0 safe-linked the PTC bin
  slots with a per-process cookie.
- **Per-thread magazine rounds (P5.13b).** The `mag_round[]` entries behind
  the bins were still raw pointers after P5.13; v3.3.1 encodes them with the
  same `ptr ^ cookie ^ (&round >> 12)` transform. This is the AGENTS.md
  "grep for siblings" rule in action: fixing the bins and declaring the
  class closed would have left the magazine layer exposed.

The transform costs ~0 % on the fast path -- the bins absorb steady-state
traffic, and the magazine transform fires only on a batch that spills past a
bin.

## Which control actually blocks the attack (P5.4 evidence gap)

The original P5.4 regression's target was *outside* the victim slab, so the
*containment check* alone blocked it -- the mangling was never exercised. A
later `inslab` case targets a live neighbour *inside* the slab, aligned, so
only the mangling stands between the overflow and a still-allocated buffer
being handed back. It fails with `-DUMEM_NO_LINK_MANGLE` and passes by
default, so both controls are now independently demonstrated
(`docs/results/2026-09-23-p54-which-control-blocks.md`). Stating "the
mangling blocks it" without a test that isolates the mangling would have
been exactly the unverified security claim AGENTS.md forbids.

## Compared to glibc

glibc's safe-linking stores `(ptr >> 12) ^ ptr` with **no secret** -- an
attacker who can read one freed chunk's link can derive the transform.
libumem's links carry a per-process `AT_RANDOM` cookie, so a read of one
link does not reveal the key. libumem's in-buffer links are therefore
**stronger** than glibc's here. (They remain in-buffer, not out-of-line;
that is the slab design, not a regression.)

## Why not snmalloc's involution encoding

snmalloc encodes **both directions** of a doubly-linked free queue -- a
forward transform `f(a) = a ^ k0` and a backward transform
`g(a,b) = (a ^ k1) * (b ^ k2)`, with the invariant `x.next.prev == x`.
That catches corruption a forward-only scheme cannot: a forged *backward*
link is detected when the invariant fails.

libumem declined it, deliberately. snmalloc's own
`docs/security/FreelistProtection.md` states the encoding "can only use
queues, and not stacks," because the backward transform requires
construction order and consumption order to **match**. libumem's magazines
are **LIFO stacks**: alloc pops `mag_round[--rounds]`, free pushes
`mag_round[rounds++]` (the last freed is the next allocated). The per-thread
cache bins are stacks too. The precondition the involution encoding needs is
exactly the one libumem's hierarchy violates.

{: .not_chosen }
> Grafting involution encoding on would mean turning the LIFO magazines and
> PTC bins into FIFO queues -- a change to the cache-locality property that
> is a core reason the slab/magazine design performs the way it does. That
> is a re-architecture, not an adoption. libumem keeps its forward-only
> cookie mangling, which is already stronger than glibc's cookieless
> safe-linking. See [ADR: involution encoding
> declined]({{ '/adr/0004-involution-encoding-declined/' | relative_url }}).

## Still open

- **`umem_abort = 0`** is the interpose-mode default: a rejected pointer is
  logged and the process continues, where glibc aborts. Defensible now that
  a rejected pointer leaves allocator state untouched.
  `UMEM_OPTIONS=abort` restores aborting (an escape hatch that, before this
  work, was documented but implemented by nothing).
