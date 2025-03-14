// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2015,2017,2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2017, Paranoid Android.
 */
#define pr_fmt(fmt) "cpu-boost: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/time.h>
#include <linux/sysfs.h>
#include <linux/pm_qos.h>
#include <linux/sched/rt.h>
#include <uapi/linux/sched/types.h>
#include <drm/mi_disp_notifier.h>

#include "qc_vas.h"

#define cpu_boost_attr_rw(_name)		\
static struct kobj_attribute _name##_attr =	\
__ATTR(_name, 0644, show_##_name, store_##_name)

#define show_one(file_name)			\
static ssize_t show_##file_name			\
(struct kobject *kobj, struct kobj_attribute *attr, char *buf)	\
{								\
	return scnprintf(buf, PAGE_SIZE, "%u\n", file_name);	\
}

#define store_one(file_name)					\
static ssize_t store_##file_name				\
(struct kobject *kobj, struct kobj_attribute *attr,		\
const char *buf, size_t count)					\
{								\
								\
	sscanf(buf, "%u", &file_name);				\
	return count;						\
}

struct cpu_sync {
	int cpu;
	unsigned int input_boost_min;
	unsigned int input_boost_freq;
};

static DEFINE_PER_CPU(struct cpu_sync, sync_info);

static struct kthread_work input_boost_work;

static bool input_boost_enabled;

static unsigned int input_boost_ms = 40;
show_one(input_boost_ms);
store_one(input_boost_ms);
cpu_boost_attr_rw(input_boost_ms);

static unsigned int sched_boost_on_input;
show_one(sched_boost_on_input);
store_one(sched_boost_on_input);
cpu_boost_attr_rw(sched_boost_on_input);

static unsigned int wake_boost_enable = 1;
show_one(wake_boost_enable);
store_one(wake_boost_enable);
cpu_boost_attr_rw(wake_boost_enable);

static unsigned int wake_boost_ms = 1000;
show_one(wake_boost_ms);
store_one(wake_boost_ms);
cpu_boost_attr_rw(wake_boost_ms);

static bool sched_boost_active;
static bool wake_boost_active;

static struct delayed_work input_boost_rem;
static u64 last_input_time;

static struct kthread_worker cpu_boost_worker;
static struct task_struct *cpu_boost_worker_thread;

#define MIN_INPUT_INTERVAL (100 * USEC_PER_MSEC)

static DEFINE_PER_CPU(struct freq_qos_request, qos_req);

static ssize_t store_input_boost_freq(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	int i, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	bool enabled = false;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* single number: apply to all CPUs */
	if (!ntokens) {
		if (sscanf(buf, "%u\n", &val) != 1)
			return -EINVAL;
		for_each_possible_cpu(i)
			per_cpu(sync_info, i).input_boost_freq = val;
		goto check_enable;
	}

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu >= num_possible_cpus())
			return -EINVAL;

		per_cpu(sync_info, cpu).input_boost_freq = val;
		cp = strnchr(cp, PAGE_SIZE - (cp - buf), ' ');
		cp++;
	}

check_enable:
	for_each_possible_cpu(i) {
		if (per_cpu(sync_info, i).input_boost_freq) {
			enabled = true;
			break;
		}
	}
	input_boost_enabled = enabled;

	return count;
}

static ssize_t show_input_boost_freq(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	int cnt = 0, cpu;
	struct cpu_sync *s;

	for_each_possible_cpu(cpu) {
		s = &per_cpu(sync_info, cpu);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt,
				"%d:%u ", cpu, s->input_boost_freq);
	}
	cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	return cnt;
}

cpu_boost_attr_rw(input_boost_freq);

static void boost_adjust_notify(struct cpufreq_policy *policy)
{
	unsigned int cpu = policy->cpu;
	struct cpu_sync *s = &per_cpu(sync_info, cpu);
	unsigned int ib_min = wake_boost_active ?
		policy->cpuinfo.max_freq : s->input_boost_min;
	struct freq_qos_request *req = &per_cpu(qos_req, cpu);
	int ret;

	pr_debug("Wake boost active = %d\n", wake_boost_active);
	pr_debug("CPU%u policy min before boost: %u kHz\n",
			 cpu, policy->min);
	pr_debug("CPU%u boost min: %u kHz\n", cpu, ib_min);

	ret = freq_qos_update_request(req, ib_min);

	if (ret < 0)
		pr_err("Failed to update freq constraint in boost_adjust: %d\n",
								ib_min);

	pr_debug("CPU%u policy min after boost: %u kHz\n",
		 cpu, policy->min);

	return;
}

static void update_policy_online(void)
{
	unsigned int i;
	struct cpufreq_policy *policy;
	struct cpumask online_cpus;
	/* Re-evaluate policy to trigger adjust notifier for online CPUs */
	get_online_cpus();
	online_cpus = *cpu_online_mask;
	for_each_cpu(i, &online_cpus) {
		policy = cpufreq_cpu_get(i);
		if (!policy) {
			pr_err("%s: cpufreq policy not found for cpu%d\n",
							__func__, i);
			return;
		}

		cpumask_andnot(&online_cpus, &online_cpus,
						policy->related_cpus);
		boost_adjust_notify(policy);
	}
	put_online_cpus();
}

static void do_input_boost_rem(struct work_struct *work)
{
	unsigned int i, ret;
	struct cpu_sync *i_sync_info;

	/* Reset the input_boost_min for all CPUs in the system */
	pr_debug("Resetting input boost min for all CPUs\n");
	for_each_possible_cpu(i) {
		i_sync_info = &per_cpu(sync_info, i);
		i_sync_info->input_boost_min = 0;
	}

	/* Reset wake boost */
	pr_debug("Resetting wake boost");
	wake_boost_active = false;

	/* Update policies for all online CPUs */
	update_policy_online();

	if (sched_boost_active) {
		ret = sched_set_boost(0);
		if (ret)
			pr_err("cpu-boost: sched boost disable failed\n");
		sched_boost_active = false;
	}
}

