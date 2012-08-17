/*
 * CPUFreq ondemand governor
 *
 * Copyright (C) 2010 Texas Instruments, Inc.
 *   Mike Turquette <mturquette@ti.com>
 *   Santosh Shilimkar <santosh.shilimkar@ti.com>
 *
 * Based on ondemand governor
 * Copyright (C)  2001 Russell King
 *           (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>,
 *                     Jun Nakajima <jun.nakajima@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Modified for early suspend support and tweaks by imoseyon (imoseyon@gmail.com)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/earlysuspend.h>

// used for suspend code
static unsigned int susp_enabled = 0;
static unsigned int suspended = 0;
static unsigned int registration = 0;
static unsigned int suspend_cpu_up = 95;
static unsigned int highfreq = 1375000;

/* greater than 80% avg load across online CPUs increases frequency */
#define DEFAULT_UP_FREQ_MIN_LOAD			(80)

/* Keep 10% of idle under the up threshold when decreasing the frequency */
#define DEFAULT_FREQ_DOWN_DIFFERENTIAL			(10)

/* less than 35% avg load across online CPUs decreases frequency */
#define DEFAULT_DOWN_FREQ_MAX_LOAD			(35)

/* default sampling period (uSec) is bogus; 10x ondemand's default for x86 */
#define DEFAULT_SAMPLING_PERIOD				(100000)

/* default number of sampling periods to average before ondemand-in decision */
#define DEFAULT_ondemand_IN_SAMPLING_PERIODS		(5)

/* default number of sampling periods to average before ondemand-out decision */
#define DEFAULT_ondemand_OUT_SAMPLING_PERIODS		(20)

static void do_dbs_timer(struct work_struct *work);
static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
		unsigned int event);

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_ondemand
static
#endif
struct cpufreq_governor cpufreq_gov_ondemand = {
       .name                   = "ondemand",
       .governor               = cpufreq_governor_dbs,
       .owner                  = THIS_MODULE,
};

struct cpu_dbs_info_s {
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_nice;
	struct cpufreq_policy *cur_policy;
	struct delayed_work work;
	struct cpufreq_frequency_table *freq_table;
	int cpu;
	/*
	 * percpu mutex that serializes governor limit change with
	 * do_dbs_timer invocation. We do not want do_dbs_timer to run
	 * when user is changing the governor or limits.
	 */
	struct mutex timer_mutex;
};
static DEFINE_PER_CPU(struct cpu_dbs_info_s, hp_cpu_dbs_info);

static unsigned int dbs_enable;	/* number of CPUs using this policy */

/*
 * dbs_mutex protects data in dbs_tuners_ins from concurrent changes on
 * different CPUs. It protects dbs_enable in governor start/stop.
 */
static DEFINE_MUTEX(dbs_mutex);

static struct workqueue_struct	*kondemand_wq;

static struct dbs_tuners {
	unsigned int sampling_rate;
	unsigned int up_threshold;
	unsigned int down_differential;
	unsigned int down_threshold;
	unsigned int ondemand_in_sampling_periods;
	unsigned int ondemand_out_sampling_periods;
	unsigned int ondemand_load_index;
	unsigned int *ondemand_load_history;
	unsigned int ignore_nice;
	unsigned int io_is_busy;
} dbs_tuners_ins = {
	.sampling_rate =		DEFAULT_SAMPLING_PERIOD,
	.up_threshold =			DEFAULT_UP_FREQ_MIN_LOAD,
	.down_differential =            DEFAULT_FREQ_DOWN_DIFFERENTIAL,
	.down_threshold =		DEFAULT_DOWN_FREQ_MAX_LOAD,
	.ondemand_in_sampling_periods =	DEFAULT_ondemand_IN_SAMPLING_PERIODS,
	.ondemand_out_sampling_periods =	DEFAULT_ondemand_OUT_SAMPLING_PERIODS,
	.ondemand_load_index =		0,
	.ignore_nice =			0,
	.io_is_busy =			0,
};

