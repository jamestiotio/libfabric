/*
 * Copyright (c) 2013-2016 Intel Corporation. All rights reserved.
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

#include <stdlib.h>
#include <string.h>

#include <ofi_enosys.h>
#include <ofi_util.h>

#define UTIL_DEF_CQ_SIZE (1024)


/* While the CQ is full, we continue to add new entries to the auxiliary
 * queue.
 */
static void ofi_cq_insert_aux(struct util_cq *cq,
			      struct util_cq_aux_entry *entry)
{
	if (!ofi_cirque_isfull(cq->cirq))
		ofi_cirque_commit(cq->cirq);

	entry->cq_slot = ofi_cirque_tail(cq->cirq);
	entry->cq_slot->flags = UTIL_FLAG_AUX;
	slist_insert_tail(&entry->list_entry, &cq->aux_queue);
}

int ofi_cq_write_overflow(struct util_cq *cq, void *context, uint64_t flags,
			  size_t len, void *buf, uint64_t data, uint64_t tag,
			  fi_addr_t src)
{
	struct util_cq_aux_entry *entry;

	assert(ofi_mutex_held(&cq->cq_lock));
	FI_DBG(cq->domain->prov, FI_LOG_CQ, "writing to CQ overflow list\n");
	assert(ofi_cirque_freecnt(cq->cirq) <= 1);

	if (!(entry = calloc(1, sizeof(*entry))))
		return -FI_ENOMEM;

	entry->comp.op_context = context;
	entry->comp.flags = flags;
	entry->comp.len = len;
	entry->comp.buf = buf;
	entry->comp.data = data;
	entry->comp.tag = tag;
	entry->comp.err = 0;
	entry->src = src;

	ofi_cq_insert_aux(cq, entry);
	return 0;
}

int ofi_cq_insert_error(struct util_cq *cq,
			const struct fi_cq_err_entry *err_entry)
{
	struct util_cq_aux_entry *entry;

	assert(ofi_mutex_held(&cq->cq_lock));
	assert(err_entry->err);
	if (!(entry = calloc(1, sizeof(*entry))))
		return -FI_ENOMEM;

	entry->comp = *err_entry;
	ofi_cq_insert_aux(cq, entry);
	return 0;
}

int ofi_cq_write_error(struct util_cq *cq,
		       const struct fi_cq_err_entry *err_entry)
{
	cq->cq_mutex_lock(&cq->cq_lock);
	ofi_cq_insert_error(cq, err_entry);
	cq->cq_mutex_unlock(&cq->cq_lock);

	if (cq->wait)
		cq->wait->signal(cq->wait);
	return 0;
}

int ofi_cq_write_error_peek(struct util_cq *cq, uint64_t tag, void *context)
{
	struct fi_cq_err_entry err_entry = {
		.op_context	= context,
		.flags		= FI_TAGGED | FI_RECV,
		.tag		= tag,
		.err		= FI_ENOMSG,
		.prov_errno	= -FI_ENOMSG,
	};
	return ofi_cq_write_error(cq, &err_entry);
}

int ofi_cq_write_error_trunc(struct util_cq *cq, void *context, uint64_t flags,
			     size_t len, void *buf, uint64_t data, uint64_t tag,
			     size_t olen)
{
	struct fi_cq_err_entry err_entry = {
		.op_context	= context,
		.flags		= flags,
		.len		= len,
		.buf		= buf,
		.data		= data,
		.tag		= tag,
		.olen		= olen,
		.err		= FI_ETRUNC,
		.prov_errno	= -FI_ETRUNC,
	};
	return ofi_cq_write_error(cq, &err_entry);
}

int ofi_check_cq_attr(const struct fi_provider *prov,
		      const struct fi_cq_attr *attr)
{
	switch (attr->format) {
	case FI_CQ_FORMAT_UNSPEC:
	case FI_CQ_FORMAT_CONTEXT:
	case FI_CQ_FORMAT_MSG:
	case FI_CQ_FORMAT_DATA:
	case FI_CQ_FORMAT_TAGGED:
		break;
	default:
		FI_WARN(prov, FI_LOG_CQ, "unsupported format\n");
		return -FI_EINVAL;
	}

	switch (attr->wait_obj) {
	case FI_WAIT_NONE:
	case FI_WAIT_YIELD:
		break;
	case FI_WAIT_SET:
		if (!attr->wait_set) {
			FI_WARN(prov, FI_LOG_CQ, "invalid wait set\n");
			return -FI_EINVAL;
		}
		/* fall through */
	case FI_WAIT_UNSPEC:
	case FI_WAIT_FD:
	case FI_WAIT_POLLFD:
		switch (attr->wait_cond) {
		case FI_CQ_COND_NONE:
		case FI_CQ_COND_THRESHOLD:
			break;
		default:
			FI_WARN(prov, FI_LOG_CQ, "unsupported wait cond\n");
			return -FI_EINVAL;
		}
		break;
	default:
		FI_WARN(prov, FI_LOG_CQ, "unsupported wait object\n");
		return -FI_EINVAL;
	}

	if (attr->flags & ~(FI_AFFINITY)) {
		FI_WARN(prov, FI_LOG_CQ, "invalid flags\n");
		return -FI_EINVAL;
	}

	if (attr->flags & FI_AFFINITY) {
		FI_WARN(prov, FI_LOG_CQ, "signaling vector ignored\n");
	}

	return 0;
}

