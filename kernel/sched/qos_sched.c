// SPDX-License-Identifier: GPL-2.0-only
/*
 * Oplus scheduler QoS policy for the SM8250 WALT/EAS scheduler.
 *
 * This first implementation deliberately consumes existing task signals.
 * FrameBoost and SchedAssist remain authoritative; QoS only keeps an
 * abnormal CFS task off the maximum-capacity cluster when a lower cluster is
 * available.  Mode 1 is observation-only, so the feature can be enabled on a
 * production kernel before placement changes are tested.
 */
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/sched/qos_sched.h>
#include <linux/sysctl.h>

#define CREATE_TRACE_POINTS
#include <trace/events/qos_sched.h>

#include "sched.h"

#ifdef OPLUS_FEATURE_SCHED_ASSIST
#include <linux/sched_assist/sched_assist_common.h>
#endif

#ifdef CONFIG_OPLUS_FEATURE_FRAME_BOOST
#include <linux/tuning/frame_group.h>
#endif

#ifdef CONFIG_OPLUS_FEATURE_ABNORMAL_FLAG
#include "../../drivers/soc/oplus/oplus_overload/task_overload.h"
#endif

/* 0=off, 1=trace-only, 2=placement, 3=reserved for preemption. */
int sysctl_sched_qos_enable;
int sysctl_sched_qos_mode = 1;
int sysctl_sched_qos_debug;

static bool qos_sched_active(void)
{
	return READ_ONCE(sysctl_sched_qos_enable) &&
		READ_ONCE(sysctl_sched_qos_mode) > 0;
}

enum qos_sched_level qos_sched_task_level(struct task_struct *task)
{
	if (!task)
		return QOS_LEVEL_NORMAL;

#ifdef CONFIG_OPLUS_FEATURE_FRAME_BOOST
	if (frame_boost_enabled() && is_fbg_task(task))
		return QOS_LEVEL_CRITICAL;
#endif

#ifdef OPLUS_FEATURE_SCHED_ASSIST
	if (test_task_ux(task))
		return QOS_LEVEL_HIGH;
#endif

#ifdef CONFIG_OPLUS_FEATURE_ABNORMAL_FLAG
	if (task->abnormal_flag > ABNORMAL_THRESHOLD)
		return QOS_LEVEL_LOW;
#endif

	return QOS_LEVEL_NORMAL;
}

static bool qos_sched_lower_cpu_available(struct task_struct *task)
{
	int cpu;
	unsigned long task_util = 0;

#ifdef CONFIG_SCHED_WALT
	task_util = task_util_est(task);
#endif

	for_each_cpu(cpu, &task->cpus_allowed) {
		if (!cpu_active(cpu) || cpu_isolated(cpu))
			continue;
		if (!is_max_capacity_cpu(cpu) &&
			task_util <= capacity_orig_of(cpu))
			return true;
	}

	return false;
}

bool qos_sched_skip_cpu(struct task_struct *task, int cpu)
{
	if (!qos_sched_active() || READ_ONCE(sysctl_sched_qos_mode) < 2)
		return false;

	if (qos_sched_task_level(task) != QOS_LEVEL_LOW)
		return false;

	return is_max_capacity_cpu(cpu) &&
		qos_sched_lower_cpu_available(task);
}

static int qos_sched_find_lower_cpu(struct task_struct *task)
{
	int cpu;
	int best_cpu = -1;
	unsigned long task_util = 0;

#ifdef CONFIG_SCHED_WALT
	task_util = task_util_est(task);
#endif

	for_each_cpu(cpu, &task->cpus_allowed) {
		unsigned long capacity;

		if (!cpu_active(cpu) || cpu_isolated(cpu))
			continue;

		capacity = capacity_orig_of(cpu);
		if (task_util > capacity)
			continue;

		if (best_cpu < 0 || capacity < capacity_orig_of(best_cpu))
			best_cpu = cpu;
	}

	return best_cpu;
}

void qos_sched_adjust_target(struct task_struct *task, int eas_cpu,
			     int *target_cpu)
{
	enum qos_sched_level level;
	int final_cpu;
	int reason = QOS_REASON_EAS_KEEP;
	int mode;

	if (!task || !target_cpu || !qos_sched_active())
		return;

	mode = READ_ONCE(sysctl_sched_qos_mode);
	level = qos_sched_task_level(task);
	final_cpu = *target_cpu;

#ifdef CONFIG_OPLUS_FEATURE_FRAME_BOOST
	if (level == QOS_LEVEL_CRITICAL) {
		reason = QOS_REASON_FBG_BYPASS;
		goto trace;
	}
#endif

#ifdef OPLUS_FEATURE_SCHED_ASSIST
	if (level == QOS_LEVEL_HIGH) {
		reason = QOS_REASON_UX_BYPASS;
		goto trace;
	}
#endif

	if (level == QOS_LEVEL_LOW && mode >= 2 &&
		is_max_capacity_cpu(final_cpu)) {
		int lower_cpu = qos_sched_find_lower_cpu(task);

		if (lower_cpu >= 0) {
			final_cpu = lower_cpu;
			reason = QOS_REASON_LOW_AVOID_MAX;
		} else {
			reason = QOS_REASON_NO_VALID_CPU;
		}
	}

	*target_cpu = final_cpu;

trace:
	if (READ_ONCE(sysctl_sched_qos_debug) || level != QOS_LEVEL_NORMAL ||
		final_cpu != eas_cpu)
		trace_qos_sched_decision(task, level, eas_cpu, final_cpu,
					reason, mode);
}