/*
 * A corner case exists when switching io_is_busy at run-time: comparing idle
 * times from a non-io_is_busy period to an io_is_busy period (or vice-versa)
 * will misrepresent the actual change in system idleness.  We ignore this
 * corner case: enabling io_is_busy might cause freq increase and disabling
 * might cause freq decrease, which probably matches the original intent.
 */
static inline cputime64_t get_cpu_idle_time(unsigned int cpu, cputime64_t *wall)
{
        u64 idle_time;
        u64 iowait_time;

        /* cpufreq-ondemand always assumes CONFIG_NO_HZ */
        idle_time = get_cpu_idle_time_us(cpu, wall);

	/* add time spent doing I/O to idle time */
        if (dbs_tuners_ins.io_is_busy) {
                iowait_time = get_cpu_iowait_time_us(cpu, wall);
                /* cpufreq-ondemand always assumes CONFIG_NO_HZ */
                if (iowait_time != -1ULL && idle_time >= iowait_time)
                        idle_time -= iowait_time;
        }

        return idle_time;
}

/************************** sysfs interface ************************/

/* XXX look at global sysfs macros in cpufreq.h, can those be used here? */

/* cpufreq_ondemand Governor Tunables */
#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", dbs_tuners_ins.object);		\
}
show_one(sampling_rate, sampling_rate);
show_one(up_threshold, up_threshold);
show_one(down_differential, down_differential);
show_one(down_threshold, down_threshold);
show_one(ondemand_in_sampling_periods, ondemand_in_sampling_periods);
show_one(ondemand_out_sampling_periods, ondemand_out_sampling_periods);
show_one(ignore_nice_load, ignore_nice);
show_one(io_is_busy, io_is_busy);

static ssize_t store_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.sampling_rate = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_up_threshold(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input <= dbs_tuners_ins.down_threshold) {
		return -EINVAL;
	}

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.up_threshold = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_down_differential(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input >= dbs_tuners_ins.up_threshold)
		return -EINVAL;

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.down_differential = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_down_threshold(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input >= dbs_tuners_ins.up_threshold) {
		return -EINVAL;
	}

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.down_threshold = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_ondemand_in_sampling_periods(struct kobject *a,
		struct attribute *b, const char *buf, size_t count)
{
	unsigned int input;
	unsigned int *temp;
	unsigned int max_windows;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	/* already using this value, bail out */
	if (input == dbs_tuners_ins.ondemand_in_sampling_periods)
		return count;

	mutex_lock(&dbs_mutex);
	ret = count;
	max_windows = max(dbs_tuners_ins.ondemand_in_sampling_periods,
			dbs_tuners_ins.ondemand_out_sampling_periods);

	/* no need to resize array */
	if (input <= max_windows) {
		dbs_tuners_ins.ondemand_in_sampling_periods = input;
		goto out;
	}

	/* resize array */
	temp = kmalloc((sizeof(unsigned int) * input), GFP_KERNEL);

	if (!temp || IS_ERR(temp)) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy(temp, dbs_tuners_ins.ondemand_load_history,
			(max_windows * sizeof(unsigned int)));
	kfree(dbs_tuners_ins.ondemand_load_history);

	/* replace old buffer, old number of sampling periods & old index */
	dbs_tuners_ins.ondemand_load_history = temp;
	dbs_tuners_ins.ondemand_in_sampling_periods = input;
	dbs_tuners_ins.ondemand_load_index = max_windows;
out:
	mutex_unlock(&dbs_mutex);

	return ret;
}

static ssize_t store_ondemand_out_sampling_periods(struct kobject *a,
		struct attribute *b, const char *buf, size_t count)
{
	unsigned int input;
	unsigned int *temp;
	unsigned int max_windows;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	/* already using this value, bail out */
	if (input == dbs_tuners_ins.ondemand_out_sampling_periods)
		return count;