static void util_cq_read_ctx(void **dst, void *src)
{
	*(struct fi_cq_entry *) *dst = *(struct fi_cq_entry *) src;
	*(char**)dst += sizeof(struct fi_cq_entry);
}

static void util_cq_read_msg(void **dst, void *src)
{
	*(struct fi_cq_msg_entry *) *dst = *(struct fi_cq_msg_entry *) src;
	*(char**)dst += sizeof(struct fi_cq_msg_entry);
}

static void util_cq_read_data(void **dst, void *src)
{
	*(struct fi_cq_data_entry *) *dst = *(struct fi_cq_data_entry *) src;
	*(char**)dst += sizeof(struct fi_cq_data_entry);
}

static void util_cq_read_tagged(void **dst, void *src)
{
	*(struct fi_cq_tagged_entry *) *dst = *(struct fi_cq_tagged_entry *) src;
	*(char **)dst += sizeof(struct fi_cq_tagged_entry);
}

ssize_t ofi_cq_readfrom(struct fid_cq *cq_fid, void *buf, size_t count,
			fi_addr_t *src_addr)
{
	struct fi_cq_tagged_entry *entry;
	struct util_cq_aux_entry *aux_entry;
	struct util_cq *cq;
	ssize_t i;

	cq = container_of(cq_fid, struct util_cq, cq_fid);

	cq->cq_mutex_lock(&cq->cq_lock);
	if (ofi_cirque_isempty(cq->cirq) || !count) {
		cq->cq_mutex_unlock(&cq->cq_lock);
		cq->progress(cq);
		cq->cq_mutex_lock(&cq->cq_lock);
		if (ofi_cirque_isempty(cq->cirq)) {
			i = -FI_EAGAIN;
			goto out;
		}
	}

	if (count > ofi_cirque_usedcnt(cq->cirq))
		count = ofi_cirque_usedcnt(cq->cirq);

	for (i = 0; i < (ssize_t) count; i++) {
		entry = ofi_cirque_head(cq->cirq);
		if (!(entry->flags & UTIL_FLAG_AUX)) {
			if (src_addr && cq->src)
				src_addr[i] = cq->src[ofi_cirque_rindex(cq->cirq)];
			cq->read_entry(&buf, entry);
			ofi_cirque_discard(cq->cirq);
		} else {
			assert(!slist_empty(&cq->aux_queue));
			aux_entry = container_of(cq->aux_queue.head,
						 struct util_cq_aux_entry,
						 list_entry);
			assert(aux_entry->cq_slot == entry);
			if (aux_entry->comp.err) {
				if (!i)
					i = -FI_EAVAIL;
				break;
			}

			if (src_addr && cq->src)
				src_addr[i] = aux_entry->src;
			cq->read_entry(&buf, &aux_entry->comp);
			slist_remove_head(&cq->aux_queue);

			if (slist_empty(&cq->aux_queue)) {
				ofi_cirque_discard(cq->cirq);
			} else {
				aux_entry = container_of(cq->aux_queue.head,
							struct util_cq_aux_entry,
							list_entry);
				if (aux_entry->cq_slot != ofi_cirque_head(cq->cirq))
					ofi_cirque_discard(cq->cirq);
			}
		}
	}
out:
	cq->cq_mutex_unlock(&cq->cq_lock);
	return i;
}

ssize_t ofi_cq_read(struct fid_cq *cq_fid, void *buf, size_t count)
{
	return ofi_cq_readfrom(cq_fid, buf, count, NULL);
}

