// SPDX-License-Identifier: GPL-2.0
/*
 * pipa-cos17: touch boost through uclamp (from our E404 kernel, where it replaced WALT's boosts for CASS).
 *
 * Raises uclamp.min of top-app and foreground above the little cores' capacity (313/1024 on kona) while the
 * screen is touched, so the UI starts on the big cores and schedutil raises their clocks. On E404 r2b
 * (Spotify spam at 144 Hz) it took jank from 21-32 % to 13-15 %. On WALT the QTI perf HAL boosts work, so
 * it is off by default here until an A/B shows it helps.
 * The boost starts with the first input event and ends ms milliseconds after the last one.
 *   /sys/module/uclamp_boost/parameters/{pct,ms}   (pct=0 disables)
 */
#define pr_fmt(fmt) "uclamp_boost: " fmt

#include <linux/cgroup.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "sched.h"

static unsigned int pct;
module_param(pct, uint, 0644);
static unsigned int ms = 1500;
module_param(ms, uint, 0644);

static const char *const uib_names[] = { "top-app", "foreground" };
static struct cgroup_subsys_state *uib_css[ARRAY_SIZE(uib_names)];
static unsigned int uib_saved[ARRAY_SIZE(uib_names)];
static atomic_t uib_active = ATOMIC_INIT(0);

static void uib_boost_fn(struct work_struct *work);
static void uib_unboost_fn(struct work_struct *work);
static DECLARE_WORK(uib_boost_work, uib_boost_fn);
static DECLARE_DELAYED_WORK(uib_unboost_work, uib_unboost_fn);

/* called by cpu_cgroup_css_online() for the top-level cpu cgroups */
void uclamp_boost_register(struct cgroup_subsys_state *css)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(uib_names); i++) {
		if (!uib_css[i] && !strcmp(css->cgroup->kn->name, uib_names[i]) && css_tryget(css)) {
			uib_css[i] = css;
			pr_info("tracking %s\n", uib_names[i]);
		}
	}
}

static void uib_boost_fn(struct work_struct *work)
{
	char buf[8];
	int i;

	snprintf(buf, sizeof(buf), "%u", min(pct, 100U));
	for (i = 0; i < ARRAY_SIZE(uib_names); i++) {
		if (!uib_css[i])
			continue;
		uib_saved[i] = container_of(uib_css[i], struct task_group, css)->uclamp_pct[UCLAMP_MIN];
		if (uib_saved[i] < pct * 100)
			cpu_uclamp_write_wrapper(uib_css[i], buf, UCLAMP_MIN);
	}
}

static void uib_unboost_fn(struct work_struct *work)
{
	char buf[16];
	int i;

	flush_work(&uib_boost_work);
	for (i = 0; i < ARRAY_SIZE(uib_names); i++) {
		if (!uib_css[i])
			continue;
		/* uclamp_pct is in hundredths of a percent */
		snprintf(buf, sizeof(buf), "%u.%02u", uib_saved[i] / 100, uib_saved[i] % 100);
		cpu_uclamp_write_wrapper(uib_css[i], buf, UCLAMP_MIN);
	}
	atomic_set(&uib_active, 0);
}

static void uib_input_event(struct input_handle *handle, unsigned int type, unsigned int code, int value)
{
	if (!pct)
		return;
	if (!atomic_xchg(&uib_active, 1))
		queue_work(system_highpri_wq, &uib_boost_work);
	mod_delayed_work(system_highpri_wq, &uib_unboost_work, msecs_to_jiffies(ms));
}

static int uib_input_connect(struct input_handler *handler, struct input_dev *dev,
			     const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "uclamp_boost_handle";
	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;
	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;
	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void uib_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id uib_ids[] = {
	/* multi-touch touchscreen (also the pen: it reports ABS_MT on pipa) */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			    BIT_MASK(ABS_MT_POSITION_X) | BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* touchpad (keyboard cover) */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] = BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) }
	},
	{ }
};

static struct input_handler uib_input_handler = {
	.event		= uib_input_event,
	.connect	= uib_input_connect,
	.disconnect	= uib_input_disconnect,
	.name		= "uclamp_boost",
	.id_table	= uib_ids,
};

static int __init uclamp_boost_init(void)
{
	int ret = input_register_handler(&uib_input_handler);

	if (ret)
		pr_err("input handler failed: %d\n", ret);
	return ret;
}
late_initcall(uclamp_boost_init);
