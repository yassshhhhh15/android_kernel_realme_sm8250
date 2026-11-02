// SPDX-License-Identifier: GPL-2.0-only
/*
 * /proc/oplus_binder/ux_flag for ColorOS 16+ user space (pipa port, walt-r10).
 *
 * ColorOS 17 libSchedAssistExtImpl.so opens this node once per process and calls
 * ioctl(fd, 0x407a6900, ...) (binderSetThreadUxFlagByIoctl) to mark binder threads for OPPO's async binder UX.
 * Without the node every call logs "mSharedBinderUxFd=-1 is invalid" (about 5 lines/s in system_server under load).
 * The async binder UX of OPPO's GKI kernels needs android_vh_binder_* hooks that this 4.19 binder.c does not have,
 * so the ioctl is accepted and does nothing. Sync binder UX inheritance (binder_set_inherit_ux in binder.c) works
 * as before. The ROM policy already labels the node (genfscon proc /oplus_binder -> oplus_binder_proc) and allows
 * ioctl 0x6900.
 * Built only when CONFIG_OPLUS_FEATURE_ASYNC_BINDER_INHERIT_UX is off (binder_sysfs.c makes the node then).
 */
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#define OPLUS_BINDER_UX_FLAG_IOC	0x6900	/* type 'i', nr 0; the size field varies with the struct */

static long ux_flag_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if ((cmd & 0xffff) != OPLUS_BINDER_UX_FLAG_IOC)
		return -ENOTTY;
	return 0;
}

static ssize_t ux_flag_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	return count;
}

static const struct file_operations ux_flag_fops = {
	.owner		= THIS_MODULE,
	.write		= ux_flag_write,
	.unlocked_ioctl	= ux_flag_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ux_flag_ioctl,
#endif
	.llseek		= noop_llseek,
};

static int __init sa_binder_ux_flag_init(void)
{
	struct proc_dir_entry *dir = proc_mkdir("oplus_binder", NULL);

	if (!dir)
		return -ENOMEM;
	if (!proc_create("ux_flag", 0666, dir, &ux_flag_fops)) {
		remove_proc_entry("oplus_binder", NULL);
		return -ENOMEM;
	}
	return 0;
}
late_initcall(sa_binder_ux_flag_init);