ssize_t ofi_cq_readerr(struct fid_cq *cq_fid, struct fi_cq_err_entry *buf,
		       uint64_t flags)
{
	struct util_cq_aux_entry *aux_entry;
	struct util_cq *cq;
	char *err_buf_save;
	size_t err_data_size;
	uint32_t api_version;
	ssize_t ret;

	cq = container_of(cq_fid, struct util_cq, cq_fid);
	api_version = cq->domain->fabric->fabric_fid.api_version;

	cq->cq_mutex_lock(&cq->cq_lock);
	if (ofi_cirque_isempty(cq->cirq) ||
	    !(ofi_cirque_head(cq->cirq)->flags & UTIL_FLAG_AUX)) {
		ret = -FI_EAGAIN;
		goto unlock;
	}

	assert(!slist_empty(&cq->aux_queue));
	aux_entry = container_of(cq->aux_queue.head,
				 struct util_cq_aux_entry, list_entry);
	assert(aux_entry->cq_slot == ofi_cirque_head(cq->cirq));

	if (!aux_entry->comp.err) {
		ret = -FI_EAGAIN;
		goto unlock;
	}

	if ((FI_VERSION_GE(api_version, FI_VERSION(1, 5))) &&
	    buf->err_data_size) {
		err_buf_save = buf->err_data;
		err_data_size = MIN(buf->err_data_size,
				    aux_entry->comp.err_data_size);

		*buf = aux_entry->comp;
		memcpy(err_buf_save, aux_entry->comp.err_data, err_data_size);
		buf->err_data = err_buf_save;
		buf->err_data_size = err_data_size;
	} else {
		memcpy(buf, &aux_entry->comp,
		       sizeof(struct fi_cq_err_entry_1_0));
	}

	slist_remove_head(&cq->aux_queue);
	free(aux_entry);
	if (slist_empty(&cq->aux_queue)) {
		ofi_cirque_discard(cq->cirq);
	} else {
		aux_entry = container_of(cq->aux_queue.head,
					 struct util_cq_aux_entry, list_entry);
		if (aux_entry->cq_slot != ofi_cirque_head(cq->cirq))
			ofi_cirque_discard(cq->cirq);
	}

	ret = 1;
unlock:
	cq->cq_mutex_unlock(&cq->cq_lock);
	return ret;
}

ssize_t ofi_cq_sreadfrom(struct fid_cq *cq_fid, void *buf, size_t count,
			 fi_addr_t *src_addr, const void *cond, int timeout)
{
	struct util_cq *cq;
	uint64_t endtime;
	int ret;

	cq = container_of(cq_fid, struct util_cq, cq_fid);
	assert(cq->wait && cq->internal_wait);
	endtime = ofi_timeout_time(timeout);

	do {
		ret = ofi_cq_readfrom(cq_fid, buf, count, src_addr);
		if (ret != -FI_EAGAIN)
			break;

		if (ofi_adjust_timeout(endtime, &timeout))
			return -FI_EAGAIN;

		if (ofi_atomic_get32(&cq->signaled)) {
			ofi_atomic_set32(&cq->signaled, 0);
			return -FI_EAGAIN;
		}

		ret = fi_wait(&cq->wait->wait_fid, timeout);
	} while (!ret);

	return ret == -FI_ETIMEDOUT ? -FI_EAGAIN : ret;
}

ssize_t ofi_cq_sread(struct fid_cq *cq_fid, void *buf, size_t count,
		const void *cond, int timeout)
{
	return ofi_cq_sreadfrom(cq_fid, buf, count, NULL, cond, timeout);
}

int ofi_cq_signal(struct fid_cq *cq_fid)
{
	struct util_cq *cq = container_of(cq_fid, struct util_cq, cq_fid);
	ofi_atomic_set32(&cq->signaled, 1);
	util_cq_signal(cq);
	return 0;
}

static const char *util_cq_strerror(struct fid_cq *cq, int prov_errno,
				    const void *err_data, char *buf, size_t len)
{
	return fi_strerror(prov_errno);
}

static struct fi_ops_cq util_cq_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = ofi_cq_read,
	.readfrom = ofi_cq_readfrom,
	.readerr = ofi_cq_readerr,
	.sread = ofi_cq_sread,
	.sreadfrom = ofi_cq_sreadfrom,
	.signal = ofi_cq_signal,
	.strerror = util_cq_strerror,
};

