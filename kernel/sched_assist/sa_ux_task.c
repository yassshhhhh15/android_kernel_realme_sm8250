// SPDX-License-Identifier: GPL-2.0-only
/*
 * ColorOS 16+ "UI First" interface on the 4.19 sched_assist:
 * /proc/oplus_scheduler/sched_assist/{ux_task,ux_task_app}.
 *
 * ColorOS 17's libSchedAssistExtImpl.so sets and reads the ux state of a thread with ioctls on these
 * nodes (system_server on ux_task, apps through ux_task_app for their own threads), as OPPO's 5.10
 * sched_assist (sa_sysfs.c) provides them. The values use the 5.10 bit layout; this file converts them
 * to our 4.19 ux_state and applies the same set/reset/priority rules as write_task_ux() there.
 *
 * 5.10 value: LIGHT/HEAVY/ANIMATOR/LISTPICK = bits 0-3 (same as ours), SET = bit 7 (same),
 * RESET = bit 8, SET_PRIORITY = bit 9 (same), SWIFT = bit 14, ONCE = bit 15, INHERIT = bit 16,
 * priority = bits 24-31 (same). Ours: INHERIT = bit 8, ONCE_UX = bit 9, camera/allocator ids = 10/11.
 */

#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/cred.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/compat.h>
#include <linux/mutex.h>

#include "sched_assist_common.h"

#define UX_MAGIC 0x27
#define CMD_UX_READ  _IOR(UX_MAGIC, 0, int[2])
#define CMD_UX_WRITE _IOW(UX_MAGIC, 1, int[3])

/* 5.10 bits that differ from ours */
#define UXT_OPT_RESET		(1 << 8)
#define UXT_TYPE_SWIFT		(1 << 14)
#define UXT_TYPE_ONCE		(1 << 15)
#define UXT_TYPE_INHERIT	(1 << 16)

/* our ux type bits (what makes a task UX) */
#define UXT_TYPES	(SA_TYPE_LIGHT | SA_TYPE_HEAVY | SA_TYPE_ANIMATOR | SA_TYPE_LISTPICK | SA_TYPE_ONCE_UX)
#define UXT_PRIO	SCHED_ASSIST_UX_PRIORITY_MASK

#define UXT_ROOT_UID			0
#define UXT_SYSTEM_UID			1000
#define UXT_FIRST_APPLICATION_UID	10000
#define UXT_LAST_APPLICATION_UID	19999
#define UXT_PER_USER_RANGE		100000

extern struct proc_dir_entry *d_sched_assist;

static DEFINE_MUTEX(uxt_mutex);
static pid_t uxt_read_pid = -1;

/* ux types of a 5.10 value in our bits; SWIFT (5.10 audio/urgent) runs as ANIMATOR here */
static int uxt_types_from_510(int v)
{
	int t = v & (SA_TYPE_LIGHT | SA_TYPE_HEAVY | SA_TYPE_ANIMATOR | SA_TYPE_LISTPICK);

	if (v & UXT_TYPE_SWIFT)
		t |= SA_TYPE_ANIMATOR;
	if (v & UXT_TYPE_ONCE)
		t |= SA_TYPE_ONCE_UX;
	return t;
}

static int uxt_state_to_510(int s)
{
	int v = s & (SA_TYPE_LIGHT | SA_TYPE_HEAVY | SA_TYPE_ANIMATOR | SA_TYPE_LISTPICK | UXT_PRIO);

	if (s & SA_TYPE_INHERIT)
		v |= UXT_TYPE_INHERIT;
	if (s & SA_TYPE_ONCE_UX)
		v |= UXT_TYPE_ONCE;
	return v;
}

/*
 * New 4.19 ux_state for a 5.10 request (rules of write_task_ux() in OPPO's 5.10 sa_sysfs.c).
 * Returns false when nothing is to be changed.
 */