static void do_input_boost(struct kthread_work *work)
{
	unsigned int i, ret;
	struct cpu_sync *i_sync_info;

	cancel_delayed_work_sync(&input_boost_rem);
	if (sched_boost_active) {
		sched_set_boost(0);
		sched_boost_active = false;
	}

	/* Set the input_boost_min for all CPUs in the system */
	pr_debug("Setting input boost min for all CPUs\n");
	for_each_possible_cpu(i) {
		i_sync_info = &per_cpu(sync_info, i);
		i_sync_info->input_boost_min = i_sync_info->input_boost_freq;
	}

	/* Update policies for all online CPUs */
	update_policy_online();

	/* Enable scheduler boost to migrate tasks to big cluster */
	if (sched_boost_on_input > 0) {
		ret = sched_set_boost(sched_boost_on_input);
		if (ret)
			pr_err("cpu-boost: sched boost enable failed\n");
		else
			sched_boost_active = true;
	}

	pr_debug("Wake boost active = %d\n", wake_boost_active);
	schedule_delayed_work(&input_boost_rem,
		msecs_to_jiffies(wake_boost_active ? wake_boost_ms : input_boost_ms));
}

static void cpuboost_queue_work(void)
{
	if (queuing_blocked(&cpu_boost_worker, &input_boost_work))
		return;

	kthread_queue_work(&cpu_boost_worker, &input_boost_work);
}

static void cpuboost_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	u64 now;

	pr_debug("Wake boost active = %d\n", wake_boost_active);
	if (!input_boost_enabled || wake_boost_active)
		return;

	now = ktime_to_us(ktime_get());
	if (now - last_input_time < MIN_INPUT_INTERVAL)
		return;

	cpuboost_queue_work();
	last_input_time = ktime_to_us(ktime_get());
}

static int cpuboost_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void cpuboost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpuboost_ids[] = {
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler cpuboost_input_handler = {
	.event          = cpuboost_input_event,
	.connect        = cpuboost_input_connect,
	.disconnect     = cpuboost_input_disconnect,
	.name           = "cpu-boost",
	.id_table       = cpuboost_ids,
};

static int mi_disp_notifier_cb(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct mi_disp_notifier *evdata = data;
	int *blank = evdata->data;

	pr_debug("Received display notifier callback\n");

	if (wake_boost_enable && !wake_boost_active &&
		action == MI_DISP_DPMS_EVENT && *blank == MI_DISP_DPMS_ON) {
		pr_debug("Going to wakeboost\n");
		wake_boost_active = true;
		cpuboost_queue_work();
	}

	return NOTIFY_OK;
}

static struct notifier_block mi_disp_notif = {
	.notifier_call = mi_disp_notifier_cb,
	.priority = INT_MAX,
};

static struct kobject *cpu_boost_kobj;
int cpu_boost_init(void)
{
	int cpu, ret;
	struct cpu_sync *s;
	struct cpufreq_policy *policy;
	struct freq_qos_request *req;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 2 };

	kthread_init_worker(&cpu_boost_worker);
	cpu_boost_worker_thread = kthread_run_perf_critical(cpu_perf_mask,
		kthread_worker_fn, &cpu_boost_worker, "cpu_boost_worker_thread");
	if (IS_ERR(cpu_boost_worker_thread))
		return -EFAULT;

	sched_setscheduler(cpu_boost_worker_thread, SCHED_FIFO, &param);
	kthread_init_work(&input_boost_work, do_input_boost);
	INIT_DELAYED_WORK(&input_boost_rem, do_input_boost_rem);

	for_each_possible_cpu(cpu) {
		s = &per_cpu(sync_info, cpu);
		s->cpu = cpu;
		req = &per_cpu(qos_req, cpu);
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_err("%s: cpufreq policy not found for cpu%d\n",
							__func__, cpu);
			return -ESRCH;
		}

		ret = freq_qos_add_request(&policy->constraints, req,
						FREQ_QOS_MIN, policy->min);
		if (ret < 0) {
			pr_err("%s: Failed to add freq constraint (%d)\n",
							__func__, ret);
			return ret;
		}

	}

	cpu_boost_kobj = kobject_create_and_add("cpu_boost",
						&cpu_subsys.dev_root->kobj);
	if (!cpu_boost_kobj)
		pr_err("Failed to initialize sysfs node for cpu_boost.\n");

	ret = sysfs_create_file(cpu_boost_kobj, &input_boost_ms_attr.attr);
	if (ret)
		pr_err("Failed to create input_boost_ms node: %d\n", ret);

	ret = sysfs_create_file(cpu_boost_kobj, &input_boost_freq_attr.attr);
	if (ret)
		pr_err("Failed to create input_boost_freq node: %d\n", ret);

	ret = sysfs_create_file(cpu_boost_kobj,
				&sched_boost_on_input_attr.attr);
	if (ret)
		pr_err("Failed to create sched_boost_on_input node: %d\n", ret);

	ret = sysfs_create_file(cpu_boost_kobj, &wake_boost_enable_attr.attr);
	if (ret)
		pr_err("Failed to create wake_boost_enable node: %d\n", ret);

	ret = sysfs_create_file(cpu_boost_kobj, &wake_boost_ms_attr.attr);
	if (ret)
		pr_err("Failed to create wake_boost_ms node: %d\n", ret);

	ret = mi_disp_register_client(&mi_disp_notif);
	if (ret)
		pr_err("Failed to register display notifier: %d\n", ret);

	ret = input_register_handler(&cpuboost_input_handler);
	return 0;
}