int ofi_cq_cleanup(struct util_cq *cq)
{
	struct util_cq_aux_entry *err;
	struct slist_entry *entry;

	if (ofi_atomic_get32(&cq->ref))
		return -FI_EBUSY;

	while (!slist_empty(&cq->aux_queue)) {
		entry = slist_remove_head(&cq->aux_queue);
		err = container_of(entry, struct util_cq_aux_entry, list_entry);
		free(err);
	}

	if (cq->wait) {
		fi_poll_del(&cq->wait->pollset->poll_fid,
			    &cq->cq_fid.fid, 0);
		if (cq->internal_wait)
			fi_close(&cq->wait->wait_fid.fid);
	}

	ofi_atomic_dec32(&cq->domain->ref);
	util_comp_cirq_free(cq->cirq);
	ofi_mutex_destroy(&cq->cq_lock);
	ofi_mutex_destroy(&cq->ep_list_lock);
	free(cq->src);
	return 0;
}

int ofi_cq_control(struct fid *fid, int command, void *arg)
{
	struct util_cq *cq = container_of(fid, struct util_cq, cq_fid.fid);

	switch (command) {
	case FI_GETWAIT:
	case FI_GETWAITOBJ:
		if (!cq->wait)
			return -FI_ENODATA;
		return fi_control(&cq->wait->wait_fid.fid, command, arg);
	default:
		FI_INFO(cq->wait->prov, FI_LOG_CQ, "Unsupported command\n");
		return -FI_ENOSYS;
	}
}

static int util_cq_close(struct fid *fid)
{
	struct util_cq *cq;
	int ret;

	cq = container_of(fid, struct util_cq, cq_fid.fid);
	ret = ofi_cq_cleanup(cq);
	if (ret)
		return ret;

	free(cq);
	return 0;
}

static struct fi_ops util_cq_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = util_cq_close,
	.bind = fi_no_bind,
	.control = ofi_cq_control,
	.ops_open = fi_no_ops_open,
};

static int fi_cq_init(struct fid_domain *domain, struct fi_cq_attr *attr,
		      fi_cq_read_func read_entry, struct util_cq *cq,
		      void *context)
{
	struct fi_wait_attr wait_attr;
	struct fid_wait *wait;
	int ret;

	cq->domain = container_of(domain, struct util_domain, domain_fid);
	ofi_atomic_initialize32(&cq->ref, 0);
	ofi_atomic_initialize32(&cq->signaled, 0);
	dlist_init(&cq->ep_list);
	ofi_mutex_init(&cq->ep_list_lock);
	ofi_mutex_init(&cq->cq_lock);
	if (cq->domain->threading == FI_THREAD_COMPLETION ||
	    (cq->domain->threading == FI_THREAD_DOMAIN)) {
		cq->cq_mutex_lock = ofi_mutex_lock_noop;
		cq->cq_mutex_unlock = ofi_mutex_unlock_noop;
	} else {
		cq->cq_mutex_lock = ofi_mutex_lock_op;
		cq->cq_mutex_unlock = ofi_mutex_unlock_op;
	}
	slist_init(&cq->aux_queue);
	cq->read_entry = read_entry;

	cq->cq_fid.fid.fclass = FI_CLASS_CQ;
	cq->cq_fid.fid.context = context;

	switch (attr->wait_obj) {
	case FI_WAIT_NONE:
		wait = NULL;
		break;
	case FI_WAIT_UNSPEC:
	case FI_WAIT_FD:
	case FI_WAIT_POLLFD:
	case FI_WAIT_MUTEX_COND:
	case FI_WAIT_YIELD:
		memset(&wait_attr, 0, sizeof wait_attr);
		wait_attr.wait_obj = attr->wait_obj;
		cq->internal_wait = 1;
		ret = fi_wait_open(&cq->domain->fabric->fabric_fid,
				   &wait_attr, &wait);
		if (ret)
			return ret;
		break;
	case FI_WAIT_SET:
		wait = attr->wait_set;
		break;
	default:
		assert(0);
		return -FI_EINVAL;
	}

	if (wait)
		cq->wait = container_of(wait, struct util_wait, wait_fid);

	ofi_atomic_inc32(&cq->domain->ref);
	return 0;
}

int ofi_check_bind_cq_flags(struct util_ep *ep, struct util_cq *cq,
			    uint64_t flags)
{
	const struct fi_provider *prov = ep->domain->fabric->prov;

	if (flags & ~(FI_TRANSMIT | FI_RECV | FI_SELECTIVE_COMPLETION)) {
		FI_WARN(prov, FI_LOG_EP_CTRL,
			"Unsupported flags\n");
		return -FI_EBADFLAGS;
	}

	if (((flags & FI_TRANSMIT) && ep->tx_cq) ||
	    ((flags & FI_RECV) && ep->rx_cq)) {
		FI_WARN(prov, FI_LOG_EP_CTRL,
			"Duplicate CQ binding\n");
		return -FI_EINVAL;
	}

	return FI_SUCCESS;
}

