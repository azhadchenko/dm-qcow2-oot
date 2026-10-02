// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2021 Virtuozzo International GmbH. All rights reserved.
 */
#include <linux/device-mapper.h>
#include <linux/sched/signal.h>
#include <linux/file.h>
#include <linux/eventfd.h>
#include <linux/minmax.h>
#include <linux/error-injection.h>
#include "dm-qcow2.h"

#define SERVICE_QIOS_MAX 64

static int qcow2_get_errors(struct qcow2_target *tgt, char *result,
			    unsigned int maxlen)
{
	bool wants_check = qcow2_wants_check(tgt);
	unsigned int sz = 0;
	int ret;

	ret = DMEMIT("wants_check=%d\nmd_writeback_error=%d\ntruncate_error=%d\n"
		     "merge_backward_error=%d\n",
		      wants_check, tgt->md_writeback_error, tgt->truncate_error,
		      tgt->backward_merge.error);

	return ret ? 1 : 0;
}

int qcow2_inflight_ref_switch(struct qcow2_target *tgt)
{
	struct completion *comp = &tgt->inflight_ref_comp;
	u8 ref_index = tgt->inflight_ref_index;

	tgt->inflight_ref_index = !ref_index;

	percpu_ref_kill(&tgt->inflight_ref[ref_index]);
	wait_for_completion(comp);

	percpu_ref_reinit(&tgt->inflight_ref[ref_index]);
	reinit_completion(comp);
	return 0;
}

static void service_qio_endio(struct qcow2_target *tgt, struct qio *qio,
			      void *data, blk_status_t status,
			      u8 ref_index, bool *keep_ref)
{
	blk_status_t *status_ptr = data;

	if (unlikely(status)) {
		WRITE_ONCE(*status_ptr, status);
		smp_wmb(); /* Pairs with smp_rmb() in qcow2_service_iter() */
	}

	atomic_dec(&tgt->service_qios);
	wake_up(&tgt->service_wq);
}

static void backward_merge_update_progress(struct qcow2_target *tgt,
					   long long progress);
static void backward_merge_update_max_progress(struct qcow2_target *tgt,
					       long long max_progress);
static bool qcow2_backward_merge_should_stop(struct qcow2_target *tgt);

static int qcow2_service_iter(struct qcow2_target *tgt, struct qcow2 *qcow2,
		  loff_t end, loff_t step, unsigned int bi_op, u8 qio_flags)
{
	static blk_status_t service_status;
	struct qio *qio;
	int ret = 0;
	loff_t pos;

	WRITE_ONCE(service_status, BLK_STS_OK);

	backward_merge_update_max_progress(tgt, end);
	for (pos = 0; pos < end; pos += step) {
		backward_merge_update_progress(tgt, pos);

		if (qcow2_backward_merge_should_stop(tgt)) {
			ret = -EINTR;
			break;
		}

		qio = qcow2_alloc_qio(tgt->qio_pool, true);
		if (!qio) {
			ret = -ENOMEM;
			break;
		}

		/* See fake_service_qio() */
		qcow2_init_qio(qio, bi_op, qcow2);
		qio->flags |= qio_flags|QIO_FREE_ON_ENDIO_FL;
		qio->bi_iter.bi_sector = to_sector(pos);
		/* The rest is zeroed in alloc_qio() */
		qio->endio_cb = service_qio_endio;
		qio->endio_cb_data = &service_status;

		qcow2_dispatch_qios(qcow2, qio, NULL);
		if (atomic_inc_return(&tgt->service_qios) == SERVICE_QIOS_MAX) {
			wait_event(tgt->service_wq,
				   atomic_read(&tgt->service_qios) < SERVICE_QIOS_MAX);
		}

		if (unlikely(READ_ONCE(service_status)))
			break;
	}

	wait_event(tgt->service_wq, !atomic_read(&tgt->service_qios));
	if (!ret) {
		smp_rmb(); /* Pairs with smp_wmb() in service_qio_endio() */
		ret = blk_status_to_errno(READ_ONCE(service_status));
	}

	backward_merge_update_progress(tgt, (pos > end) ? end : pos);
	return ret;
}
ALLOW_ERROR_INJECTION(qcow2_service_iter, ERRNO);

