/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 ipfw_addtable contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * ipfw O_ADDTABLE action: asynchronously add the source or destination IP
 * of a matching packet to an ipfw address table.
 *
 * Design notes
 * ------------
 * ipfw_chk() holds IPFW_PF_RLOCK (a reader lock on the rule chain) during
 * packet processing.  add_table_entry() internally acquires IPFW_UW_WLOCK
 * then IPFW_WLOCK, both of which conflict with the reader lock held in the
 * packet path, creating a potential deadlock.
 *
 * To avoid this, O_ADDTABLE enqueues a lightweight task to a private
 * taskqueue thread.  The task runs outside the packet path, acquires the
 * correct locks, and calls add_table_entry().  UMA is used for the task
 * structure allocation so that M_NOWAIT allocs are fast and safe from the
 * packet path.
 *
 * The action is non-terminal: after enqueueing, rule processing continues
 * to the next rule (same semantics as "count").
 *
 * VNET
 * ----
 * All mutable state (UMA zone, taskqueue, active flag) is per-VNET so that
 * destroying one VNET does not drain or free resources belonging to another.
 *
 * LOCKING / TEARDOWN RACE
 * -----------------------
 * V_addtable_active is an atomic flag: 1 while the subsystem is live, 0
 * once ipfw_addtable_destroy() has been entered.  ipfw_addtable_enqueue()
 * checks this flag and returns ENXIO early if teardown has begun.
 *
 * There is an inherent residual race: a thread that passes the flag check
 * could be preempted before it calls taskqueue_enqueue(), then teardown
 * completes and frees the taskqueue.  The primary protection against this
 * is the ipfw module-unload contract: all O_ADDTABLE rules must be deleted
 * before the module can unload, ensuring no new packets can reach the
 * O_ADDTABLE action once destroy is in progress.  The atomic flag is a
 * belt-and-suspenders guard for any in-flight packets still past the rule
 * check at that instant.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/taskqueue.h>
#include <vm/uma.h>

#include <machine/atomic.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/ip_fw.h>
#include <netinet6/in6.h>

#include "ip_fw_private.h"
#include "ip_fw_table.h"
#include "ip_fw_addtable.h"

/*
 * Per-entry context passed to the taskqueue worker.
 */
struct addtable_entry {
	struct task		 task;
	struct ip_fw_chain	*chain;
	uint16_t		 tbl;	  /* target table index */
	uint8_t			 flags;	  /* ADDTABLE_F_DST or 0 */
	uint8_t			 af;	  /* AF_INET or AF_INET6 */
	union {
		struct in_addr	 addr4;
		struct in6_addr	 addr6;
	};
};

/*
 * All mutable state is per-VNET.
 */
VNET_DEFINE_STATIC(uma_zone_t,		 addtable_zone);
VNET_DEFINE_STATIC(struct taskqueue *,	 addtable_tq);
VNET_DEFINE_STATIC(u_int,		 addtable_active);

#define	V_addtable_zone		VNET(addtable_zone)
#define	V_addtable_tq		VNET(addtable_tq)
#define	V_addtable_active	VNET(addtable_active)

/*
 * Global rate-limit state for error logging (not VNET-specific; used only
 * to suppress log flooding, correctness does not depend on per-VNET values).
 */
static struct timeval	 addtable_errtv;
static int		 addtable_errcnt;

/*
 * Worker function: runs in the per-VNET ipfw_addtable taskqueue thread.
 * Calls add_table_entry() with proper locking outside the packet path.
 */
static void
addtable_task_fn(void *context, int pending __unused)
{
	struct addtable_entry	*e = context;
	struct tid_info		 ti;
	struct tentry_info	 tei;
	int			 error;

	memset(&ti, 0, sizeof(ti));
	ti.uidx = e->tbl;
	ti.type = IPFW_TABLE_ADDR;

	memset(&tei, 0, sizeof(tei));
	tei.subtype = e->af;
	tei.masklen = (e->af == AF_INET6) ? 128 : 32;

	if (e->af == AF_INET6)
		tei.paddr = &e->addr6;
	else
		tei.paddr = &e->addr4;

	/*
	 * add_table_entry() acquires IPFW_UW_WLOCK + IPFW_WLOCK internally.
	 * EEXIST is benign: the address is already present in the table.
	 */
	error = add_table_entry(e->chain, &ti, &tei, 0, 1);
	if (error != 0 && error != EEXIST) {
		if (ppsratecheck(&addtable_errtv, &addtable_errcnt, 1))
			printf("ipfw_addtable: error %d adding entry to "
			    "table %u\n", error, (unsigned)e->tbl);
	}

	uma_zfree(V_addtable_zone, e);
}