	mutex_lock(&dbs_mutex);
	ret = count;
	max_windows = max(dbs_tuners_ins.ondemand_in_sampling_periods,
			dbs_tuners_ins.ondemand_out_sampling_periods);

	/* no need to resize array */
	if (input <= max_windows) {
		dbs_tuners_ins.ondemand_out_sampling_periods = input;
		goto out;
	}

	/* resize array */
	temp = kmalloc((sizeof(unsigned int) * input), GFP_KERNEL);

	if (!temp || IS_ERR(temp)) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy(temp, dbs_tuners_ins.ondemand_load_history,
			(max_windows * sizeof(unsigned int)));
	kfree(dbs_tuners_ins.ondemand_load_history);

	/* replace old buffer, old number of sampling periods & old index */
	dbs_tuners_ins.ondemand_load_history = temp;
	dbs_tuners_ins.ondemand_out_sampling_periods = input;
	dbs_tuners_ins.ondemand_load_index = max_windows;
out:
	mutex_unlock(&dbs_mutex);

	return ret;
}

static ssize_t store_ignore_nice_load(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	unsigned int j;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	mutex_lock(&dbs_mutex);
	if (input == dbs_tuners_ins.ignore_nice) { /* nothing to do */
		mutex_unlock(&dbs_mutex);
		return count;
	}
	dbs_tuners_ins.ignore_nice = input;

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct cpu_dbs_info_s *dbs_info;
		dbs_info = &per_cpu(hp_cpu_dbs_info, j);
		dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&dbs_info->prev_cpu_wall);
		if (dbs_tuners_ins.ignore_nice)
			dbs_info->prev_cpu_nice = kstat_cpu(j).cpustat.nice;

	}
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_io_is_busy(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.io_is_busy = !!input;
	mutex_unlock(&dbs_mutex);

	return count;
}

define_one_global_rw(sampling_rate);
define_one_global_rw(up_threshold);
define_one_global_rw(down_differential);
define_one_global_rw(down_threshold);
define_one_global_rw(ondemand_in_sampling_periods);
define_one_global_rw(ondemand_out_sampling_periods);
define_one_global_rw(ignore_nice_load);
define_one_global_rw(io_is_busy);

static struct attribute *dbs_attributes[] = {
	&sampling_rate.attr,
	&up_threshold.attr,
	&down_differential.attr,
	&down_threshold.attr,
	&ondemand_in_sampling_periods.attr,
	&ondemand_out_sampling_periods.attr,
	&ignore_nice_load.attr,
	&io_is_busy.attr,
	NULL
};

static struct attribute_group dbs_attr_group = {
	.attrs = dbs_attributes,
	.name = "ondemand",
};

/************************** sysfs end ************************/

