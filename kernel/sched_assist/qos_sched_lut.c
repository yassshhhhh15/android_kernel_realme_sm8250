// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2025 Oplus. All rights reserved. */
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include "qos_sched.h"
#include "qos_sched_lut.h"

#define QOS_LUT_MAX (4)

static struct qos_lut_ctl lut_ctl[QOS_LUT_MAX];
DEFINE_MUTEX(qs_lut_mutex);

struct qos_lut_ctl *qs_get_lut_ctl(s32 type)
{
	if (type != QOS_LUT_TASK && type != QOS_LUT_PROCESS &&
	    type != QOS_LUT_GROUP)
		return NULL;
	return &lut_ctl[type - 1];
}

/* Caller holds qs_lut_mutex. */
s64 qs_get_lut_version(s32 type)
{
	struct qos_lut_ctl *ctl = qs_get_lut_ctl(type);

	return ctl ? ctl->version : 0;
}

void qs_put_lut(struct qos_lut_info *info)
{
	if (info && refcount_dec_and_test(&info->refs)) {
		free_pages((unsigned long)info->mem, get_order(info->lut_size));
		kfree(info);
	}
}

bool qs_get_lut_item(s32 level, s32 type, struct qos_lut_item *item)
{
	struct qos_lut_ctl *ctl = qs_get_lut_ctl(type);
	struct qos_lut_info *info;
	bool found = false;

	if (!ctl)
		return false;
	rcu_read_lock();
	info = rcu_dereference(ctl->info);
	if (info && level >= 0 && level < info->lut_num) {
		/* The ABI allows userspace to write this mapping at any time. */
		memcpy(item, &info->mem[level], sizeof(*item));
		found = true;
	}
	rcu_read_unlock();
	return found;
}

/* Caller serializes replacement and per-file selection with qs_lut_mutex. */
s32 qs_update_lut_request(struct qos_lut_ioctl_request *request,
			  struct qos_lut_file *state)
{
	struct qos_lut_ctl *ctl = qs_get_lut_ctl(request->type);
	struct qos_lut_info *info, *old;

	if (!ctl || request->lut_size <= 0 ||
	    request->lut_size > MAX_QOS_LEVEL * sizeof(struct qos_lut_item) ||
	    request->lut_size % sizeof(struct qos_lut_item))
		return -EINVAL;
	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->mem = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
					   get_order(request->lut_size));
	if (!info->mem) {
		kfree(info);
		return -ENOMEM;
	}
	info->lut_size = request->lut_size;
	info->lut_num = request->lut_size / sizeof(struct qos_lut_item);
	/* One publication reference and one reference for the selecting fd. */
	refcount_set(&info->refs, 2);
	old = rcu_dereference_protected(ctl->info,
				      lockdep_is_held(&qs_lut_mutex));
	rcu_assign_pointer(ctl->info, info);
	ctl->version = request->version;
	qs_put_lut(state->info);
	state->info = info;
	state->type = request->type;
	/* RCU readers finish copying before the publication reference is put. */
	synchronize_rcu();
	qs_put_lut(old);
	return 0;
}