static int qcow2_merge_common(struct qcow2_target *tgt, struct qcow2 *qcow2)
{
	struct qcow2 *lower = qcow2->lower;
	u32 clu_size = qcow2->clu_size;
	loff_t end = lower->hdr.size;

	return qcow2_service_iter(tgt, qcow2, end, clu_size,
				  REQ_OP_WRITE, QIO_IS_MERGE_FL);
}

/*
 * Forward merge is a simple COW simulation in every clu.
 * After that, all mapped clus from lower delta become
 * mapped in top delta. Then, userspace may remove lower
 * delta from the deltas stack (and it also has to update
 * backing file name in top delta's metadata).
 */
static int qcow2_merge_forward(struct qcow2_target *tgt)
{
	return -ENOTTY; /* TODO */
}
ALLOW_ERROR_INJECTION(qcow2_merge_forward, ERRNO);

static int qcow2_break_l1cow(struct qcow2_target *tgt, struct qcow2 *qcow2)
{
	loff_t end = qcow2->hdr.size;
	loff_t step = (u64)qcow2->l2_entries * qcow2->clu_size;

	return qcow2_service_iter(tgt, qcow2, end, step,
				  REQ_OP_WRITE, QIO_IS_L1COW_FL);
}

static void set_backward_merge_in_process(struct qcow2_target *tgt,
				     struct qcow2 *qcow2, bool set)
{
	LIST_HEAD(list);

	/*
	 * There are no writes if it is not the top qcow2 image.
	 * so we do not need to stop and flush requests when setting
	 */
	if (qcow2 != tgt->top && set) {
		qcow2->backward_merge_in_process = set;
		return;
	}

	/*
	 * To avoid race between allocations and COWS
	 * we completely stop queueing qios and wait
	 * for pending qios. Lock is for visability.
	 */
	spin_lock_irq(&qcow2->deferred_lock);
	qcow2->pause_submitting_qios = true;
	spin_unlock_irq(&qcow2->deferred_lock);
	qcow2_inflight_ref_switch(tgt);

	/* queue is stopped */
	spin_lock_irq(&qcow2->deferred_lock);
	WARN_ON_ONCE(qcow2->backward_merge_in_process == set);
	qcow2->backward_merge_in_process = set;
	qcow2->pause_submitting_qios = false;
	list_splice_init(&qcow2->paused_qios, &list);
	spin_unlock_irq(&qcow2->deferred_lock);

	qcow2_submit_toplevel_qios(tgt, &list);
}

static void __backward_merge_update_stage(struct qcow2_target *tgt,
					   enum qcow2_backward_merge_stage stage)
{
	tgt->backward_merge.stage = stage;
	if (stage == BACKWARD_MERGE_STAGE_START) {
		tgt->backward_merge.progress = 0;
		tgt->backward_merge.max_progress = 0;
	}
}

static void backward_merge_update_stage(struct qcow2_target *tgt,
					 enum qcow2_backward_merge_stage stage)
{
	mutex_lock(&tgt->ctl_mutex);
	__backward_merge_update_stage(tgt, stage);
	mutex_unlock(&tgt->ctl_mutex);
}

static void backward_merge_update_max_progress(struct qcow2_target *tgt,
					       long long max_progress)
{
	xchg(&tgt->backward_merge.max_progress, max_progress);
}

static void backward_merge_update_progress(struct qcow2_target *tgt,
					   long long progress)
{
	xchg(&tgt->backward_merge.progress, progress);
}

char *backward_merge_stage_names[] = {
	"none",
	"start",
	"break_l1cow",
	"set_dirty",
	"running",
	"waiting_completion",
	"completing",
	"fail",
};

static int qcow2_merge_backward_progress(struct qcow2_target *tgt,
					 char *result, unsigned int maxlen)
{
	struct qcow2_backward_merge backward_merge;
	unsigned int sz = 0;
	int ret;

	BUILD_BUG_ON(ARRAY_SIZE(backward_merge_stage_names) != BACKWARD_MERGE_STAGE_MAX);

	mutex_lock(&tgt->ctl_mutex);
	backward_merge = tgt->backward_merge;
	mutex_unlock(&tgt->ctl_mutex);

	ret = DMEMIT("stage=%s\nprogress=%lld\nmax_progress=%lld\nerror=%d\n",
		     backward_merge_stage_names[backward_merge.stage],
		     backward_merge.progress,
		     backward_merge.max_progress,
		     backward_merge.error);

	return ret ? 1 : 0;
}

