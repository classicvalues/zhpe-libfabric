/*
 * Copyright (c) 2014 Intel Corporation, Inc.  All rights reserved.
 * Copyright (c) 2016 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2017-2018 Hewlett Packard Enterprise Development LP.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <zhpe.h>

#define ZHPE_LOG_DBG(...) _ZHPE_LOG_DBG(FI_LOG_EP_DATA, __VA_ARGS__)
#define ZHPE_LOG_ERROR(...) _ZHPE_LOG_ERROR(FI_LOG_EP_DATA, __VA_ARGS__)

/* Debugging hook. */
#define set_rx_state(_rx_entry, _state)			\
do {							\
	struct zhpe_rx_entry *__rx_entry = (_rx_entry);	\
	__rx_entry->rx_state = (_state);		\
} while (0)

static void zhpe_pe_rx_get(struct zhpe_rx_entry *rx_entry, bool retry);

static inline void
zhpe_pe_root_update_status(struct zhpe_pe_root *pe_root,
			   int32_t status)
{
	if (OFI_UNLIKELY(status < 0) && OFI_LIKELY(pe_root->status >= 0))
	    pe_root->status = status;
}

static void zhpe_pe_report_complete(struct zhpe_cqe *zcqe,
				    int32_t err, uint64_t rem)
{
	struct zhpe_comp	*comp = zcqe->comp;
	struct zhpe_cq		*cq;
	struct fid_cq		*cq_fid;
	struct zhpe_eq		*eq;
	struct zhpe_cntr	*cntr;
	struct fid_cntr		*cntr_fid;
	uint8_t			event;
	int			rc;

	struct zhpe_triggered_context *trigger_context;

	if (OFI_UNLIKELY(zcqe->cqe.flags & ZHPE_TRIGGERED_OP)) {
		trigger_context = zcqe->cqe.op_context;
		cntr_fid = trigger_context->trigger.work.completion_cntr;
		if (cntr_fid) {
			fi_cntr_add(cntr_fid, 1);
			return;
		}
	}

	if (zcqe->cqe.flags & ZHPE_NO_COMPLETION)
		return;

	switch (zcqe->cqe.flags & (FI_SEND | FI_RECV | FI_READ | FI_WRITE |
				   FI_REMOTE_READ | FI_REMOTE_WRITE)) {

	case FI_SEND:
		cq = comp->send_cq;
		event = comp->send_cq_event;
		cntr = zcqe->comp->send_cntr;
		break;

	case FI_RECV:
		cq = comp->recv_cq;
		event = comp->recv_cq_event;
		cntr = zcqe->comp->recv_cntr;
		break;

	case FI_READ:
		cq = comp->send_cq;
		event = comp->send_cq_event;
		cntr = zcqe->comp->read_cntr;
		break;

	case FI_WRITE:
		cq = comp->send_cq;
		event = comp->send_cq_event;
		cntr = zcqe->comp->write_cntr;
		break;

	case FI_REMOTE_READ:
		cq = NULL;
		event = 0;
		cntr = zcqe->comp->rem_read_cntr;
		break;

	case FI_REMOTE_WRITE:
		cq = comp->recv_cq;
		event = 0;
		cntr = zcqe->comp->rem_write_cntr;
		break;

	default:
		ZHPE_LOG_ERROR("Unexpected flags 0x%Lx\n",
			       (ullong)zcqe->cqe.flags);
		abort();
	}
	if (OFI_UNLIKELY(err < 0)) {
		if (cntr)
			fi_cntr_adderr(&cntr->cntr_fid, 1);
		if (cq)
			zhpe_cq_report_error(cq, &zcqe->cqe, rem, -err, -err,
					     NULL, 0);
		return;
	}
	if (cntr)
		zhpe_cntr_inc(cntr);
	if (cq && (!event || (zcqe->cqe.flags & FI_COMPLETION))) {
		rc = cq->report_completion(cq, zcqe->addr,   &zcqe->cqe);
		if (rc < 0) {
			ZHPE_LOG_ERROR("Failed to report completion %p: %d\n",
				       zcqe, rc);
			eq = comp->eq;
			cq_fid = &cq->cq_fid;
			if (eq)
				zhpe_eq_report_error(eq, &cq_fid->fid,
						     cq_fid->fid.context, 0,
						     FI_ENOSPC, 0, NULL, 0);
		}
	}
}

static inline void
zhpe_pe_rx_report_complete(struct zhpe_rx_ctx *rx_ctx,
			   const struct zhpe_rx_entry *rx_entry,
			   int status, uint64_t rem)
{
	struct zhpe_cqe		zcqe = {
		.addr = rx_entry->addr,
		.comp = &rx_ctx->comp,
		.cqe = {
			.op_context = rx_entry->context,
			.flags = rx_entry->flags,
			.len = rx_entry->total_len,
			.buf = rx_entry->buf,
			.data = rx_entry->cq_data,
			.tag = rx_entry->tag,
		},
	};

	zhpe_pe_report_complete(&zcqe, status, rem);
}

void _zhpe_pe_tx_report_complete(const struct zhpe_pe_entry *pe_entry)
{
	const struct zhpe_pe_root *pe_root = &pe_entry->pe_root;
	struct zhpe_conn	*conn = pe_root->conn;
	struct zhpe_cqe		zcqe = {
		.addr = conn->fi_addr,
		.comp = &conn->tx_ctx->comp,
		.cqe = {
			.op_context = pe_root->context,
			.flags = (pe_entry->flags &
				  ~(FI_REMOTE_READ | FI_REMOTE_WRITE)),
		},
	};

	zhpe_pe_report_complete(&zcqe, pe_root->status, pe_entry->rem);
}

static void zhpe_pe_rx_discard_recv(struct zhpe_rx_entry *rx_entry,
				    bool locked)
{
	struct zhpe_conn	*conn = rx_entry->pe_root.conn;
	struct zhpe_rx_ctx	*rx_ctx = conn->rx_ctx;
	struct zhpe_msg_hdr	zhdr;

	if (!locked)
		fastlock_acquire(&rx_ctx->lock);
	dlist_remove(&rx_entry->lentry);
	if (rx_entry->rx_state == ZHPE_RX_STATE_EAGER) {
		dlist_insert_tail(&rx_entry->lentry, &rx_ctx->rx_work_list);
		set_rx_state(rx_entry, ZHPE_RX_STATE_DISCARD);
		fastlock_release(&rx_ctx->lock);
	} else {
		zhdr = rx_entry->zhdr;
		zhpe_rx_release_entry(rx_ctx, rx_entry);
		fastlock_release(&rx_ctx->lock);
		if (zhdr.flags & ZHPE_MSG_ANY_COMPLETE)
			zhpe_send_status(conn, zhdr, 0, 0);
	}
}

void zhpe_pe_rx_complete(struct zhpe_rx_ctx *rx_ctx,
			 struct zhpe_rx_entry *rx_entry,
			 int status, bool locked)
{
	struct zhpe_rx_entry	*rx_cur;
	struct dlist_entry	*dentry;
	struct dlist_entry	*dnext;
	struct dlist_entry	dcomplete;
	struct dlist_entry	ddrop;

	/* Assumed:rx_entry on work list and we are only user. */
	if (!locked)
		fastlock_acquire(&rx_ctx->lock);

	if (status >= 0 && rx_entry->rem)
		status = -FI_ETRUNC;
	zhpe_pe_root_update_status(&rx_entry->pe_root, status);
	set_rx_state(rx_entry, ZHPE_RX_STATE_COMPLETE);

	/* Grab completed entries off list and complete in order. */
	dlist_init(&dcomplete);
	dlist_init(&ddrop);
	dlist_foreach_safe(&rx_ctx->rx_work_list, dentry, dnext) {
		rx_cur = container_of(dentry, struct zhpe_rx_entry, lentry);
		if (rx_cur->rx_state == ZHPE_RX_STATE_COMPLETE) {
			dlist_remove(dentry);
			dlist_insert_tail(dentry, &dcomplete);
		} else if (rx_cur->rx_state == ZHPE_RX_STATE_DROP) {
			dlist_remove(dentry);
			dlist_insert_tail(dentry, &ddrop);
		} else
			break;
	}
	if (dlist_empty(&dcomplete) && dlist_empty(&ddrop))
		goto done;
	fastlock_release(&rx_ctx->lock);

	dlist_foreach_safe(&dcomplete, dentry, dnext) {
		rx_cur = container_of(dentry, struct zhpe_rx_entry, lentry);
		status = rx_cur->pe_root.status;

		zhpe_pe_rx_report_complete(rx_ctx, rx_cur, status,
					   rx_cur->rem);
		if (rx_cur->zhdr.flags & ZHPE_MSG_ANY_COMPLETE)
			zhpe_send_status(rx_cur->pe_root.conn,
					 rx_cur->zhdr, status, rx_cur->rem);
	}
	/* Free resources after completion to reduce latency (I hope). */
	fastlock_acquire(&rx_ctx->lock);
	dlist_foreach_safe(&dcomplete, dentry, dnext) {
		rx_cur = container_of(dentry, struct zhpe_rx_entry, lentry);
		zhpe_rx_release_entry(rx_ctx, rx_cur);
	}
	dlist_foreach_safe(&ddrop, dentry, dnext) {
		rx_cur = container_of(dentry, struct zhpe_rx_entry, lentry);
		zhpe_rx_release_entry(rx_ctx, rx_cur);
	}
 done:
	fastlock_release(&rx_ctx->lock);
	zhpe_stats_stop(&zhpe_stats_recv, true);
}