static void dbs_check_cpu(struct cpu_dbs_info_s *this_dbs_info)
{
	/* combined load of all enabled CPUs */
	unsigned int total_load = 0;
	/* single largest CPU load percentage*/
	unsigned int max_load = 0;
	/* largest CPU load in terms of frequency */
	unsigned int max_load_freq = 0;
	/* average load across all enabled CPUs */
	unsigned int avg_load = 0;
	/* average load across multiple sampling periods for ondemand events */
	unsigned int ondemand_in_avg_load = 0;
	unsigned int ondemand_out_avg_load = 0;
	/* number of sampling periods averaged for ondemand decisions */
	unsigned int periods;

	struct cpufreq_policy *policy;
	unsigned int i, j;

	policy = this_dbs_info->cur_policy;

	/*
	 * cpu load accounting
	 * get highest load, total load and average load across all CPUs
	 */
	for_each_cpu(j, policy->cpus) {
		unsigned int load;
		unsigned int idle_time, wall_time;
		cputime64_t cur_wall_time, cur_idle_time;
		struct cpu_dbs_info_s *j_dbs_info;

		j_dbs_info = &per_cpu(hp_cpu_dbs_info, j);

		/* update both cur_idle_time and cur_wall_time */
		cur_idle_time = get_cpu_idle_time(j, &cur_wall_time);

		/* how much wall time has passed since last iteration? */
		wall_time = (unsigned int) cputime64_sub(cur_wall_time,
				j_dbs_info->prev_cpu_wall);
		j_dbs_info->prev_cpu_wall = cur_wall_time;

		/* how much idle time has passed since last iteration? */
		idle_time = (unsigned int) cputime64_sub(cur_idle_time,
				j_dbs_info->prev_cpu_idle);
		j_dbs_info->prev_cpu_idle = cur_idle_time;

		if (unlikely(!wall_time || wall_time < idle_time))
			continue;

		/* load is the percentage of time not spent in idle */
		load = 100 * (wall_time - idle_time) / wall_time;

		/* keep track of combined load across all CPUs */
		total_load += load;

		/* keep track of highest single load across all CPUs */
		if (load > max_load)
			max_load = load;
	}

	/* use the max load in the OPP freq change policy */
	max_load_freq = max_load * policy->cur;

	/* calculate the average load across all related CPUs */
	avg_load = total_load / num_online_cpus();


	/*
	 * ondemand load accounting
	 * average load over multiple sampling periods
	 */

	/* how many sampling periods do we use for ondemand decisions? */
	periods = max(dbs_tuners_ins.ondemand_in_sampling_periods,
			dbs_tuners_ins.ondemand_out_sampling_periods);

	/* store avg_load in the circular buffer */
	dbs_tuners_ins.ondemand_load_history[dbs_tuners_ins.ondemand_load_index]
		= avg_load;

	/* compute average load across in & out sampling periods */
	for (i = 0, j = dbs_tuners_ins.ondemand_load_index;
			i < periods; i++, j--) {
		if (i < dbs_tuners_ins.ondemand_in_sampling_periods)
			ondemand_in_avg_load +=
				dbs_tuners_ins.ondemand_load_history[j];
		if (i < dbs_tuners_ins.ondemand_out_sampling_periods)
			ondemand_out_avg_load +=
				dbs_tuners_ins.ondemand_load_history[j];

		if (j == 0)
			j = periods;
	}

	ondemand_in_avg_load = ondemand_in_avg_load /
		dbs_tuners_ins.ondemand_in_sampling_periods;

	ondemand_out_avg_load = ondemand_out_avg_load /
		dbs_tuners_ins.ondemand_out_sampling_periods;

	/* return to first element if we're at the circular buffer's end */
	if (++dbs_tuners_ins.ondemand_load_index == periods)
		dbs_tuners_ins.ondemand_load_index = 0;

	/* check if auxiliary CPU is needed based on avg_load */
	/* imoseyon - support cpu_up even if suspended if load is super high */
	if (!suspended || (suspended && avg_load > suspend_cpu_up))
	  if (avg_load > dbs_tuners_ins.up_threshold) {
		/* should we enable auxillary CPUs? */
		if (num_online_cpus() < 2 && ondemand_in_avg_load >
				dbs_tuners_ins.up_threshold) {
			/* ondemand with cpufreq is nasty
			 * a call to cpufreq_governor_dbs may cause a lockup.
			 * wq is not running here so its safe.
			 */
			mutex_unlock(&this_dbs_info->timer_mutex);
			cpu_up(1);
			mutex_lock(&this_dbs_info->timer_mutex);
			goto out;
		}
	}

	/* check for frequency increase based on max_load */
	if (max_load > dbs_tuners_ins.up_threshold) {
		// imosey go to highest freq only if load is really high
		if (max_load < suspend_cpu_up && highfreq <= policy->max) 
			__cpufreq_driver_target(policy, highfreq,
					CPUFREQ_RELATION_H);
		else
		/* increase to highest frequency supported */
		if (policy->cur < policy->max)
			__cpufreq_driver_target(policy, policy->max,
					CPUFREQ_RELATION_H);

		goto out;
	}

	/* check for frequency decrease */
	if (avg_load < dbs_tuners_ins.down_threshold) {
		/* are we at the minimum frequency already? */
		if (policy->cur == policy->min) {
			/* should we disable auxillary CPUs? */
			if (num_online_cpus() > 1 && ondemand_out_avg_load <
					dbs_tuners_ins.down_threshold) {
				mutex_unlock(&this_dbs_info->timer_mutex);
				cpu_down(1);
				mutex_lock(&this_dbs_info->timer_mutex);
			}
			goto out;
		}
	}

	/*
	 * go down to the lowest frequency which can sustain the load by
	 * keeping 30% of idle in order to not cross the up_threshold
	 */
	if ((max_load_freq <
	    (dbs_tuners_ins.up_threshold - dbs_tuners_ins.down_differential) *
	     policy->cur) && (policy->cur > policy->min)) {
		unsigned int freq_next;
		freq_next = max_load_freq /
				(dbs_tuners_ins.up_threshold -
				 dbs_tuners_ins.down_differential);

		if (freq_next < policy->min)
			freq_next = policy->min;

		 __cpufreq_driver_target(policy, freq_next,
					 CPUFREQ_RELATION_L);
	}
out:
	return;
}