static int qcow2_merge_backward_set_eventfd(struct qcow2_target *tgt, int efd);

static int qcow2_merge_backward_start(struct qcow2_target *tgt, int efd, u32 depth)
{
	struct qcow2 *qcow2, *lower, **pqcow2 = &tgt->top;
	int ret;

	lockdep_assert_held(&tgt->ctl_mutex);

	while (depth-- && *pqcow2)
		pqcow2 = &(*pqcow2)->lower;

	qcow2 = *pqcow2;
	if (!qcow2)
		return -ENOENT;
	lower = qcow2->lower;
	if (!lower)
		return -ENOENT;
	if (!qcow2_file_is_writable(lower))
		return -EACCES;
	if (qcow2->clu_size != lower->clu_size)
		return -EOPNOTSUPP;
	if (lower->hdr.size < qcow2->hdr.size)
		return -EBADSLT;

	if (tgt->backward_merge.state != BACKWARD_MERGE_STOPPED)
		return -EBUSY;

	ret = qcow2_merge_backward_set_eventfd(tgt, efd);
	if (ret)
		return ret;

	tgt->backward_merge.pqcow2 = pqcow2;
	tgt->backward_merge.state = BACKWARD_MERGE_START;
	__backward_merge_update_stage(tgt, BACKWARD_MERGE_STAGE_START);
	tgt->backward_merge.error = 0;

	schedule_work(&tgt->backward_merge.work);
	return 0;
}
ALLOW_ERROR_INJECTION(qcow2_merge_backward_start, ERRNO);

void qcow2_merge_backward_work(struct work_struct *work)
{
	struct qcow2_target *tgt = container_of(work, struct qcow2_target,
						backward_merge.work);
	struct qcow2 *qcow2, *lower;
	int ret, ret2;

	mutex_lock(&tgt->ctl_mutex);
	if (tgt->backward_merge.state != BACKWARD_MERGE_START) {
		if (tgt->backward_merge.eventfd_ctx)
			eventfd_signal(tgt->backward_merge.eventfd_ctx);
		qcow2_merge_backward_set_eventfd(tgt, -1);
		mutex_unlock(&tgt->ctl_mutex);
		return;
	}
	tgt->backward_merge.state = BACKWARD_MERGE_RUN;
	__backward_merge_update_stage(tgt, BACKWARD_MERGE_STAGE_BREAK_L1COW);
	mutex_unlock(&tgt->ctl_mutex);

	qcow2 = *tgt->backward_merge.pqcow2;
	lower = qcow2->lower;

	/*
	 * Break all COW clus at L1 level. Otherwise, later
	 * there would be problems with unusing them:
	 * we'd have to freeze IO going to all data clusters
	 * under every L1 entry related to several snapshots.
	 * Readonly images skip this stage.
	 */
	if (qcow2_file_is_writable(qcow2)) {
		ret = qcow2_break_l1cow(tgt, qcow2);
		if (ret) {
			QC_ERR(tgt->ti, "Can't break L1 COW");
			goto out_err;
		}
	}

	backward_merge_update_stage(tgt, BACKWARD_MERGE_STAGE_SET_DIRTY);
	ret = qcow2_set_image_file_features(lower, true);
	if (ret) {
		QC_ERR(tgt->ti, "Can't set dirty bit");
		goto out_err;
	}
	set_backward_merge_in_process(tgt, qcow2, true);

	/* Start merge */
	backward_merge_update_stage(tgt, BACKWARD_MERGE_STAGE_RUNNING);
	ret = qcow2_merge_common(tgt, qcow2);
	if (ret) {
		set_backward_merge_in_process(tgt, qcow2, false);
		ret2 = qcow2_set_image_file_features(lower, false);
		if (ret2 < 0)
			QC_ERR(tgt->ti, "Can't unuse lower (%d)", ret2);
	}

out_err:
	mutex_lock(&tgt->ctl_mutex);
	if (ret) {
		/* Error */
		tgt->backward_merge.state = BACKWARD_MERGE_STOPPED;
		tgt->backward_merge.error = ret;
		__backward_merge_update_stage(tgt, BACKWARD_MERGE_STAGE_FAIL);
	} else if (tgt->backward_merge.state == BACKWARD_MERGE_STOP) {
		/* Merge is canceled */
		set_backward_merge_in_process(tgt, qcow2, false);
		tgt->backward_merge.state = BACKWARD_MERGE_STOPPED;
		tgt->backward_merge.error = -EINTR;
		__backward_merge_update_stage(tgt, BACKWARD_MERGE_STAGE_FAIL);
	} else {
		/* Finish merge */
		tgt->backward_merge.state = BACKWARD_MERGE_WAIT_COMPLETION;
		__backward_merge_update_stage(tgt, BACKWARD_MERGE_STAGE_WAITING_COMPLETION);
	}
	if (tgt->backward_merge.eventfd_ctx)
		eventfd_signal(tgt->backward_merge.eventfd_ctx);
	qcow2_merge_backward_set_eventfd(tgt, -1);
	mutex_unlock(&tgt->ctl_mutex);
}

