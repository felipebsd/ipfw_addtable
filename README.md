# ipfw O_ADDTABLE

An out-of-tree patch for FreeBSD 15/stable that adds an **`addtable`** action
to `ipfw(8)`.  When a packet matches a rule with this action, the packet's
source or destination IP address is asynchronously inserted into the specified
`ipfw` address table and the packet is passed (like `accept`).  No subsequent
rules are evaluated, making `addtable` a *terminal* action.

---

## Table of Contents

1. [Motivation](#motivation)
2. [Design](#design)
3. [Repository layout](#repository-layout)
4. [Requirements](#requirements)
5. [Installation](#installation)
6. [Syntax and examples](#syntax-and-examples)
7. [Displaying rules](#displaying-rules)
8. [Caveats](#caveats)
9. [Testing](#testing)

---

## Motivation

`ipfw` tables are a powerful building block for dynamic packet classification.
They can be updated from userspace at any time, and rules can test table
membership with `O(log n)` or `O(1)` complexity depending on the underlying
algorithm.

Before this patch, inserting an address into a table required a round-trip to
userspace (typically via a `divert` socket or a custom daemon watching
`pflog`/`netflow` data).  The `addtable` action closes that gap: the kernel
itself can populate a table the moment a packet matches a rule, without any
userspace involvement.

Typical use cases:

* **Port-scan / brute-force mitigation** — add the source address to a
  block-list table after the first syn-flood or repeated connection attempt.
* **Traffic accounting** — collect the set of active source addresses observed
  on an interface.
* **Dynamic allow-listing** — add destination addresses of outbound connections
  to an "allowed inbound reply" table.

---

## Design

### Non-blocking packet path

`ipfw_chk()` is called with `IPFW_PF_RLOCK` (a reader lock on the rule chain)
held.  `add_table_entry()` internally acquires `IPFW_UW_WLOCK` then
`IPFW_WLOCK`, both write locks that conflict with the reader lock — calling
it directly would deadlock.

`O_ADDTABLE` therefore *enqueues* a lightweight task onto a private
`taskqueue(9)` thread.  The task runs outside the packet path, acquires the
correct locks, and calls `add_table_entry()`.  UMA is used for the per-entry
allocation so that `M_NOWAIT` allocs are fast and predictable from the packet
path.

### VNET isolation

All mutable subsystem state (UMA zone, taskqueue handle, active flag) is
declared with `VNET_DEFINE_STATIC` so that each network stack instance
(jail/VNET) has its own independent copy.  Destroying one VNET neither drains
nor frees resources belonging to another.

The taskqueue worker thread runs without an inherent VNET context.
`ipfw_addtable_enqueue()` therefore captures `curvnet` and the per-VNET UMA
zone pointer at enqueue time (while the correct VNET is still active) and
stores them in the entry.  The worker restores the right VNET with
`CURVNET_SET()` before calling `add_table_entry()` or `uma_zfree()`.

### Teardown safety

`V_addtable_active` is an atomic flag: `1` while the subsystem is live, `0`
once `ipfw_addtable_destroy()` has been entered.
`ipfw_addtable_enqueue()` checks this flag and returns `ENXIO` early if
teardown has begun.

The primary protection against use-after-free is the `ipfw` module-unload
contract: all `O_ADDTABLE` rules must be deleted before the module can unload,
ensuring no new packets can reach the action once destroy is in progress.  The
atomic flag is a belt-and-suspenders guard for any in-flight packets that
already passed the rule check at that instant.

### Instruction layout

```
ipfw_insn_addtable (2 x 32-bit words = 8 bytes)
 ┌────────────────────────┬───────────────────────┐
 │  ipfw_insn o           │  uint8_t flags        │
 │  .opcode = O_ADDTABLE  │  ADDTABLE_F_DST (0x1) │
 │  .arg1   = table no.   │  0 → add src (default)│
 │  .len    = 2           │  uint8_t _pad[3]      │
 └────────────────────────┴───────────────────────┘
```

`CTASSERT(sizeof(ipfw_insn_addtable) == 2 * sizeof(uint32_t))` is asserted at
compile time so that `F_INSN_SIZE()` and the instruction-pointer advance in
`ipfw_chk()` are always correct.

---

## Repository layout

```
ipfw_addtable/
├── CLAUDE.md                          Project guidelines for AI-assisted dev
├── Makefile                           Apply patches + build helper
├── patches/
│   ├── 0001-ip_fw.h-add-O_ADDTABLE.patch          New opcode + struct
│   ├── 0002-ip_fw2.c-handle-O_ADDTABLE.patch       Kernel action handler + lifecycle
│   ├── 0003-kernel-Makefile.patch                  Build system
│   ├── 0004-ipfw2.h-add-TOK_ADDTABLE.patch         Userspace token
│   ├── 0005-ipfw2.c-userspace-parser.patch         CLI parser + printer
│   └── 0006-ip_fw_sockopt.c-validate-O_ADDTABLE.patch  Opcode validation + table existence check
└── src/
    └── sys/netpfil/ipfw/
        ├── ip_fw_addtable.h           Kernel-internal API
        └── ip_fw_addtable.c           Full implementation
```

---

## Requirements

| Item | Requirement |
|---|---|
| FreeBSD version | 15/stable (or a recent 15-RELEASE) |
| Compiler | clang (FreeBSD base, ≥ 16) |
| Source tree | `/usr/src` checked out from `stable/15` |
| Privileges | `root` for `make install` / `kldload` |

Obtain the FreeBSD source tree if not already present:

```sh
git clone --branch stable/15 https://git.freebsd.org/src.git /usr/src
```

---

## Installation

All operations are driven by the top-level `Makefile`.  `SRCDIR` defaults to
`/usr/src`; override it if your tree lives elsewhere.

### 1. Apply the patches and copy new files

```sh
make SRCDIR=/usr/src apply
```

This copies `ip_fw_addtable.{h,c}` into `${SRCDIR}/sys/netpfil/ipfw/` and
applies all six patches with `patch(1)`.  Patches are idempotent; re-running
`apply` on an already-patched tree will print a harmless "already applied"
message.

### 2. Rebuild the ipfw kernel module

```sh
make SRCDIR=/usr/src kernel
```

Produces `ipfw.ko` under `${SRCDIR}/sys/modules/ipfw/`.

### 3. Rebuild the ipfw(8) userspace tool

```sh
make SRCDIR=/usr/src userspace
```

Produces a new `ipfw` binary under `${SRCDIR}/sbin/ipfw/`.

### 4. Load / reload

```sh
# Unload old module (if loaded), load new one
kldunload ipfw 2>/dev/null || true
kldload ${SRCDIR}/sys/modules/ipfw/ipfw.ko

# Install new userspace binary (optional)
install -m 555 ${SRCDIR}/sbin/ipfw/ipfw /sbin/ipfw
```

### One-shot

```sh
make SRCDIR=/usr/src all     # apply + kernel + userspace
```

### Reverting

New source files can be removed automatically:

```sh
make SRCDIR=/usr/src clean
```

Patches must be reverted manually one at a time:

```sh
for p in patches/000*.patch; do
    patch -d /usr/src -p1 -R < "$p"
done
```

---

## Syntax and examples

```
addtable <tblno> [src|dst]
```

| Token | Description |
|---|---|
| `<tblno>` | Target table number, 0–65535 |
| `src` | Add the **source** address (default when omitted) |
| `dst` | Add the **destination** address |

The action is *terminal*: after the address is enqueued for insertion, the
packet is passed and no subsequent rules are evaluated (return code
`IP_FW_PASS`, same as `accept`).

### Create the target table first

Tables **must exist** before a rule can reference them.  Attempting to add
a rule whose table number does not exist will fail with `ESRCH`:

```sh
ipfw table 10 create type addr
ipfw table 11 create type addr
```

### Add source addresses

```sh
# Tag the source of every packet entering em0
ipfw add 100 addtable 10 src from any to any via em0
```

### Add destination addresses

```sh
# Record destinations of outbound TCP SYN packets
ipfw add 200 addtable 11 dst tcp from me to any setup via em0 out
```

### Combine with other rules

Because `addtable` is terminal (packet is passed immediately after the
address is enqueued), place it *after* any deny rules that should fire
first:

```sh
# 1. Block known bad sources first
ipfw add 499 deny ip from table\(20\) to any

# 2. Record the source of new TCP SYN packets to port 22 and pass them
ipfw add 500 addtable 20 src tcp from any to me 22 setup in

# 3. Normal allow-all traffic otherwise
ipfw add 65534 allow ip from any to any
```

### IPv6 works transparently

```sh
ipfw table 30 create type addr
ipfw add 300 addtable 30 src ip6 from any to any
```

`addtable` uses `addr_type` from the packet's `ipfw_flow_id` to select
`AF_INET` or `AF_INET6` and sets the prefix length to `/32` or `/128`
respectively.

---

## Displaying rules

`ipfw show` prints `addtable` rules in the same format used to create them:

```
00100 addtable 10 src from any to any via em0
00200 addtable 11 dst tcp from me to any setup out via em0
```

---

## Caveats

* **Best-effort insertion** — if the UMA allocation fails (`M_NOWAIT`) or the
  subsystem is shutting down, the packet continues normally and the address is
  *not* added.  This is intentional: dropping packets due to a table
  administrative failure would be worse than missing an entry.

* **Race on teardown** — there is a narrow window between a thread passing the
  `V_addtable_active` check and the taskqueue being freed during module unload.
  The module-unload contract (all `O_ADDTABLE` rules must be deleted first)
  makes this race unreachable in practice; the atomic flag handles any
  theoretical residual case.

* **Table must already exist** — the kernel does not create the target table
  automatically.  Attempting to add a rule whose table does not exist is
  rejected at rule-creation time with `ESRCH` ("No such process" / table not
  found).  Create the table first with `ipfw table <n> create type addr`.

* **No duplicate suppression in the fast path** — `EEXIST` from
  `add_table_entry()` is treated as success and not logged.  Inserting an
  address that is already present is silently ignored.

* **Opcode slot** — `O_ADDTABLE` is inserted just before `O_LAST_OPCODE` in
  `enum ipfw_opcodes`.  Any binary that was compiled against the old header
  must be recompiled after applying the patches.

---

## Testing

There is no automated test suite yet.  Suggested manual checks:

```sh
# Table-existence guard: rule creation must fail before the table exists
ipfw add 9000 addtable 99 src ip from any to any
# Expected: error — table 99 not found (ESRCH)

# Verify the rule parses and shows correctly once the table exists
ipfw table 99 create type addr
ipfw add 9000 addtable 99 src ip from any to any
ipfw show 9000
# Expected: 09000 addtable 99 src ip from any to any

# Generate traffic and inspect the table (action is terminal: packet passes)
ping -c 3 127.0.0.1
ipfw table 99 list
# Expect 127.0.0.1/32 to appear

# Dst variant
ipfw table 98 create type addr
ipfw add 9001 addtable 98 dst ip from any to any
ping -c 1 8.8.8.8
ipfw table 98 list
# Expect 8.8.8.8/32 to appear

# Terminal action: a rule below addtable is NOT reached for matching packets
ipfw add 9002 deny ip from any to any
ping -c 1 127.0.0.1  # should still succeed (addtable passes, rule 9002 not seen)

# Cleanup
ipfw delete 9000 9001 9002
ipfw table 99 destroy
ipfw table 98 destroy
```

---

## License

BSD 2-Clause.  See the SPDX header in `src/sys/netpfil/ipfw/ip_fw_addtable.c`.