void ofi_cq_progress(struct util_cq *cq)
{
	struct util_ep *ep;
	struct fid_list_entry *fid_entry;
	struct dlist_entry *item;

	cq->cq_mutex_lock(&cq->ep_list_lock);
	dlist_foreach(&cq->ep_list, item) {
		fid_entry = container_of(item, struct fid_list_entry, entry);
		ep = container_of(fid_entry->fid, struct util_ep, ep_fid.fid);
		ep->progress(ep);

	}
	cq->cq_mutex_unlock(&cq->ep_list_lock);
}

int ofi_cq_init(const struct fi_provider *prov, struct fid_domain *domain,
		 struct fi_cq_attr *attr, struct util_cq *cq,
		 ofi_cq_progress_func progress, void *context)
{
	fi_cq_read_func read_func;
	int ret;

	assert(progress);
	ret = ofi_check_cq_attr(prov, attr);
	if (ret)
		return ret;

	cq->cq_fid.fid.ops = &util_cq_fi_ops;
	cq->cq_fid.ops = &util_cq_ops;
	cq->progress = progress;

	switch (attr->format) {
	case FI_CQ_FORMAT_UNSPEC:
	case FI_CQ_FORMAT_CONTEXT:
		read_func = util_cq_read_ctx;
		break;
	case FI_CQ_FORMAT_MSG:
		read_func = util_cq_read_msg;
		break;
	case FI_CQ_FORMAT_DATA:
		read_func = util_cq_read_data;
		break;
	case FI_CQ_FORMAT_TAGGED:
		read_func = util_cq_read_tagged;
		break;
	default:
		assert(0);
		return -FI_EINVAL;
	}

	ret = fi_cq_init(domain, attr, read_func, cq, context);
	if (ret)
		return ret;

	/* CQ must be fully operational before adding to wait set */
	if (cq->wait) {
		ret = fi_poll_add(&cq->wait->pollset->poll_fid,
				  &cq->cq_fid.fid, 0);
		if (ret)
			goto cleanup;
	}

	cq->cirq = util_comp_cirq_create(attr->size == 0 ? UTIL_DEF_CQ_SIZE : attr->size);
	if (!cq->cirq) {
		ret = -FI_ENOMEM;
		goto cleanup;
	}

	if (cq->domain->info_domain_caps & FI_SOURCE) {
		cq->src = calloc(cq->cirq->size, sizeof *cq->src);
		if (!cq->src) {
			ret = -FI_ENOMEM;
			goto cleanup;
		}
	}
	return 0;

cleanup:
	(void) ofi_cq_cleanup(cq);
	return ret;
}

uint64_t ofi_rx_flags[] = {
	[ofi_op_msg] = FI_MSG | FI_RECV,
	[ofi_op_tagged] = FI_RECV | FI_TAGGED,
	[ofi_op_read_req] = FI_RMA | FI_REMOTE_READ,
	[ofi_op_read_rsp] = FI_RMA | FI_REMOTE_READ,
	[ofi_op_write] = FI_RMA | FI_REMOTE_WRITE,
	[ofi_op_write_async] = FI_RMA | FI_REMOTE_WRITE,
	[ofi_op_atomic] = FI_ATOMIC | FI_REMOTE_WRITE,
	[ofi_op_atomic_fetch] = FI_ATOMIC | FI_REMOTE_READ,
	[ofi_op_atomic_compare] = FI_ATOMIC | FI_REMOTE_READ,
	[ofi_op_read_async] = FI_RMA | FI_REMOTE_READ,
};

uint64_t ofi_tx_flags[] = {
	[ofi_op_msg] = FI_SEND | FI_MSG,
	[ofi_op_tagged] = FI_SEND | FI_TAGGED,
	[ofi_op_read_req] = FI_RMA | FI_READ,
	[ofi_op_read_rsp] = FI_RMA | FI_READ,
	[ofi_op_write] = FI_RMA | FI_WRITE,
	[ofi_op_write_async] = FI_RMA | FI_WRITE,
	[ofi_op_atomic] = FI_ATOMIC | FI_WRITE,
	[ofi_op_atomic_fetch] = FI_ATOMIC | FI_READ,
	[ofi_op_atomic_compare] = FI_ATOMIC | FI_READ,
	[ofi_op_read_async] = FI_RMA | FI_READ,
};