static bool uxt_new_state(int orig, int v, int *state)
{
	int in = uxt_types_from_510(v);
	int prio = v & UXT_PRIO;
	/* bits that are not types or priority (inherit, camera/allocator ids) stay */
	int keep = orig & ~(UXT_TYPES | UXT_PRIO);

	if ((v & (UXT_OPT_RESET | SA_OPT_SET_PRIORITY)) == (UXT_OPT_RESET | SA_OPT_SET_PRIORITY)) {
		/* reset ux and priority: overwrite */
		*state = keep | in | prio;
	} else if (v & UXT_OPT_RESET) {
		/* reset ux, keep the priority */
		*state = keep | in | (orig & UXT_PRIO);
	} else if ((v & (SA_OPT_SET | SA_OPT_SET_PRIORITY)) == (SA_OPT_SET | SA_OPT_SET_PRIORITY)) {
		if (!in)	/* clear all ux types but animator, set priority */
			*state = keep | prio | (orig & SA_TYPE_ANIMATOR);
		else		/* add the types, set priority */
			*state = keep | prio | (orig & UXT_TYPES) | in;
	} else if (v & SA_OPT_SET) {
		if (!in)	/* clear all ux types but animator */
			*state = orig & ~(UXT_TYPES & ~SA_TYPE_ANIMATOR);
		else
			*state = orig | in;
	} else if (v & SA_OPT_SET_PRIORITY) {
		/* only change the priority of a ux task */
		if (!(orig & UXT_TYPES))
			return false;
		*state = (orig & ~UXT_PRIO) | prio;
	} else {
		if (!in)	/* clear all ux types but animator */
			*state = orig & ~(UXT_TYPES & ~SA_TYPE_ANIMATOR);
		else		/* clear the given types */
			*state = orig & ~in;
		/* an inherited ux stays until the inheritance ends */
		if (!(*state & UXT_TYPES) && (orig & SA_TYPE_INHERIT))
			return false;
	}
	return true;
}

static bool uxt_verified(struct task_struct *task, bool from_sys)
{
	uid_t uid = current_uid().val;

	if (from_sys)	/* system may control any task */
		return uid == UXT_ROOT_UID || uid == UXT_SYSTEM_UID;

	/* an app (or system) only its own threads */
	uid %= UXT_PER_USER_RANGE;
	return current->tgid == task->tgid && (uid == UXT_SYSTEM_UID ||
		(uid >= UXT_FIRST_APPLICATION_UID && uid <= UXT_LAST_APPLICATION_UID));
}

/* referenced task tid of process pid, or an error */
static struct task_struct *uxt_get_task(pid_t pid, pid_t tid, bool from_sys, long *err)
{
	struct task_struct *task;

	rcu_read_lock();
	task = find_task_by_vpid(tid);
	if (!task)
		*err = -ESRCH;
	else if (task->tgid != pid)
		*err = -EINVAL;
	else if (!uxt_verified(task, from_sys))
		*err = -EPERM;
	else
		get_task_struct(task);
	rcu_read_unlock();

	return *err ? NULL : task;
}

static long uxt_read(pid_t pid, pid_t tid, bool from_sys)
{
	long ret = 0;
	struct task_struct *task = uxt_get_task(pid, tid, from_sys, &ret);

	if (!task)
		return ret;
	ret = uxt_state_to_510(READ_ONCE(task->ux_state));
	put_task_struct(task);
	return ret;
}

static long uxt_write(pid_t pid, pid_t tid, int v, bool from_sys)
{
	long ret = 0;
	int state;
	struct task_struct *task;

	/* set and reset are mutually exclusive */
	if ((v & UXT_OPT_RESET) && (v & SA_OPT_SET))
		return -EINVAL;

	task = uxt_get_task(pid, tid, from_sys, &ret);
	if (!task)
		return ret;

	mutex_lock(&uxt_mutex);
	state = READ_ONCE(task->ux_state);
	if (uxt_new_state(state, v, &state)) {
		oplus_set_ux_state_lock(task, state, true);
		sched_assist_systrace_pid(task->tgid, state, "ux_state %d", task->pid);
	}
	mutex_unlock(&uxt_mutex);

	ret = uxt_state_to_510(state);
	put_task_struct(task);
	return ret;
}