static void do_dbs_timer(struct work_struct *work)
{
	struct cpu_dbs_info_s *dbs_info =
		container_of(work, struct cpu_dbs_info_s, work.work);
	unsigned int cpu = dbs_info->cpu;

	/* We want all related CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);

	mutex_lock(&dbs_info->timer_mutex);
	dbs_check_cpu(dbs_info);
	queue_delayed_work_on(cpu, kondemand_wq, &dbs_info->work, delay);
	mutex_unlock(&dbs_info->timer_mutex);
}

static inline void dbs_timer_init(struct cpu_dbs_info_s *dbs_info)
{
	/* We want all related CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);
	delay -= jiffies % delay;

	INIT_DELAYED_WORK_DEFERRABLE(&dbs_info->work, do_dbs_timer);
	queue_delayed_work_on(dbs_info->cpu, kondemand_wq, &dbs_info->work,
		delay);
}

static inline void dbs_timer_exit(struct cpu_dbs_info_s *dbs_info)
{
	cancel_delayed_work_sync(&dbs_info->work);
}

static void ondemand_suspend(int suspend)
{
	if (!susp_enabled) return;
        if (!suspend) {
		suspended = 0;
		if (num_online_cpus() < 2) cpu_up(1);
		pr_info("[imoseyon] ondemand awake cpu1 up\n");
	} else {
		suspended = 1;
                if (num_online_cpus() > 1) cpu_down(1);
		pr_info("[imoseyon] ondemand suspended cpu1 down\n");
	}
}
static void ondemand_early_suspend(struct early_suspend *handler) {
     if (!registration) ondemand_suspend(1);
}

static void ondemand_late_resume(struct early_suspend *handler) {
     ondemand_suspend(0);
}

static struct early_suspend ondemand_power_suspend = {
        .suspend = ondemand_early_suspend,
        .resume = ondemand_late_resume,
        .level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 1,
};

static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
				   unsigned int event)
{
	unsigned int cpu = policy->cpu;
	struct cpu_dbs_info_s *this_dbs_info;
	unsigned int i, j, max_periods;
	int rc;

	this_dbs_info = &per_cpu(hp_cpu_dbs_info, cpu);

	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;

		mutex_lock(&dbs_mutex);
		dbs_enable++;
		for_each_cpu(j, policy->cpus) {
			struct cpu_dbs_info_s *j_dbs_info;
			j_dbs_info = &per_cpu(hp_cpu_dbs_info, j);
			j_dbs_info->cur_policy = policy;

			j_dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&j_dbs_info->prev_cpu_wall);
			if (dbs_tuners_ins.ignore_nice) {
				j_dbs_info->prev_cpu_nice =
						kstat_cpu(j).cpustat.nice;
			}

			max_periods = max(DEFAULT_ondemand_IN_SAMPLING_PERIODS,
					DEFAULT_ondemand_OUT_SAMPLING_PERIODS);
			dbs_tuners_ins.ondemand_load_history = kmalloc(
					(sizeof(unsigned int) * max_periods),
					GFP_KERNEL);
			if (!dbs_tuners_ins.ondemand_load_history) {
				WARN_ON(1);
				return -ENOMEM;
			}
			for (i = 0; i < max_periods; i++)
				dbs_tuners_ins.ondemand_load_history[i] = 50;
		}
		this_dbs_info->cpu = cpu;
		this_dbs_info->freq_table = cpufreq_frequency_get_table(cpu);
		/*
		 * Start the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 1) {
			rc = sysfs_create_group(cpufreq_global_kobject,
						&dbs_attr_group);
			if (rc) {
				mutex_unlock(&dbs_mutex);
				return rc;
			}
		}
		mutex_unlock(&dbs_mutex);

		mutex_init(&this_dbs_info->timer_mutex);
		dbs_timer_init(this_dbs_info);
		susp_enabled = 1;
		registration = 1;
                register_early_suspend(&ondemand_power_suspend);
		registration = 0;
                pr_info("[imoseyon] ondemand start\n");
		break;

	case CPUFREQ_GOV_STOP:
		dbs_timer_exit(this_dbs_info);

		mutex_lock(&dbs_mutex);
		mutex_destroy(&this_dbs_info->timer_mutex);
		dbs_enable--;
		mutex_unlock(&dbs_mutex);
		if (!dbs_enable)
			sysfs_remove_group(cpufreq_global_kobject,
					   &dbs_attr_group);
		kfree(dbs_tuners_ins.ondemand_load_history);
		/*
		 * XXX BIG CAVEAT: Stopping the governor with CPU1 offline
		 * will result in it remaining offline until the user onlines
		 * it again.  It is up to the user to do this (for now).
		 */
	        susp_enabled = 0;
                unregister_early_suspend(&ondemand_power_suspend);
                pr_info("[imoseyon] ondemand inactive\n");
		break;

	case CPUFREQ_GOV_LIMITS:
		mutex_lock(&this_dbs_info->timer_mutex);
		if (policy->max < this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(this_dbs_info->cur_policy,
				policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(this_dbs_info->cur_policy,
				policy->min, CPUFREQ_RELATION_L);
		mutex_unlock(&this_dbs_info->timer_mutex);
		break;
	}
	return 0;
}

static int __init cpufreq_gov_dbs_init(void)
{
	int err;
	cputime64_t wall;
	u64 idle_time;
	int cpu = get_cpu();

	idle_time = get_cpu_idle_time_us(cpu, &wall);
	put_cpu();
	if (idle_time != -1ULL) {
		dbs_tuners_ins.up_threshold = DEFAULT_UP_FREQ_MIN_LOAD;
	} else {
		pr_err("cpufreq-ondemand: %s: assumes CONFIG_NO_HZ\n",
				__func__);
		return -EINVAL;
	}

	kondemand_wq = create_workqueue("kondemand");
	if (!kondemand_wq) {
		pr_err("Creation of kondemand failed\n");
		return -EFAULT;
	}
	err = cpufreq_register_governor(&cpufreq_gov_ondemand);
	if (err)
		destroy_workqueue(kondemand_wq);

	return err;
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_ondemand);
	destroy_workqueue(kondemand_wq);
}

MODULE_AUTHOR("Mike Turquette <mturquette@ti.com>");
MODULE_DESCRIPTION("'cpufreq_ondemand' - cpufreq governor for dynamic frequency scaling and CPU ondemandging");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_ondemand
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