void zhpe_pe_rx_peek_recv(struct zhpe_rx_ctx *rx_ctx,
			  fi_addr_t fiaddr, uint64_t tag, uint64_t ignore,
			  uint64_t flags, struct fi_context *context)
{
	struct zhpe_rx_entry	*rx_buffered;
	struct zhpe_cqe		zcqe = {
		.comp = &rx_ctx->comp,
		.cqe = {
			.op_context = context,
		},
	};

	fastlock_acquire(&rx_ctx->lock);
	dlist_foreach_container(&rx_ctx->rx_buffered_list,
				struct zhpe_rx_entry, rx_buffered, lentry) {
		if (!zhpe_rx_match_entry(rx_buffered, true, fiaddr, tag,
					 ignore, flags))
			continue;
		goto found;
	}
	fastlock_release(&rx_ctx->lock);
	zcqe.addr = fiaddr;
	zcqe.cqe.flags = flags;
	zcqe.cqe.tag = tag;
	zhpe_pe_report_complete(&zcqe, -FI_ENOMSG, 0);
	goto done;
 found:
	zcqe.addr = rx_buffered->addr;
	zcqe.cqe.flags = rx_buffered->flags | (flags & FI_COMPLETION);
	zcqe.cqe.len = rx_buffered->total_len;
	zcqe.cqe.data = rx_buffered->cq_data;
	zcqe.cqe.tag = rx_buffered->tag;
	if (flags & FI_DISCARD) {
		zhpe_pe_rx_discard_recv(rx_buffered, true);
		/* rx_ctx->lock dropped. */
	} else if (flags & FI_CLAIM) {
		context->internal[0] = rx_buffered;
		dlist_remove(&rx_buffered->lentry);
		dlist_insert_tail(&rx_buffered->lentry, &rx_ctx->rx_work_list);
		fastlock_release(&rx_ctx->lock);
	} else
		fastlock_release(&rx_ctx->lock);
	zhpe_pe_report_complete(&zcqe, 0, 0);
 done:
	return;
}

static inline int rx_buf_alloc(struct zhpe_rx_entry *rx_buffered,
			       size_t msg_len)
{
	int			ret = -FI_ENOSPC;
	struct zhpe_conn	*conn = rx_buffered->pe_root.conn;
	struct zhpe_rx_ctx	*rx_ctx = conn->rx_ctx;
	size_t			old;

	/* It is assumed that msg_len <= zhpe_ep_max_eager_sz */
	old = __sync_fetch_and_add(&rx_ctx->buffered_len, msg_len);
	if (old + msg_len > rx_ctx->attr.total_buffered_recv) {
		__sync_fetch_and_sub(&rx_ctx->buffered_len, msg_len);
		goto done;
	}
	rx_buffered->lstate.cnt = 1;
	ret = zhpe_slab_alloc(&rx_ctx->eager, msg_len, &rx_buffered->liov[0]);
	if (ret >= 0)
		rx_buffered->buffered = ZHPE_RX_BUF_EAGER;
	else
		__sync_fetch_and_sub(&rx_ctx->buffered_len, msg_len);
 done:
	return ret;
}

static inline void rx_user_claim(struct zhpe_rx_entry *rx_buffered,
				 struct zhpe_rx_entry *rx_user,
				 bool locked, bool user_linked)
{
	struct zhpe_conn	*conn = rx_buffered->pe_root.conn;
	struct zhpe_rx_ctx	*rx_ctx = conn->rx_ctx;
	uint8_t			state;
	uint64_t		msg_len;
	uint64_t		avail;

	/* Assume: rx_buffered already on work list. */
	if (!locked)
		fastlock_acquire(&rx_ctx->lock);
	state = rx_buffered->rx_state;
	if (state == ZHPE_RX_STATE_EAGER)
		state = rx_buffered->rx_state = ZHPE_RX_STATE_EAGER_CLAIMED;
	rx_buffered->flags |= (rx_user->flags & FI_COMPLETION);
	rx_buffered->context = rx_user->context;
	/* FIXME: Assume 1 iov for now. */
	rx_buffered->ustate = rx_user->lstate;
	rx_buffered->buf = zhpe_ziov_state_ptr(&rx_buffered->ustate);
	avail = zhpe_ziov_state_avail(&rx_buffered->ustate);
	msg_len = rx_buffered->total_len;
	if (msg_len > avail)
		msg_len = avail;
	if (rx_user->flags & FI_MULTI_RECV) {
		rx_buffered->ustate.missing = 0;
		zhpe_ziov_state_adv(&rx_user->lstate, msg_len);
		if (avail - msg_len < rx_ctx->min_multi_recv) {
			rx_buffered->flags |= FI_MULTI_RECV;
			if (user_linked)
				dlist_remove(&rx_user->lentry);
			dlist_insert_tail(&rx_user->lentry,
					  &rx_ctx->rx_work_list);
			set_rx_state(rx_user, ZHPE_RX_STATE_DROP);
		} else if (!user_linked)
			dlist_insert_tail(&rx_user->lentry,
					  &rx_ctx->rx_posted_list);
	} else {
		rx_user->lstate.missing = 0;
		if (user_linked)
			dlist_remove(&rx_user->lentry);
		dlist_insert_tail(&rx_user->lentry, &rx_ctx->rx_work_list);
		set_rx_state(rx_user, ZHPE_RX_STATE_DROP);
	}
	fastlock_release(&rx_ctx->lock);

	switch (state) {

	case ZHPE_RX_STATE_RND:
		rx_buffered->riov[0].iov_len =
			(rx_buffered->riov[0].iov_len & ZHPE_ZIOV_LEN_KEY_INT) |
			msg_len;
#if 1
		set_rx_state(rx_buffered, ZHPE_RX_STATE_RND_DIRECT);
		rx_buffered->lstate = rx_buffered->ustate;
#else
		if (!(rx_user->flags & FI_INJECT)) {
			set_rx_state(rx_buffered, ZHPE_RX_STATE_RND_DIRECT);
			rx_buffered->lstate = rx_buffered->ustate;
		} else {
			if (rx_buffered->total_len > zhpe_ep_max_eager_sz ||
			    rx_buf_alloc(rx_buffered, msg_len) < 0) {
				zhpe_pe_rx_complete(rx_ctx, rx_buffered,
						    -FI_ETRUNC, false);
				goto done;
			}
			set_rx_state(rx_buffered, ZHPE_RX_STATE_RND_BUF);
		}
#endif
		zhpe_pe_rx_get(rx_buffered, false);
		goto done;

	case ZHPE_RX_STATE_EAGER_CLAIMED:
		break;

	case ZHPE_RX_STATE_EAGER_DONE:
		/* Reset lstate to beginning of buffer. */
		zhpe_ziov_state_reset(&rx_buffered->lstate);
		rx_buffered->rem = (rx_buffered->total_len -
				    copy_iov(&rx_buffered->ustate,
					     ZHPE_IOV_ZIOV,
					     &rx_buffered->lstate,
					     ZHPE_IOV_ZIOV, msg_len));
		zhpe_pe_rx_complete(rx_ctx, rx_buffered, 0, false);
		break;

	case ZHPE_RX_STATE_INLINE:
		rx_buffered->rem -= copy_mem_to_iov(&rx_buffered->ustate,
						    ZHPE_IOV_ZIOV,
						    rx_buffered->inline_data,
						    msg_len);
		rx_buffered->rstate.cnt = 0;
		zhpe_pe_rx_complete(rx_ctx, rx_buffered, 0, false);
		break;

	default:
		ZHPE_LOG_ERROR("rx_buffered %p in bad state %d\n",
			       rx_buffered, state);
		abort();
		break;
	}
 done:

	return;
}

void zhpe_pe_rx_post_recv(struct zhpe_rx_ctx *rx_ctx,
			  struct zhpe_rx_entry *rx_user)
{
	struct zhpe_rx_entry	*rx_buffered;