static int qcow2_merge_backward_complete(struct qcow2_target *tgt)
{
	struct qcow2 *qcow2, *i;
	int ret;

	lockdep_assert_held(&tgt->ctl_mutex);

	/*
	 * .pqcow2 is NULL until the first "merge_backward start" and
	 * is valid while the state is BACKWARD_MERGE_WAIT_COMPLETION.
	 */
	if (tgt->backward_merge.state != BACKWARD_MERGE_WAIT_COMPLETION)
		return -EBUSY;

	qcow2 = *tgt->backward_merge.pqcow2;
	*tgt->backward_merge.pqcow2 = qcow2->lower;

	i = tgt->top;
	while (i != qcow2->lower) {
		i->img_id--;
		i = i->lower;
	}

	__backward_merge_update_stage(tgt, BACKWARD_MERGE_STAGE_COMPLETING);
	smp_wmb(); /* Pairs with qcow2_ref_inc() */
	qcow2_inflight_ref_switch(tgt); /* Pending qios */
	qcow2_flush_deferred_activity(tgt, qcow2); /* Delayed md pages */
	qcow2->lower = NULL;

	if (qcow2_file_is_writable(qcow2)) {
		ret = qcow2_set_image_file_features(qcow2, false);
		if (ret < 0)
			QC_ERR(tgt->ti, "Can't unuse merged img (%d)", ret);
	}
	qcow2_destroy(qcow2);

	tgt->backward_merge.state = BACKWARD_MERGE_STOPPED;
	__backward_merge_update_stage(tgt, BACKWARD_MERGE_STAGE_NONE);

	return 0;
}
ALLOW_ERROR_INJECTION(qcow2_merge_backward_complete, ERRNO);

void qcow2_merge_backward_cancel(struct qcow2_target *tgt)
{
	bool flush = false;

	mutex_lock(&tgt->ctl_mutex);
	if (tgt->backward_merge.state == BACKWARD_MERGE_STOPPED) {
		mutex_unlock(&tgt->ctl_mutex);
		return;
	}

	if (tgt->backward_merge.state == BACKWARD_MERGE_START) {
		tgt->backward_merge.state = BACKWARD_MERGE_STOPPED;
		flush = true;
	} else if (tgt->backward_merge.state == BACKWARD_MERGE_RUN) {
		tgt->backward_merge.state = BACKWARD_MERGE_STOP;
		flush = true;
	} else if (tgt->backward_merge.state == BACKWARD_MERGE_STOP) {
		flush = true;
	} else if (tgt->backward_merge.state == BACKWARD_MERGE_WAIT_COMPLETION) {
		set_backward_merge_in_process(tgt, *tgt->backward_merge.pqcow2, false);
		tgt->backward_merge.state = BACKWARD_MERGE_STOPPED;
		__backward_merge_update_stage(tgt, BACKWARD_MERGE_STAGE_NONE);
	}
	mutex_unlock(&tgt->ctl_mutex);

	if (flush)
		flush_work(&tgt->backward_merge.work);
}

static bool qcow2_backward_merge_should_stop(struct qcow2_target *tgt)
{
	return READ_ONCE(tgt->backward_merge.state) == BACKWARD_MERGE_STOP;
}