static long uxt_ioctl(unsigned int cmd, unsigned long arg, bool from_sys)
{
	void __user *uarg = (void __user *)arg;

	if (cmd == CMD_UX_READ) {
		int p[2];

		if (copy_from_user(p, uarg, sizeof(p)))
			return -EFAULT;
		return uxt_read(p[0], p[1], from_sys);
	} else if (cmd == CMD_UX_WRITE) {
		int p[3];

		if (copy_from_user(p, uarg, sizeof(p)))
			return -EFAULT;
		return uxt_write(p[0], p[1], p[2], from_sys);
	}

	return -ENOTTY;
}

static long proc_ux_task_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return uxt_ioctl(cmd, arg, true);
}

static long proc_ux_task_app_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return uxt_ioctl(cmd, arg, false);
}

#ifdef CONFIG_COMPAT
static long proc_ux_task_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return uxt_ioctl(cmd, (unsigned long)compat_ptr(arg), true);
}

static long proc_ux_task_app_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return uxt_ioctl(cmd, (unsigned long)compat_ptr(arg), false);
}
#endif

/*
 * Text interface (debugging, as on 5.10):
 *   echo "p <tid> <5.10 ux value>" > ux_task    set, e.g. "p 1611 129" = SET|LIGHT
 *   echo "r <tid>" > ux_task; cat ux_task         read
 */
static ssize_t proc_ux_task_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[64], op;
	int tid, v, n;
	long ret;
	struct task_struct *task;
	pid_t tgid;

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;
	buffer[count] = '\0';

	n = sscanf(buffer, " %c %d %d", &op, &tid, &v);
	if (n == 2 && op == 'r') {
		uxt_read_pid = tid;
		return count;
	}
	if (n != 3 || op != 'p' || tid <= 0 || v < 0)
		return -EINVAL;

	rcu_read_lock();
	task = find_task_by_vpid(tid);
	tgid = task ? task->tgid : 0;
	rcu_read_unlock();
	if (!task)
		return -ESRCH;

	ret = uxt_write(tgid, tid, v, true);
	return ret < 0 ? ret : count;
}

static ssize_t proc_ux_task_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[160];
	size_t len;
	struct task_struct *task;

	rcu_read_lock();
	task = find_task_by_vpid(uxt_read_pid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	if (task) {
		int s = READ_ONCE(task->ux_state);

		len = snprintf(buffer, sizeof(buffer), "comm=%s pid=%d tgid=%d ux_state=0x%08x (5.10: 0x%08x)\n",
			task->comm, task->pid, task->tgid, s, uxt_state_to_510(s));
		put_task_struct(task);
	} else {
		len = snprintf(buffer, sizeof(buffer), "Can not find task\n");
	}

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static const struct file_operations proc_ux_task_fops = {
	.write		= proc_ux_task_write,
	.read		= proc_ux_task_read,
	.unlocked_ioctl	= proc_ux_task_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= proc_ux_task_compat_ioctl,
#endif
};

static const struct file_operations proc_ux_task_app_fops = {
	.unlocked_ioctl	= proc_ux_task_app_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= proc_ux_task_app_compat_ioctl,
#endif
};

int oplus_ux_task_proc_init(void)
{
	if (!d_sched_assist)
		return -ENOENT;

	if (!proc_create("ux_task", 0666, d_sched_assist, &proc_ux_task_fops) ||
	    !proc_create("ux_task_app", 0666, d_sched_assist, &proc_ux_task_app_fops)) {
		ux_err("failed to create proc node ux_task\n");
		remove_proc_entry("ux_task", d_sched_assist);
		return -ENOENT;
	}

	return 0;
}