	fastlock_acquire(&rx_ctx->lock);
	dlist_foreach_container(&rx_ctx->rx_buffered_list,
				struct zhpe_rx_entry, rx_buffered, lentry) {
		if (!zhpe_rx_match_entry(rx_buffered, true, rx_user->addr,
					 rx_user->tag, rx_user->ignore,
					 rx_user->flags))
			continue;
		dlist_remove(&rx_buffered->lentry);
		dlist_insert_tail(&rx_buffered->lentry, &rx_ctx->rx_work_list);
		rx_user_claim(rx_buffered, rx_user, true, false);
		/* Lock is dropped. */
		goto done;
	}
	dlist_insert_tail(&rx_user->lentry, &rx_ctx->rx_posted_list);
	fastlock_release(&rx_ctx->lock);
 done:
	zhpe_pe_signal(rx_ctx->domain->pe);

	return;
}

void zhpe_pe_rx_claim_recv(struct zhpe_rx_entry *rx_claimed,
			   struct zhpe_rx_entry *rx_user)
{
	struct zhpe_rx_ctx	*rx_ctx;

	if (rx_user->flags & FI_DISCARD) {
		rx_ctx = rx_claimed->pe_root.conn->rx_ctx;
		zhpe_pe_rx_report_complete(rx_ctx, rx_user, 0, 0);
		fastlock_acquire(&rx_ctx->lock);
		zhpe_rx_release_entry(rx_ctx, rx_user);
		zhpe_pe_rx_discard_recv(rx_claimed, true);
		/* Lock will be dropped. */
		goto done;
	}
	rx_user_claim(rx_claimed, rx_user, false, false);
 done:
	return;
}

int zhpe_pe_tx_handle_entry(struct zhpe_pe_root *pe_root,
			    struct zhpeq_cq_entry *zq_cqe)
{
	struct zhpe_pe_entry	*pe_entry =
		container_of(pe_root, struct zhpe_pe_entry, pe_root);
	struct zhpe_pe_entry	*pe_entryu;

	if (zq_cqe && zq_cqe->z.status != ZHPEQ_CQ_STATUS_SUCCESS)
		zhpe_pe_root_update_status(&pe_entry->pe_root, -FI_EIO);
	pe_entry->pe_root.completions--;
	if (!pe_entry->pe_root.completions) {
		if (!(pe_entry->pe_root.flags & ZHPE_PE_PROV)) {
			zhpe_pe_tx_report_complete(pe_entry,
						   FI_TRANSMIT_COMPLETE |
						   FI_DELIVERY_COMPLETE);
		} else if ((pe_entryu = pe_entry->pe_root.context)) {
			zhpe_pe_tx_report_complete(pe_entryu,
						   FI_TRANSMIT_COMPLETE |
						   FI_DELIVERY_COMPLETE);
			zhpe_tx_release(pe_entryu->pe_root.conn, pe_entryu);
		}
		zhpe_tx_release(pe_entry->pe_root.conn, pe_entry);
	}

	return 0;
}

static int zhpe_pe_rx_handle_status(struct zhpe_conn *conn,
				    struct zhpe_msg_hdr *zhdr)
{
	union zhpe_msg_payload	*zpay;
	int32_t			status;
	struct zhpe_pe_entry	*pe_entry;

	pe_entry = &conn->ztx->pentries[ntohs(zhdr->pe_entry_id)];

	zpay = zhpe_pay_ptr(conn, zhdr, 0, alignof(*zpay));
	status = ntohl(zpay->status.status);
	zhpe_pe_root_update_status(&pe_entry->pe_root, status);
	pe_entry->rem = be64toh(zpay->status.rem);

	return pe_entry->pe_root.handler(&pe_entry->pe_root, NULL);
}

static int zhpe_pe_rx_handle_writedata(struct zhpe_conn *conn,
				       struct zhpe_msg_hdr *zhdr)
{
	struct zhpe_cqe		zcqe = {
		.addr = conn->fi_addr,
		.comp = &conn->rx_ctx->comp,
	};
	union zhpe_msg_payload	*zpay;

	zpay = zhpe_pay_ptr(conn, zhdr, 0, alignof(*zpay));
	zcqe.cqe.flags = (be64toh(zpay->writedata.flags) &
			  (FI_REMOTE_READ | FI_REMOTE_WRITE |
			   FI_REMOTE_CQ_DATA | FI_RMA | FI_ATOMIC));
	if ((zcqe.cqe.flags & (FI_REMOTE_WRITE | FI_REMOTE_CQ_DATA)) ==
	     FI_REMOTE_CQ_DATA)
	    zcqe.cqe.flags |= FI_REMOTE_WRITE;
	zcqe.cqe.data = be64toh(zpay->writedata.cq_data);
	zhpe_pe_report_complete(&zcqe, 0, 0);

	return 0;
}