#define QCOW2_FILE_UNBIND -1

static int qcow2_merge_backward_set_eventfd(struct qcow2_target *tgt, int efd)
{
	struct eventfd_ctx *ctx = NULL;

	lockdep_assert_held(&tgt->ctl_mutex);

	ctx = efd == QCOW2_FILE_UNBIND ? NULL : eventfd_ctx_fdget(efd);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	swap(ctx, tgt->backward_merge.eventfd_ctx);
	if (ctx)
		eventfd_ctx_put(ctx);

	return 0;
}

static int qcow2_merge_backward_update_eventfd(struct qcow2_target *tgt, int efd)
{
	int ret;

	mutex_lock(&tgt->ctl_mutex);
	if (efd != QCOW2_FILE_UNBIND &&
	    (tgt->backward_merge.state != BACKWARD_MERGE_START &&
	     tgt->backward_merge.state != BACKWARD_MERGE_RUN)) {
		mutex_unlock(&tgt->ctl_mutex);
		return -EBUSY;
	}

	ret = qcow2_merge_backward_set_eventfd(tgt, efd);
	if (ret) {
		mutex_unlock(&tgt->ctl_mutex);
		return ret;
	}

	mutex_unlock(&tgt->ctl_mutex);
	return 0;
}

static struct qcow2 *qcow2_get_img(struct qcow2_target *tgt, u32 img_id, u8 *ref_index)
{
	struct qcow2 *qcow2;

	qcow2 = qcow2_ref_inc(tgt, ref_index);

	while (qcow2->img_id > img_id)
		qcow2 = qcow2->lower;

	if (qcow2->img_id != img_id) {
		qcow2_ref_dec(tgt, *ref_index);
		return NULL;
	}
	return qcow2;
}
ALLOW_ERROR_INJECTION(qcow2_get_img, NULL);

static int qcow2_get_img_fd(struct qcow2_target *tgt, u32 img_id,
			    char *result, unsigned int maxlen)
{
	struct qcow2 *qcow2;
	unsigned int sz = 0;
	u8 ref_index;
	int fd, ret;

	qcow2 = qcow2_get_img(tgt, img_id, &ref_index);
	if (!qcow2) {
		result[0] = 0; /* empty output */
		return 1;
	}

	ret = fd = get_unused_fd_flags(0);
	if (fd < 0)
		goto out_ref_dec;

	if (DMEMIT("%d\n", fd) == 0) {
		/* Not enough space in @result */
		ret = 0;
		put_unused_fd(fd);
		goto out_ref_dec;
	}

	ret = 1;
	fd_install(fd, get_file(qcow2->file));
out_ref_dec:
	qcow2_ref_dec(tgt, ref_index);
	return ret;
}

static int qcow2_get_img_name(struct qcow2_target *tgt, u32 img_id,
			      char *result, unsigned int maxlen)
{
	struct qcow2 *qcow2;
	u8 ref_index;
	char *p;
	int ret;

	qcow2 = qcow2_get_img(tgt, img_id, &ref_index);
	if (!qcow2) {
		result[0] = 0; /* empty output */
		return 1;
	}

	p = file_path(qcow2->file, result, maxlen - 1);
	if (IS_ERR(p)) {
		ret = PTR_ERR(p);
		if (PTR_ERR(p) == -ENAMETOOLONG)
			ret = 0; /* dm should pass bigger buffer */
		goto out_ref_dec;
	}

	ret = strlen(p);
	memmove(result, p, ret);
	result[ret] = 0;
	ret = 1;
out_ref_dec:
	qcow2_ref_dec(tgt, ref_index);
	return ret;
}

static int qcow2_set_fault_injection(struct qcow2_target *tgt,
				     u32 img_id, u32 ratio)
{
	struct qcow2 *qcow2;
	u8 ref_index;

	if (ratio > 100 * QCOW2_FAULT_RATIO)
		return -EINVAL;

	qcow2 = qcow2_get_img(tgt, img_id, &ref_index);
	if (!qcow2)
		return -ENOENT;

	qcow2->fault_injection = ratio; /* Unlocked */
	qcow2_ref_dec(tgt, ref_index);
	return 0;
}

