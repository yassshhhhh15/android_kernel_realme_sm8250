/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Oplus. All rights reserved.
 */
#ifndef _OPLUS_QOS_SCHED_LUT_H
#define _OPLUS_QOS_SCHED_LUT_H

#include <linux/sched.h>
#include <linux/types.h>
#include <linux/refcount.h>
#include <linux/mutex.h>

#define MAX_QOS_LEVEL       (256)
#define QOS_LATENCY_SHIFT   (32)
#define QOS_LUT_TYPE_MASK   (7)
#define LUT_CTRL_MAGIC 'p'

struct qos_lut_ioctl_request {
	s32 lut_size;
	s32 type;
	s64 version;
};

struct qos_lut_item {
	s32 qos_level;
	s64 latency;
	union {
		s32 share;
		s32 prio;
	} share_or_prio;
	s32 uclamp_min;
	s32 uclamp_max;
	s32 stune;
};

enum QOS_LUT_TYPE {
	QOS_LUT_TASK = 1,
	QOS_LUT_PROCESS,
	QOS_LUT_GROUP = 4,
};

struct qos_lut {
	struct qos_lut_item *items;
};

struct qos_lut_info {
	u16 lut_num;
	s32 lut_size;
	struct qos_lut_item *mem;
	refcount_t refs;
};

struct qos_lut_ctl {
	s64 version;
	struct qos_lut_info __rcu *info;
};

/* Protected by qs_lut_mutex, including the file's table reference. */
struct qos_lut_file {
	s32 type;
	struct qos_lut_info *info;
};

enum {
	UPDATE_LUT_REQUEST = 1,
	GET_LUT_VERSION,
	CTRL_LUT_MAX,
};


#define IOCTL_UPDATE_LUT_REQUEST \
	_IOW(LUT_CTRL_MAGIC, UPDATE_LUT_REQUEST, struct qos_lut_ioctl_request)
#define IOCTL_GET_LUT_VERSION \
	_IOR(LUT_CTRL_MAGIC, GET_LUT_VERSION, struct qos_lut_ioctl_request)

extern struct mutex qs_lut_mutex;
s32 qs_update_lut_request(struct qos_lut_ioctl_request *request,
			  struct qos_lut_file *state);
s64 qs_get_lut_version(s32 type);
struct qos_lut_ctl *qs_get_lut_ctl(s32 type);
void qs_put_lut(struct qos_lut_info *info);
bool qs_get_lut_item(s32 level, s32 type, struct qos_lut_item *item);
#endif  /* _OPLUS_QOS_SCHED_LUT_H */