#define ATOMIC_OP(_size)						\
do {									\
	switch(zpay->atomic_req.op) {					\
									\
	case FI_ATOMIC_READ:						\
		rem = *(volatile uint ## _size ## _t *)dst;		\
		break;							\
	case FI_ATOMIC_WRITE:						\
		*(volatile uint ## _size ## _t *)dst = o64;		\
		break;							\
	case FI_BAND:							\
		rem = __sync_fetch_and_and(				\
			(uint ## _size ## _t *)dst,			\
			(uint ## _size ## _t)o64);			\
		break;							\
	case FI_BOR:							\
		rem = __sync_fetch_and_or(				\
			(uint ## _size ## _t *)dst,			\
			(uint ## _size ## _t)o64);			\
		break;							\
	case FI_BXOR:							\
		rem = __sync_fetch_and_xor(				\
			(uint ## _size ## _t *)dst,			\
			(uint ## _size ## _t)o64);			\
		break;							\
	case FI_CSWAP:							\
		rem = __sync_val_compare_and_swap(			\
			(uint ## _size ## _t *)dst,			\
			(uint ## _size ## _t)c64,			\
			(uint ## _size ## _t)o64);			\
		break;							\
	case FI_SUM:							\
		rem = __sync_fetch_and_add(				\
			(uint ## _size ## _t *)dst,			\
			(uint ## _size ## _t)o64);			\
		break;							\
	}								\
} while(0)

static int zhpe_pe_rx_handle_atomic(struct zhpe_conn *conn,
				    struct zhpe_msg_hdr *zhdr)
{
	int32_t			status = -FI_ENOKEY;
	uint64_t		rem;
	union zhpe_msg_payload	*zpay;
	uint64_t		o64;
	uint64_t		c64;
	void			*dst;
	struct zhpe_mr		*zmr;
	uint64_t		dontcare;
	struct zhpe_key		zkey;

	zpay = zhpe_pay_ptr(conn, zhdr, 0, alignof(*zpay));

	o64 = be64toh(zpay->atomic_req.operand);
	c64 = be64toh(zpay->atomic_req.compare);
	dst = (void *)(uintptr_t)be64toh(zpay->atomic_req.vaddr);

	zkey.key = be64toh(zpay->atomic_req.zkey.key);
	zkey.internal = !!zpay->atomic_req.zkey.internal;
	zmr = zhpe_mr_find(conn->ep_attr->domain, &zkey);
	if (zmr) {
		status = zhpeq_lcl_key_access(
			zmr->kdata, dst, zpay->atomic_req.datasize,
			ZHPEQ_MR_GET | ZHPEQ_MR_PUT, &dontcare);
		zhpe_mr_put(zmr);
		if (status < 0)
			goto done;
	}

	status = 0;

	switch (zpay->atomic_req.datatype) {

	case FI_UINT8:
		ATOMIC_OP(8);
		break;

	case FI_UINT16:
		ATOMIC_OP(16);
		break;

	case FI_UINT32:
		ATOMIC_OP(32);
		break;

	case FI_UINT64:
		ATOMIC_OP(64);
		break;
	}
 done:
	if (zhdr->flags & ZHPE_MSG_DELIVERY_COMPLETE)
		zhpe_send_status(conn, *zhdr, status, rem);

	return 0;
}

void zhpe_pe_complete_key_response(struct zhpe_conn *conn,
				   struct zhpe_msg_hdr ohdr, int rc)
{
	struct zhpe_pe_entry	*pe_entry;

	pe_entry = &conn->ztx->pentries[ntohs(ohdr.pe_entry_id)];
	zhpe_pe_root_update_status(&pe_entry->pe_root, rc);

	pe_entry->pe_root.handler(&pe_entry->pe_root, NULL);
}

static int zhpe_pe_rx_handle_key_import(struct zhpe_conn *conn,
					struct zhpe_msg_hdr *zhdr)
{
	union zhpe_msg_payload	*zpay;
	size_t			blob_len;

	zpay = zhpe_pay_ptr(conn, zhdr, 0, alignof(*zpay));
	blob_len = zhdr->inline_len - (zpay->key_data.blob - (char *)zhdr);
	return zhpe_conn_rkey_import(conn, *zhdr, be64toh(zpay->key_data.key),
				     zpay->key_data.blob, blob_len, NULL);
}

static int zhpe_pe_rx_handle_key_request(struct zhpe_conn *conn,
					 struct zhpe_msg_hdr *zhdr)
{
	int			ret = 0;
	struct zhpe_domain	*domain;
	union zhpe_msg_payload	*zpay;
	size_t			i;
	size_t			keys;
	struct zhpe_mr		*zmr;
	struct zhpe_key		zkey;

	zpay = zhpe_pay_ptr(conn, zhdr, 0, alignof(*zpay));
	keys = ((zhdr->inline_len - ((char *)zpay - (char *)zhdr)) /
		sizeof(zpay->key_req.zkeys[0]));
	domain = conn->ep_attr->domain;
	for (i = 0; i < keys; i++) {
		if (ret >= 0) {
			memcpy(&zkey, &zpay->key_req.zkeys[i], sizeof(zkey));
			zkey.key = be64toh(zkey.key);
			zkey.internal = !!zkey.internal;
			zmr = zhpe_mr_find(domain, &zkey);
			if (zmr) {
				ret = zhpe_conn_key_export(conn, *zhdr, zmr);
				zhpe_mr_put(zmr);
			} else
				ret = -FI_ENOKEY;
		}
		if (ret < 0)
			zhpe_send_status(conn, *zhdr, ret, 0);
	}

	return 0;
}

static int zhpe_pe_rx_handle_key_revoke(struct zhpe_conn *conn,
					struct zhpe_msg_hdr *zhdr)
{
	int			ret = 0;
	union zhpe_msg_payload	*zpay;
	size_t			i;
	size_t			keys;
	struct zhpe_key		zkey;
	int			rc;

	zpay = zhpe_pay_ptr(conn, zhdr, 0, alignof(*zpay));
	keys = ((zhdr->inline_len - ((char *)zpay - (char *)zhdr)) /
		sizeof(zpay->key_req.zkeys[0]));
	for (i = 0; i < keys; i++) {
		memcpy(&zkey, &zpay->key_req.zkeys[i], sizeof(zkey));
		zkey.key = be64toh(zkey.key);
		zkey.internal = !!zkey.internal;
		rc = zhpe_conn_rkey_revoke(conn, *zhdr, &zkey);
		if (rc < 0 && ret >= 0)
			ret = rc;
	}

	return ret;
}

static int zhpe_pe_tx_handle_rx_get(struct zhpe_pe_root *pe_root,
				    struct zhpeq_cq_entry *zq_cqe)
{
	struct zhpe_rx_entry	*rx_entry =
		container_of(pe_root, struct zhpe_rx_entry, pe_root);

	zhpe_stats_start(&zhpe_stats_recv);
	if (zq_cqe->z.status != ZHPEQ_CQ_STATUS_SUCCESS)
		zhpe_pe_root_update_status(&rx_entry->pe_root, -FI_EIO);
	rx_entry->pe_root.completions--;
	zhpe_pe_rx_get(rx_entry, false);
	zhpe_stats_pause(&zhpe_stats_recv);

	return 0;
}

void zhpe_pe_retry_tx_ring1(struct zhpe_pe_retry *pe_retry)
{
	int			rc;
	int64_t			tindex = -1;
	struct zhpe_pe_entry	*pe_entry;
	struct zhpe_conn	*conn;
	struct zhpe_msg_hdr	*rhdr;
	struct zhpe_msg_hdr	*zhdr;
	uint64_t		lzaddr;
	struct zhpe_pe_root	*pe_root;

	pe_entry = pe_retry->data;
	pe_root = &pe_entry->pe_root;
	conn = pe_root->conn;
	rhdr = (void *)((char *)(pe_entry + 1) + conn->hdr_off);
	zhpe_tx_reserve_vars(rc, pe_root->handler, conn, pe_root->context,
			     tindex, pe_entry, zhdr, lzaddr, requeue,
			     (pe_root->flags & ZHPE_PE_PROV));
	memcpy(zhdr, rhdr, rhdr->inline_len);
	rc = zhpe_pe_tx_ring(pe_entry, zhdr, lzaddr, zhdr->inline_len);
	if (rc < 0) {
		ZHPE_LOG_ERROR("Retry failed %d\n", rc);
		abort();
	}
	free(pe_retry->data);
	free(pe_retry);

	return;
 requeue:
	zhpe_pe_retry_insert(conn, pe_retry);
}

void zhpe_pe_retry_tx_ring2(struct zhpe_pe_retry *pe_retry)
{
	int			rc;
	struct zhpe_pe_entry	*pe_entry;
	struct zhpe_conn	*conn;
	size_t			off;
	struct zhpe_msg_hdr	*zhdr;
	uint64_t		lzaddr;

	pe_entry = pe_retry->data;
	conn = pe_entry->pe_root.conn;
	off = zhpe_ring_off(conn, pe_entry - conn->ztx->pentries);
	zhdr = (void *)(conn->ztx->zentries + off);
	lzaddr = conn->ztx->lz_zentries + off;
	rc = zhpe_pe_tx_ring(pe_entry, zhdr, lzaddr, zhdr->inline_len);
	if (rc < 0) {
		ZHPE_LOG_ERROR("Retry failed %d\n", rc);
		abort();
	}
	free(pe_retry);
}

static void zhpe_pe_retry_rx_get(struct zhpe_pe_retry *pe_retry)
{
	zhpe_stats_start(&zhpe_stats_recv);
	zhpe_pe_rx_get(pe_retry->data, true);
	zhpe_stats_pause(&zhpe_stats_recv);
	free(pe_retry);
}

static inline int zhpe_pe_rem_setup(struct zhpe_conn *conn,
				    struct zhpe_iov_state *rstate,
				    bool get)
{
	int			ret = 0;
	struct zhpe_iov		*riov = rstate->viov;
	int			i;
	struct zhpe_key		zkey;
	struct zhpe_rkey_data	*rkey;

	for (i = ffs(rstate->missing) - 1; i >= 0;
	     i = ffs(rstate->missing) - 1) {
		zhpe_ziov_to_zkey(&riov[i], &zkey);
		rkey = zhpe_conn_rkey_get(conn, &zkey);
		if (OFI_UNLIKELY(!rkey)) {
			ZHPE_LOG_ERROR("No rkey data for 0x%Lx/%d\n",
				       (ullong)zkey.key, zkey.internal);
			ret = -FI_ENOKEY;
			break;
		}
		/* rkey no longer missing. */
		riov[i].iov_rkey = rkey;
		rstate->missing &= ~(1U << i);
		ret = zhpeq_rem_key_access(rkey->kdata, riov[i].iov_addr,
					   zhpe_ziov_len(&riov[i]),
					   (get ? ZHPEQ_MR_GET_REMOTE :
					    ZHPEQ_MR_PUT_REMOTE),
					   &riov[i].iov_zaddr);
		if (ret < 0) {
			ZHPE_LOG_ERROR("zhpeq_rem_key_access() returned %d\n",
				       ret);
			break;
		}
	}

	return ret;
}

static void zhpe_pe_rx_get(struct zhpe_rx_entry *rx_entry, bool retry)
{
	int			rc;
	size_t			max_ops;
	size_t			max_bytes;
	struct zhpe_rx_ctx	*rx_ctx;
	struct zhpe_conn	*conn;
	struct zhpe_msg_hdr	zhdr;
	uint8_t			state;

	if (OFI_UNLIKELY(rx_entry->pe_root.status < 0))
		goto complete;

	switch (rx_entry->rx_state) {

	case ZHPE_RX_STATE_EAGER:
	case ZHPE_RX_STATE_EAGER_CLAIMED:
	case ZHPE_RX_STATE_RND_BUF:
	case ZHPE_RX_STATE_RND_DIRECT:
		if (rx_entry->total_len != rx_entry->rem || retry)
			break;
		rc = 0;
		if (rx_entry->lstate.missing) {
			rx_ctx = rx_entry->pe_root.conn->rx_ctx;
			rc = zhpe_mr_reg_int_iov(
				rx_ctx->domain, &rx_entry->lstate,
				rx_entry->total_len);
		}
		if (rc >= 0)
			rc = zhpe_pe_rem_setup(rx_entry->pe_root.conn,
					       &rx_entry->rstate, true);
		if (rc < 0) {
			zhpe_pe_root_update_status(&rx_entry->pe_root, rc);
			goto complete;
		}
		break;

	case ZHPE_RX_STATE_DISCARD:
		if (rx_entry->pe_root.completions)
			goto done;
		zhpe_pe_rx_discard_recv(rx_entry, false);
		goto done;

	default:
		ZHPE_LOG_ERROR("rx_entry %p in bad state %d\n",
			       rx_entry, rx_entry->rx_state);
		abort();
	}
	if (rx_entry->pe_root.completions >= ZHPE_EP_MAX_IO_OPS)
		goto done;
	max_ops = ZHPE_EP_MAX_IO_OPS - rx_entry->pe_root.completions;
	max_bytes = rx_entry->rem;
	if (max_bytes > ZHPE_EP_MAX_IO_BYTES)
		max_bytes = ZHPE_EP_MAX_IO_BYTES;
	if (!max_bytes || !max_ops)
		goto complete;
	rc = zhpe_iov_op(&rx_entry->pe_root,
			 &rx_entry->lstate, &rx_entry->rstate,
			 max_bytes, max_ops, zhpe_iov_op_get, &rx_entry->rem);
	if (rc > 0)
		goto done;
	if (rc < 0) {
		if (rc == -FI_EAGAIN) {
			rc = zhpe_pe_retry(rx_entry->pe_root.conn,
					   zhpe_pe_retry_rx_get, rx_entry);
			if (rc >= 0)
				goto done;
		}
		zhpe_pe_root_update_status(&rx_entry->pe_root, rc);
	}

 complete:
	if (rx_entry->pe_root.completions)
		goto done;

	switch (rx_entry->rx_state) {

	case ZHPE_RX_STATE_RND_DIRECT:
		rx_ctx = rx_entry->pe_root.conn->rx_ctx;
		zhpe_pe_rx_complete(rx_ctx, rx_entry, 0, false);
		break;

	case ZHPE_RX_STATE_EAGER:
	case ZHPE_RX_STATE_EAGER_CLAIMED:
		/* We have to worry about races with peek/claim/receive.
		 * EAGER can actually be: EAGER, EAGER_CLAIMED, or DISCARD.
		 */
		conn = rx_entry->pe_root.conn;
		rx_ctx = conn->rx_ctx;
		zhdr.flags = 0;
		fastlock_acquire(&rx_ctx->lock);
		if (rx_entry->rx_state == ZHPE_RX_STATE_DISCARD) {
			zhpe_pe_rx_discard_recv(rx_entry, false);
			/* Lock is dropped. */
			break;
		}
		if (rx_entry->rx_state == ZHPE_RX_STATE_EAGER)
			set_rx_state(rx_entry, ZHPE_RX_STATE_EAGER_DONE);
		zhdr = rx_entry->zhdr;
		rx_entry->zhdr.flags &= ~ZHPE_MSG_TRANSMIT_COMPLETE;
		state = rx_entry->rx_state;
		fastlock_release(&rx_ctx->lock);
		if (zhdr.flags & ZHPE_MSG_TRANSMIT_COMPLETE)
			zhpe_send_status(conn, zhdr, rx_entry->pe_root.status,
					 rx_entry->rem);
		if (state == ZHPE_RX_STATE_EAGER_DONE)
			break;
		__attribute__ ((fallthrough));
		/* FALLTHROUGH: CLAIMED */
	case ZHPE_RX_STATE_RND_BUF:
		rx_ctx = rx_entry->pe_root.conn->rx_ctx;
		zhpe_ziov_state_reset(&rx_entry->lstate);
		rx_entry->rem =
			rx_entry->total_len -
			copy_iov(&rx_entry->ustate, ZHPE_IOV_ZIOV,
				 &rx_entry->lstate, ZHPE_IOV_ZIOV,
				 rx_entry->total_len - rx_entry->rem);
		zhpe_pe_rx_complete(rx_ctx, rx_entry, 0, false);
		break;

	default:
		ZHPE_LOG_ERROR("rx_entry %p in bad state %d\n",
			       rx_entry, rx_entry->rx_state);
	}
 done:
	return;
}

static inline void rx_riov_init(struct zhpe_rx_entry *rx_entry,
				union zhpe_msg_payload *zpay)
{
	rx_entry->riov[0].iov_len = be64toh(zpay->indirect.len);
	rx_entry->riov[0].iov_base =
		(void *)(uintptr_t)be64toh(zpay->indirect.vaddr);
	rx_entry->riov[0].iov_key = be64toh(zpay->indirect.key);
	rx_entry->riov[0].iov_zaddr = 0;
	rx_entry->rstate.viov = rx_entry->riov;
	rx_entry->rstate.off = 0;
	rx_entry->rstate.idx = 0;
	rx_entry->rstate.cnt = 1;
	rx_entry->rstate.missing = 1;
}

static inline void rx_basic_init(struct zhpe_rx_entry *rx_entry,
				 struct zhpe_conn *conn,
				 struct zhpe_msg_hdr *zhdr,
				 uint64_t msg_len, uint64_t tag,
				 uint64_t cq_data, uint64_t flags)
{
	rx_entry->pe_root.handler = zhpe_pe_tx_handle_rx_get;
	rx_entry->pe_root.conn = conn;
	rx_entry->pe_root.completions = 0;
	rx_entry->rem = msg_len;
	rx_entry->total_len = msg_len;
	rx_entry->addr = conn->fi_addr;
	rx_entry->cq_data = cq_data;
	rx_entry->tag = tag;
	rx_entry->zhdr = *zhdr;
	rx_entry->flags |= flags;
}

static inline bool rx_buffered_init(struct zhpe_rx_entry *rx_buffered,
				    struct zhpe_msg_hdr *zhdr,
				    union zhpe_msg_payload *zpay,
				    struct zhpe_rx_entry *rx_user)
{
	struct zhpe_conn	*conn = rx_buffered->pe_root.conn;
	void			*src;

	rx_buffered->buffered = ZHPE_RX_BUF;
	if (rx_buffered->zhdr.flags & ZHPE_MSG_INLINE) {
		src = zhpe_pay_ptr(conn, zhdr, 0, sizeof(int));
		memcpy(rx_buffered->inline_data, src,
		       rx_buffered->total_len);
		set_rx_state(rx_buffered, ZHPE_RX_STATE_INLINE);
		return false;
	}
	rx_riov_init(rx_buffered, zpay);
	if (rx_user || rx_buffered->total_len > zhpe_ep_max_eager_sz) {
		set_rx_state(rx_buffered, ZHPE_RX_STATE_RND);
		return false;
	}
	set_rx_state(rx_buffered, ZHPE_RX_STATE_EAGER);

	return true;
}

static int zhpe_pe_rx_handle_send(struct zhpe_conn *conn,
				  struct zhpe_msg_hdr *zhdr)
{
	int			ret = 0;
	uint64_t		flags = 0;
	struct zhpe_rx_ctx	*rx_ctx = conn->rx_ctx;
	uint64_t		tag = 0;
	uint64_t		cq_data = 0;
	union zhpe_msg_payload	*zpay = NULL;
	struct zhpe_rx_entry	*rx_entry;
	struct zhpe_rx_entry	*rx_posted;
	uint64_t		msg_len;
	uint64_t		*data;
	void			*src;

	if (zhdr->flags & ZHPE_MSG_INLINE) {
		msg_len = zhdr->inline_len;
		data = zhpe_pay_ptr(conn, zhdr, msg_len, alignof(*data));
		if (zhdr->flags & ZHPE_MSG_TAGGED) {
			flags |= FI_TAGGED;
			tag = be64toh(*data++);
		}
		if (zhdr->flags & ZHPE_MSG_REMOTE_CQ_DATA) {
			flags |= FI_REMOTE_CQ_DATA;
			cq_data = be64toh(*data++);
		}
	}  else {
		zpay = zhpe_pay_ptr(conn, zhdr, 0, alignof(*zpay));
		msg_len = be64toh(zpay->indirect.len) & ~ZHPE_ZIOV_LEN_KEY_INT;
		if (zhdr->flags & ZHPE_MSG_TAGGED) {
			flags |= FI_TAGGED;
			tag = be64toh(zpay->indirect.tag);
		}
		if (zhdr->flags & ZHPE_MSG_REMOTE_CQ_DATA) {
			flags |= FI_REMOTE_CQ_DATA;
			cq_data = be64toh(zpay->indirect.cq_data);
		}
	}

	fastlock_acquire(&rx_ctx->lock);
	dlist_foreach_container(&rx_ctx->rx_posted_list, struct zhpe_rx_entry,
				rx_entry, lentry) {
		if (!zhpe_rx_match_entry(rx_entry, false, conn->fi_addr, tag,
					 rx_entry->ignore, flags))
			continue;
		goto found;
	}
	rx_entry = zhpe_rx_new_entry(rx_ctx);
	if (!rx_entry) {
		ret = -FI_ENOMEM;
		fastlock_release(&rx_ctx->lock);
		goto done;
	}
	rx_basic_init(rx_entry, conn, zhdr, msg_len, tag, cq_data, flags);
	dlist_insert_tail(&rx_entry->lentry, &rx_ctx->rx_buffered_list);
	if (!rx_buffered_init(rx_entry, zhdr, zpay, NULL)) {
		fastlock_release(&rx_ctx->lock);
		goto done;
	}
	if (rx_buf_alloc(rx_entry, msg_len) < 0) {
		/* Eager allocation failed, go with rendezvous. */
		set_rx_state(rx_entry, ZHPE_RX_STATE_RND);
		fastlock_release(&rx_ctx->lock);
	} else {
		fastlock_release(&rx_ctx->lock);
		zhpe_pe_rx_get(rx_entry, false);
	}
	goto done;
 found:
	/* Found a user entry, but do we still need a buffer entry? */
	if (rx_entry->flags & FI_MULTI_RECV) {
		/* We need to buffer. */
		rx_posted = rx_entry;
		rx_entry = zhpe_rx_new_entry(rx_ctx);
		if (!rx_entry) {
			ret = -FI_ENOMEM;
			fastlock_release(&rx_ctx->lock);
			goto done;
		}
		rx_basic_init(rx_entry, conn, zhdr, msg_len, tag, cq_data,
			      flags);
		dlist_insert_tail(&rx_entry->lentry, &rx_ctx->rx_work_list);
		rx_buffered_init(rx_entry, zhdr, zpay, rx_posted);
		rx_user_claim(rx_entry, rx_posted, true, true);
		goto done;
	}
	/* A single posted receive */
	dlist_remove(&rx_entry->lentry);
	dlist_insert_tail(&rx_entry->lentry, &rx_ctx->rx_work_list);
	fastlock_release(&rx_ctx->lock);
	rx_basic_init(rx_entry, conn, zhdr, msg_len, tag, cq_data, flags);
	rx_entry->buf = zhpe_ziov_state_ptr(&rx_entry->lstate);
	if (rx_entry->zhdr.flags & ZHPE_MSG_INLINE) {
		src = zhpe_pay_ptr(conn, zhdr, 0, sizeof(int));
		rx_entry->rem -= copy_mem_to_iov(&rx_entry->lstate,
						 ZHPE_IOV_ZIOV, src, msg_len);
		zhpe_pe_rx_complete(rx_ctx, rx_entry, 0, false);
		goto done;
	}
	rx_riov_init(rx_entry, zpay);
	set_rx_state(rx_entry, ZHPE_RX_STATE_RND_DIRECT);
	zhpe_pe_rx_get(rx_entry, false);
 done:
	if (ret < 0)
		ZHPE_LOG_ERROR("Error %d\n", ret);

	return ret;
}

int zhpe_pe_tx_handle_rma(struct zhpe_pe_root *pe_root,
			  struct zhpeq_cq_entry *zq_cqe)
{
	struct zhpe_pe_entry	*pe_entry =
		container_of(pe_root, struct zhpe_pe_entry, pe_root);

	pe_entry->pe_root.completions--;
	if (zq_cqe) {
		if (zq_cqe->z.status != ZHPEQ_CQ_STATUS_SUCCESS)
			zhpe_pe_root_update_status(&pe_entry->pe_root, -FI_EIO);
		if (!pe_entry->pe_root.completions &&
		    ((pe_entry->flags & (FI_INJECT | FI_READ)) ==
		     (FI_INJECT | FI_READ)))
			copy_mem_to_iov(&pe_entry->lstate, ZHPE_IOV_ZIOV,
					zq_cqe->z.result.data, ZHPEQ_IMM_MAX);
	}
	zhpe_pe_tx_rma(pe_entry);

	return 0;
}

static void zhpe_pe_retry_tx_rma(struct zhpe_pe_retry *pe_retry)
{
	zhpe_pe_tx_rma(pe_retry->data);
	free(pe_retry);
}

static int zhpe_pe_writedata(struct zhpe_pe_entry *pe_entry)
{
	struct zhpe_msg_hdr	ohdr;
	struct zhpe_msg_writedata writedata;

	ohdr.op_type = ZHPE_OP_WRITEDATA;
	ohdr.rx_id = pe_entry->rx_id;
	writedata.flags = htobe64(pe_entry->flags);
	writedata.cq_data = htobe64(pe_entry->cq_data);

	return zhpe_tx_op(pe_entry->pe_root.conn, ohdr,
			  ZHPE_PE_PROV | ZHPE_PE_RETRY,
			  &writedata, sizeof(writedata), pe_entry);
}

void zhpe_pe_tx_rma(struct zhpe_pe_entry *pe_entry)
{
	int			rc;
	size_t			max_ops;
	size_t			max_bytes;

	if (OFI_UNLIKELY(pe_entry->pe_root.status < 0))
		goto complete;

	if (OFI_UNLIKELY(pe_entry->pe_root.flags & ZHPE_PE_KEY_WAIT)) {
		if (pe_entry->pe_root.completions)
			goto done;
		pe_entry->pe_root.flags &= ~ZHPE_PE_KEY_WAIT;
		rc = zhpe_pe_rem_setup(pe_entry->pe_root.conn,
				       &pe_entry->rstate,
				       !(pe_entry->flags & FI_WRITE));
		zhpe_pe_root_update_status(&pe_entry->pe_root, rc);
		if (rc < 0)
			goto complete;
	}
	if (OFI_UNLIKELY(pe_entry->pe_root.completions >= ZHPE_EP_MAX_IO_OPS))
		goto done;

	if (pe_entry->flags & FI_INJECT) {
		if (pe_entry->flags & FI_READ)
			rc = zhpe_iov_to_get_imm(
				&pe_entry->pe_root, pe_entry->rem,
				&pe_entry->rstate, &pe_entry->rem);
		else
			rc = zhpe_put_imm_to_iov(
				&pe_entry->pe_root, pe_entry->inline_data,
				pe_entry->rem, &pe_entry->rstate,
				&pe_entry->rem);
	} else {
		max_ops = ZHPE_EP_MAX_IO_OPS - pe_entry->pe_root.completions;
		max_bytes = pe_entry->rem;
		if (max_bytes > ZHPE_EP_MAX_IO_BYTES)
			max_bytes = ZHPE_EP_MAX_IO_BYTES;
		if (!max_bytes || !max_ops)
			goto complete;
		rc = zhpe_iov_op(&pe_entry->pe_root,
				 &pe_entry->lstate, &pe_entry->rstate,
				 max_bytes, max_ops,
				 ((pe_entry->flags & FI_READ) ?
				  zhpe_iov_op_get : zhpe_iov_op_put),
				 &pe_entry->rem);
	}
	if (rc > 0)
		goto done;
	if (rc < 0) {
		if (rc == -FI_EAGAIN) {
			rc = zhpe_pe_retry(pe_entry->pe_root.conn,
					   zhpe_pe_retry_tx_rma, pe_entry);
			if (rc >= 0)
				goto done;
		}
		zhpe_pe_root_update_status(&pe_entry->pe_root, rc);
	}

 complete:
	if (pe_entry->pe_root.completions)
		goto done;

	if (pe_entry->flags &
	    (FI_REMOTE_READ | FI_REMOTE_WRITE | FI_REMOTE_CQ_DATA)) {
		rc = zhpe_pe_writedata(pe_entry);
		if (rc >= 0)
			goto done;
	}
	zhpe_pe_tx_report_complete(pe_entry,
				   FI_TRANSMIT_COMPLETE |
				   FI_DELIVERY_COMPLETE);
	zhpe_tx_release(pe_entry->pe_root.conn, pe_entry);
 done:
	return;
}

void zhpe_pe_rkey_request(struct zhpe_conn *conn, struct zhpe_msg_hdr ohdr,
			  struct zhpe_iov_state *rstate, uint8_t *completions)
{
	struct zhpe_iov		*ziov = rstate->viov;
	uint			missing = rstate->missing;
	int			i;
	uint			j;
	struct zhpe_msg_key_request key_req;
	struct zhpe_key		zkey;

	for (i = ffs(missing) - 1, j = 0; i >= 0;
	     (missing &= ~(1U << i), i = ffs(missing) - 1)) {
		zhpe_ziov_to_zkey(&ziov[i], &zkey);
		memcpy(&key_req.zkeys[j++], &zkey, sizeof(key_req.zkeys[0]));
		(*completions)++;
	}
	ohdr.op_type = ZHPE_OP_KEY_REQUEST;
	zhpe_prov_op(conn, ohdr, ZHPE_PE_RETRY,
		     &key_req, sizeof(key_req.zkeys[0]) * j);
}

int zhpe_pe_tx_handle_atomic(struct zhpe_pe_root *pe_root,
			     struct zhpeq_cq_entry *zq_cqe)
{
	struct zhpe_pe_entry	*pe_entry =
		container_of(pe_root, struct zhpe_pe_entry, pe_root);
	int			rc;

	if (zq_cqe && zq_cqe->z.status != ZHPEQ_CQ_STATUS_SUCCESS)
		zhpe_pe_root_update_status(&pe_entry->pe_root, -FI_EIO);
	pe_entry->pe_root.completions--;
	if (!pe_entry->pe_root.completions) {
		if (pe_entry->result) {
			switch (pe_entry->result_type) {

			case FI_UINT8:
				*(uint8_t *)pe_entry->result = pe_entry->rem;
				break;

			case FI_UINT16:
				*(uint16_t *)pe_entry->result = pe_entry->rem;
				break;

			case FI_UINT32:
				*(uint32_t *)pe_entry->result = pe_entry->rem;
				break;

			case FI_UINT64:
				*(uint64_t *)pe_entry->result = pe_entry->rem;
				break;
			}
		}

		if (pe_entry->flags & FI_REMOTE_CQ_DATA) {
			rc = zhpe_pe_writedata(pe_entry);
			if (rc >= 0)
				goto done;
		}
		zhpe_pe_tx_report_complete(pe_entry,
					   FI_TRANSMIT_COMPLETE |
					   FI_DELIVERY_COMPLETE);
		zhpe_tx_release(pe_entry->pe_root.conn, pe_entry);
	}
 done:

	return 0;
}

void zhpe_pe_signal(struct zhpe_pe *pe)
{
	char c = 0;
	if (pe->domain->progress_mode != FI_PROGRESS_AUTO)
		return;

	fastlock_acquire(&pe->signal_lock);
	if (pe->wcnt == pe->rcnt) {
		if (ofi_write_socket(pe->signal_fds[ZHPE_SIGNAL_WR_FD], &c, 1)
		    != 1)
			ZHPE_LOG_ERROR("Failed to signal\n");
		else
			pe->wcnt++;
	}
	fastlock_release(&pe->signal_lock);
}

void zhpe_pe_add_tx_ctx(struct zhpe_pe *pe,
			struct zhpe_tx_ctx *ctx)
{
	struct zhpe_tx_ctx *curr_ctx;

	mutex_acquire(&pe->list_lock);
	dlist_foreach_container(&pe->tx_list, struct zhpe_tx_ctx,
				curr_ctx, pe_lentry) {
		if (curr_ctx == ctx)
			goto out;
	}
	dlist_insert_tail(&ctx->pe_lentry, &pe->tx_list);
	zhpe_pe_signal(pe);
out:
	mutex_release(&pe->list_lock);
	ZHPE_LOG_DBG("TX ctx added to PE\n");
}

void zhpe_pe_add_rx_ctx(struct zhpe_pe *pe,
			struct zhpe_rx_ctx *ctx)
{
	struct zhpe_rx_ctx *curr_ctx;

	mutex_acquire(&pe->list_lock);
	dlist_foreach_container(&pe->tx_list, struct zhpe_rx_ctx,
				curr_ctx, pe_lentry) {
		if (curr_ctx == ctx)
			goto out;
	}
	dlist_insert_tail(&ctx->pe_lentry, &pe->rx_list);
	zhpe_pe_signal(pe);
out:
	mutex_release(&pe->list_lock);
	ZHPE_LOG_DBG("RX ctx added to PE\n");
}

void zhpe_pe_remove_tx_ctx(struct zhpe_tx_ctx *tx_ctx)
{
	mutex_acquire(&tx_ctx->domain->pe->list_lock);
	dlist_remove(&tx_ctx->pe_lentry);
	mutex_release(&tx_ctx->domain->pe->list_lock);
}

void zhpe_pe_remove_rx_ctx(struct zhpe_rx_ctx *rx_ctx)
{
	mutex_acquire(&rx_ctx->domain->pe->list_lock);
	dlist_remove(&rx_ctx->pe_lentry);
	mutex_release(&rx_ctx->domain->pe->list_lock);
}

static int zhpe_pe_progress_rx_ep(struct zhpe_pe *pe,
				  struct zhpe_ep_attr *ep_attr,
				  struct zhpe_rx_ctx *rx_ctx)
{
	int			ret = 0;
	bool			map_locked = false;
	struct zhpe_rx_local	*rx_ringl;
	int			i;
	struct zhpe_conn	*conn;
	struct zhpe_conn_map	*map;
	struct zhpe_msg_hdr	*zhdr;
	uint32_t		rindex;
	uint32_t		idx;
	uint8_t			valid;

	map = &ep_attr->cmap;
	if (!map->used)
		goto done;

	/* Poll all connections for traffic. */
	/* FIXME: think about how to poll more efficiently. */
	for (i = 0;; i++) {
		if (!map_locked) {
			mutex_acquire(&map->mutex);
			map_locked = true;
		}
		if (i >= map->used)
			break;
		conn = &map->table[i];
		if (conn->state != ZHPE_CONN_STATE_READY)
			continue;

		/* Read new entries in ring. */
		rx_ringl = &conn->rx_local;
		for (rindex = rx_ringl->head; ; rindex++) {
			idx = rindex & rx_ringl->cmn.mask;
			valid = ((rindex & (rx_ringl->cmn.mask + 1)) ? 0 :
				 ZHPE_MSG_VALID_TOGGLE);
			zhdr = (void *)(rx_ringl->zentries +
					zhpe_ring_off(conn, idx));
			if ((zhdr->flags & ZHPE_MSG_VALID_TOGGLE) != valid)
				break;

			switch (zhdr->op_type) {

			case ZHPE_OP_ATOMIC:
				ret = zhpe_pe_rx_handle_atomic(conn, zhdr);
				break;

			case ZHPE_OP_KEY_EXPORT:
			case ZHPE_OP_KEY_RESPONSE:
				ret = zhpe_pe_rx_handle_key_import(conn, zhdr);
				break;

			case ZHPE_OP_KEY_REQUEST:
				ret = zhpe_pe_rx_handle_key_request(
					conn, zhdr);
				break;

			case ZHPE_OP_KEY_REVOKE:
				ret = zhpe_pe_rx_handle_key_revoke(conn, zhdr);
				break;

			case ZHPE_OP_SEND:
				zhpe_stats_start(&zhpe_stats_recv);
				ret = zhpe_pe_rx_handle_send(conn, zhdr);
				zhpe_stats_pause(&zhpe_stats_recv);
				break;

			case ZHPE_OP_STATUS:
				ret = zhpe_pe_rx_handle_status(conn, zhdr);
				break;

			case ZHPE_OP_WRITEDATA:
				ret = zhpe_pe_rx_handle_writedata(conn, zhdr);
				break;

			default:
				ret = -FI_ENOSYS;
				ZHPE_LOG_ERROR("Illegal opcode %d\n",
					       zhdr->op_type);
				break;
			}
			/* Track completions so information are what
			 * entries are free can flow back to tx side.
			 */
			zhpe_rx_local_release(conn, idx);
		}
		rx_ringl->head = rindex;
		mutex_release(&map->mutex);
		map_locked = false;
		if (OFI_UNLIKELY(ret) < 0)
			break;
	}
 done:
	if (map_locked)
		mutex_release(&map->mutex);

	return ret;
}

int zhpe_pe_progress_rx_ctx(struct zhpe_pe *pe,
			    struct zhpe_rx_ctx *rx_ctx)
{
	int			ret = 0;
	struct zhpe_ep_attr	*ep_attr;

	/* check for incoming data */
	if (rx_ctx->ctx.fid.fclass == FI_CLASS_SRX_CTX) {
		dlist_foreach_container(&rx_ctx->ep_list, struct zhpe_ep_attr,
					ep_attr, rx_ctx_lentry) {
			ret = zhpe_pe_progress_rx_ep(pe, ep_attr, rx_ctx);
			if (ret < 0)
				goto out;
		}
	} else {
		ep_attr = rx_ctx->ep_attr;
		ret = zhpe_pe_progress_rx_ep(pe, ep_attr, rx_ctx);
		if (ret < 0)
			goto out;
	}

out:
	if (ret < 0)
		ZHPE_LOG_ERROR("failed to progress RX ctx\n");
	return ret;
}

int zhpe_pe_progress_tx_ctx(struct zhpe_pe *pe,
			    struct zhpe_tx_ctx *tx_ctx)
{
	int			ret = 0;
	struct zhpe_ep_attr	*ep_attr = tx_ctx->ep_attr;
	struct dlist_entry	*dentry;
	struct dlist_entry	*dnext;
	struct dlist_entry	head;
	struct zhpeq_cq_entry	zq_cqe[ZHPE_RING_TX_CQ_ENTRIES];
	ssize_t			entries;
	ssize_t			i;
	void			*context;
	struct zhpe_conn_map	*map;
	struct zhpe_pe_root	*pe_root;
	struct zhpe_pe_retry	*pe_retry;

	map = &ep_attr->cmap;
	mutex_acquire(&map->mutex);
	if (!ep_attr->ztx)
		goto done;
	entries = zhpeq_cq_read(ep_attr->ztx->zq, zq_cqe, ARRAY_SIZE(zq_cqe));
	if (entries < 0) {
		ret = entries;
		ZHPE_LOG_ERROR("zhpeq_cq_read() error %d\n", ret);
		goto done;
	}
	for (i = 0; i < entries; i++) {
		context = zq_cqe[i].z.context;
		if (context == ZHPE_CONTEXT_IGNORE_PTR) {
			if (zq_cqe[i].z.status == ZHPEQ_CQ_STATUS_SUCCESS)
				continue;
			ZHPE_LOG_ERROR("Send of control I/O failed\n");
			ret = -EIO;
			goto done;
		}
		pe_root = context;
		ret = pe_root->handler(pe_root, &zq_cqe[i]);
		if (ret < 0)
			goto done;
	}

	if (!dlist_empty(&ep_attr->pe_retry_list)) {
		/* Snarf existing list and process it. */
		fastlock_acquire(&ep_attr->pe_retry_lock);
		dlist_init(&head);
		dlist_splice_tail(&head, &ep_attr->pe_retry_list);
		dlist_init(&ep_attr->pe_retry_list);
		fastlock_release(&ep_attr->pe_retry_lock);
		dlist_foreach_safe(&head, dentry, dnext) {
			pe_retry = container_of(dentry,
						struct zhpe_pe_retry, lentry);
			pe_retry->handler(pe_retry);
		}
	}
 done:
	mutex_release(&map->mutex);
	if (ret < 0)
		ZHPE_LOG_ERROR("failed to progress TX ctx\n");
	return ret;
}

static int zhpe_pe_wait_ok(struct zhpe_pe *pe)
{
	struct zhpe_tx_ctx	*tx_ctx;
	struct zhpe_rx_ctx	*rx_ctx;
	struct zhpe_tx		*ztx;

	if (pe->waittime &&
	    ((fi_gettime_ms() - pe->waittime) < (uint64_t)zhpe_pe_waittime))
		return 0;

	dlist_foreach_container(&pe->tx_list, struct zhpe_tx_ctx,
				tx_ctx, pe_lentry) {
		if (!dlist_empty(&tx_ctx->ep_attr->pe_retry_list))
			return 0;
		/* FIXME:Should the tx_ctx have a direct pointer to the ztx? */
		ztx = tx_ctx->ep_attr->ztx;
		if (ztx && ztx->ufree.count + ztx->pfree.count <= ztx->mask)
			return 0;
	}

	dlist_foreach_container(&pe->rx_list, struct zhpe_rx_ctx,
				rx_ctx, pe_lentry) {
		/* rx_entry_list check is racy, but signal will fix */
		if (!dlist_empty(&rx_ctx->rx_posted_list) ||
		    !dlist_empty(&rx_ctx->rx_buffered_list))
			return 0;
	}

	return 1;
}

static void zhpe_pe_wait(struct zhpe_pe *pe)
{
	int		read_fd = pe->signal_fds[ZHPE_SIGNAL_RD_FD];
	char		tmp;
	int		rc;
	struct pollfd	pollfd;

	pollfd.fd = read_fd;
	pollfd.events = POLLIN;
	rc = poll(&pollfd, 1, 1);
	if (rc == -1)
		ZHPE_LOG_ERROR("poll failed : %s\n", strerror(errno));

	if (rc > 0) {
		fastlock_acquire(&pe->signal_lock);
		if (pe->rcnt != pe->wcnt) {
			if (ofi_read_socket(read_fd, &tmp, 1) == 1)
				pe->rcnt++;
			else
				ZHPE_LOG_ERROR("Invalid signal\n");
		}
		fastlock_release(&pe->signal_lock);
	}
	pe->waittime = fi_gettime_ms();
}

#if !defined __APPLE__ && !defined _WIN32
static void zhpe_thread_set_affinity(char *s)
{
	char *saveptra = NULL, *saveptrb = NULL, *saveptrc = NULL;
	char *a, *b, *c;
	int j, first, last, stride;
	cpu_set_t mycpuset;
	pthread_t mythread;

	mythread = pthread_self();
	CPU_ZERO(&mycpuset);

	a = strtok_r(s, ",", &saveptra);
	while (a) {
		first = last = -1;
		stride = 1;
		b = strtok_r(a, "-", &saveptrb);
		assert(b);
		first = atoi(b);
		/* Check for range delimiter */
		b = strtok_r(NULL, "-", &saveptrb);
		if (b) {
			c = strtok_r(b, ":", &saveptrc);
			assert(c);
			last = atoi(c);
			/* Check for stride */
			c = strtok_r(NULL, ":", &saveptrc);
			if (c)
				stride = atoi(c);
		}

		if (last == -1)
			last = first;

		for (j = first; j <= last; j += stride)
			CPU_SET(j, &mycpuset);
		a =  strtok_r(NULL, ",", &saveptra);
	}

	j = pthread_setaffinity_np(mythread, sizeof(cpu_set_t), &mycpuset);
	if (j != 0)
		ZHPE_LOG_ERROR("pthread_setaffinity_np failed\n");
}
#endif

static void zhpe_pe_set_affinity(void)
{
	if (zhpe_pe_affinity_str == NULL)
		return;

#if !defined __APPLE__ && !defined _WIN32
	zhpe_thread_set_affinity(zhpe_pe_affinity_str);
#else
	ZHPE_LOG_ERROR("*** FI_SOCKETS_PE_AFFINITY is not supported on OS X\n");
#endif
}

static void *zhpe_pe_progress_thread(void *data)
{
	struct zhpe_pe		*pe = (struct zhpe_pe *)data;
	bool			locked = false;
	int			rc;
	struct zhpe_tx_ctx	*tx_ctx;
	struct zhpe_rx_ctx	*rx_ctx;

	ZHPE_LOG_DBG("Progress thread started\n");
	zhpe_pe_set_affinity();
	while (atomic_load(&pe->do_progress)) {
		mutex_acquire(&pe->list_lock);
		locked = false;
		if (pe->domain->progress_mode == FI_PROGRESS_AUTO &&
		    zhpe_pe_wait_ok(pe)) {
			mutex_release(&pe->list_lock);
			zhpe_pe_wait(pe);
			mutex_acquire(&pe->list_lock);
		}
		dlist_foreach_container(&pe->tx_list, struct zhpe_tx_ctx,
					tx_ctx, pe_lentry) {
			rc = zhpe_pe_progress_tx_ctx(pe, tx_ctx);
			if (rc < 0)
				goto done;
		}
		dlist_foreach_container(&pe->rx_list, struct zhpe_rx_ctx,
					rx_ctx, pe_lentry) {
			rc = zhpe_pe_progress_rx_ctx(pe, rx_ctx);
			if (rc < 0)
				goto done;
		}
		/* Unlock to allow things in. */
		locked = false;
		mutex_release(&pe->list_lock);
	}

 done:
	if (locked)
		mutex_release(&pe->list_lock);
	ZHPE_LOG_DBG("Progress thread terminated\n");
	return NULL;
}

struct zhpe_pe *zhpe_pe_init(struct zhpe_domain *domain)
{
	struct zhpe_pe *pe;

	pe = calloc(1, sizeof(*pe));
	if (!pe)
		return NULL;

	dlist_init(&pe->tx_list);
	dlist_init(&pe->rx_list);
	fastlock_init(&pe->signal_lock);
	mutex_init(&pe->list_lock, NULL);
	pe->domain = domain;

	if (domain->progress_mode == FI_PROGRESS_AUTO) {
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, pe->signal_fds) < 0)
			goto err3;

		fi_fd_nonblock(pe->signal_fds[ZHPE_SIGNAL_RD_FD]);

		atomic_store_lazy_uint32(&pe->do_progress, 1);
		if (pthread_create(&pe->progress_thread, NULL,
				   zhpe_pe_progress_thread, (void *)pe)) {
			ZHPE_LOG_ERROR("Couldn't create progress thread\n");
			goto err5;
		}
	}
	ZHPE_LOG_DBG("PE init: OK\n");
	return pe;

err5:
	ofi_close_socket(pe->signal_fds[0]);
	ofi_close_socket(pe->signal_fds[1]);
err3:
	mutex_destroy(&pe->list_lock);
	free(pe);
	return NULL;
}

void zhpe_pe_finalize(struct zhpe_pe *pe)
{
	if (pe->domain->progress_mode == FI_PROGRESS_AUTO) {
		atomic_store_lazy_uint32(&pe->do_progress, 0);
		zhpe_pe_signal(pe);
		pthread_join(pe->progress_thread, NULL);
		ofi_close_socket(pe->signal_fds[0]);
		ofi_close_socket(pe->signal_fds[1]);
	}

	fastlock_destroy(&pe->signal_lock);
	mutex_destroy(&pe->list_lock);
	free(pe);
	ZHPE_LOG_DBG("Progress engine finalize: OK\n");
}