static int qcow2_get_event(struct qcow2_target *tgt, char *result, unsigned int maxlen)
{
	unsigned int sz = 0;
	int ret = 0;

	spin_lock_irq(&tgt->event_lock);
	if (tgt->event_enospc) {
		ret = (DMEMIT("event_ENOSPC\n")) ? 1 : 0;
		if (ret)
			tgt->event_enospc = false;
	}
	spin_unlock_irq(&tgt->event_lock);

	return ret;
}

int qcow2_message(struct dm_target *ti, unsigned int argc, char **argv,
		  char *result, unsigned int maxlen)
{
	struct qcow2_target *tgt = to_qcow2_target(ti);
	int ret = -EPERM;
	u32 val, val2, depth = 0;
	int efd;

	if (!capable(CAP_SYS_ADMIN))
		goto out;

	ret = -EINVAL;
	if (argc < 1)
		goto out;

	if (!strcmp(argv[0], "get_img_fd")) {
		if (argc != 2 || kstrtou32(argv[1], 10, &val)) {
			ret = -EINVAL;
			goto out;
		}
		ret = qcow2_get_img_fd(tgt, val, result, maxlen);
		goto out;
	} else if (!strcmp(argv[0], "get_img_name")) {
		if (argc != 2 || kstrtou32(argv[1], 10, &val)) {
			ret = -EINVAL;
			goto out;
		}
		ret = qcow2_get_img_name(tgt, val, result, maxlen);
		goto out;
	} else if (!strcmp(argv[0], "set_fault_injection")) {
		if (argc != 3 || kstrtou32(argv[1], 10, &val) ||
				 kstrtou32(argv[2], 10, &val2)) {
			ret = -EINVAL;
			goto out;
		}
		ret = qcow2_set_fault_injection(tgt, val, val2);
		goto out;
	} else if (!strcmp(argv[0], "get_event")) {
		if (argc != 1) {
			ret = -EINVAL;
			goto out;
		}
		ret = qcow2_get_event(tgt, result, maxlen);
		goto out;
	} else if (!strcmp(argv[0], "merge_backward")) {
		if (argc < 2) {
			ret = -EINVAL;
			goto out;
		}
		if (!strcmp(argv[1], "cancel")) {
			if (argc != 2) {
				ret = -EINVAL;
				goto out;
			}
			qcow2_merge_backward_cancel(tgt);
			ret = 0;
			goto out;
		} else if (!strcmp(argv[1], "update_eventfd")) {
			if (argc != 3 || kstrtoint(argv[2], 10, &efd)) {
				ret = -EINVAL;
				goto out;
			}
			ret = qcow2_merge_backward_update_eventfd(tgt, efd);
			goto out;
		} else if (!strcmp(argv[1], "progress")) {
			if (argc != 2) {
				ret = -EINVAL;
				goto out;
			}
			ret = qcow2_merge_backward_progress(tgt, result, maxlen);
			goto out;
		}
	}

	mutex_lock(&tgt->ctl_mutex);

	if (!strcmp(argv[0], "get_errors")) {
		ret = qcow2_get_errors(tgt, result, maxlen);
	} else if (!tgt->service_operations_allowed) {
		ret = -EBUSY; /* Suspended */
		/* Service operations goes below: */
	} else if (!strcmp(argv[0], "merge_forward")) {
		ret = qcow2_merge_forward(tgt);
	} else if (!strcmp(argv[0], "merge_backward")) {
		/* argc >= 2 */
		if (!strcmp(argv[1], "start")) {
			if (argc < 3 || argc > 4 || kstrtoint(argv[2], 10, &efd) || efd < 0) {
				ret = -EINVAL;
				goto out_unlock;
			}
			if (argc == 4 && kstrtou32(argv[3], 10, &depth)) {
				ret = -EINVAL;
				goto out_unlock;
			}
			ret = qcow2_merge_backward_start(tgt, efd, depth);
		} else if (!strcmp(argv[1], "complete")) {
			if (argc != 2) {
				ret = -EINVAL;
				goto out_unlock;
			}
			ret = qcow2_merge_backward_complete(tgt);
		} else {
			ret = -ENOTTY;
		}
	} else {
		ret = -ENOTTY;
	}

out_unlock:
	mutex_unlock(&tgt->ctl_mutex);
out:
	return ret;
}