/*
 * ipfw_addtable_init -- called once per VNET during ipfw module init.
 */
int
ipfw_addtable_init(struct ip_fw_chain *ch __unused)
{
	int error;

	V_addtable_zone = uma_zcreate("ipfw_addtable",
	    sizeof(struct addtable_entry),
	    NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, 0);
	if (V_addtable_zone == NULL)
		return (ENOMEM);

	V_addtable_tq = taskqueue_create("ipfw_addtable", M_WAITOK,
	    taskqueue_thread_enqueue, &V_addtable_tq);
	if (V_addtable_tq == NULL) {
		uma_zdestroy(V_addtable_zone);
		V_addtable_zone = NULL;
		return (ENOMEM);
	}

	error = taskqueue_start_threads(&V_addtable_tq, 1, PI_NET,
	    "ipfw_addtable");
	if (error != 0) {
		taskqueue_free(V_addtable_tq);
		V_addtable_tq = NULL;
		uma_zdestroy(V_addtable_zone);
		V_addtable_zone = NULL;
		return (error);
	}

	/* Mark subsystem live only after all resources are ready. */
	atomic_store_rel_int(&V_addtable_active, 1);

	return (0);
}

/*
 * ipfw_addtable_destroy -- called once per VNET during ipfw module unload.
 */
void
ipfw_addtable_destroy(struct ip_fw_chain *ch __unused)
{
	/*
	 * Signal teardown first so that any racing ipfw_addtable_enqueue()
	 * calls return ENXIO instead of touching the taskqueue.
	 */
	atomic_store_rel_int(&V_addtable_active, 0);

	if (V_addtable_tq != NULL) {
		taskqueue_drain_all(V_addtable_tq);
		taskqueue_free(V_addtable_tq);
		V_addtable_tq = NULL;
	}
	if (V_addtable_zone != NULL) {
		uma_zdestroy(V_addtable_zone);
		V_addtable_zone = NULL;
	}
}

/*
 * ipfw_addtable_enqueue -- called from the packet processing path
 * (IPFW_PF_RLOCK held).  Never blocks.
 *
 * Returns ENXIO if the subsystem is shutting down, ENOMEM if the UMA
 * allocation fails.  In both cases the packet continues processing
 * normally; the table addition is simply skipped.
 */
int
ipfw_addtable_enqueue(struct ip_fw_chain *ch, uint16_t tbl,
    struct ipfw_flow_id *fid, uint8_t flags)
{
	struct addtable_entry	*e;

	/*
	 * Check the active flag before touching the taskqueue.  This is the
	 * primary guard against enqueueing onto a destroyed or NULL taskqueue
	 * during module teardown (see "LOCKING / TEARDOWN RACE" above).
	 */
	if (atomic_load_acq_int(&V_addtable_active) == 0)
		return (ENXIO);

	if (V_addtable_tq == NULL)
		return (ENXIO);

	e = uma_zalloc(V_addtable_zone, M_NOWAIT);
	if (e == NULL)
		return (ENOMEM);

	e->chain = ch;
	e->tbl   = tbl;
	e->flags = flags;

	if (fid->addr_type == 6) {
		e->af = AF_INET6;
		if (flags & ADDTABLE_F_DST)
			memcpy(&e->addr6, &fid->dst_ip6,
			    sizeof(struct in6_addr));
		else
			memcpy(&e->addr6, &fid->src_ip6,
			    sizeof(struct in6_addr));
	} else {
		e->af = AF_INET;
		/* f_id stores IPv4 addresses in host byte order */
		if (flags & ADDTABLE_F_DST)
			e->addr4.s_addr = htonl(fid->dst_ip);
		else
			e->addr4.s_addr = htonl(fid->src_ip);
	}

	TASK_INIT(&e->task, 0, addtable_task_fn, e);
	taskqueue_enqueue(V_addtable_tq, &e->task);

	return (0);
}
