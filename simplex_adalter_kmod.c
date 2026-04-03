/* simplex_adalter_kmod.c
 *
 * Build:
 *   make -C /lib/modules/$(uname -r)/build M=$PWD modules
 *
 * Load:
 *   sudo insmod simplex_adalter_kmod.ko
 *
 * Device:
 *   /dev/simplex_adalter
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/device.h>
#include <linux/slab.h>

#include "simplex_adalter_uapi.h"

#define SIMPLEX_PPM 1000000U
#define SIMPLEX_Q16 65536U

#define SIMPLEX_FLAG_SUPERCOMPUTE 0x1
#define SIMPLEX_FLAG_CHIP_RELAY   0x2

static unsigned int recursion_levels = 22;
module_param(recursion_levels, uint, 0644);
MODULE_PARM_DESC(recursion_levels, "Number of Fibonacci recursive levels");

static unsigned int nodes_per_level = 12;
module_param(nodes_per_level, uint, 0644);
MODULE_PARM_DESC(nodes_per_level, "Nodes per level");

static unsigned int min_retained_ppm = 10200;
module_param(min_retained_ppm, uint, 0644);
MODULE_PARM_DESC(min_retained_ppm, "Minimum retained ratio in ppm");

static unsigned int max_retained_ppm = 120000;
module_param(max_retained_ppm, uint, 0644);
MODULE_PARM_DESC(max_retained_ppm, "Maximum retained ratio in ppm");

static unsigned int arm_q16 = 77857;        /* 1.188 * 65536 */
module_param(arm_q16, uint, 0644);
MODULE_PARM_DESC(arm_q16, "ARM multiplier in q16");

static unsigned int adder_q16 = 27525;      /* 0.42 * 65536 */
module_param(adder_q16, uint, 0644);
MODULE_PARM_DESC(adder_q16, "ADDER multiplier in q16");

static unsigned int modifier_q16 = 67830;   /* 1.035 * 65536 */
module_param(modifier_q16, uint, 0644);
MODULE_PARM_DESC(modifier_q16, "Modifier multiplier in q16");

static unsigned int u_scalar_q16 = 114688;  /* 1.75 * 65536 */
module_param(u_scalar_q16, uint, 0644);
MODULE_PARM_DESC(u_scalar_q16, "u_scalar in q16");

static unsigned int knot_q16 = 32086;       /* ~0.4896 * 65536 */
module_param(knot_q16, uint, 0644);
MODULE_PARM_DESC(knot_q16, "knot scalar in q16");

static unsigned int level_gain_ppm = 144000;
module_param(level_gain_ppm, uint, 0644);
MODULE_PARM_DESC(level_gain_ppm, "Level gain in ppm");

static unsigned int node_gain_ppm = 21000;
module_param(node_gain_ppm, uint, 0644);
MODULE_PARM_DESC(node_gain_ppm, "Node gain in ppm");

static unsigned int phase_gain_ppm = 10200;
module_param(phase_gain_ppm, uint, 0644);
MODULE_PARM_DESC(phase_gain_ppm, "Phase gain in ppm");

static bool supercompute_switcher = true;
module_param(supercompute_switcher, bool, 0644);
MODULE_PARM_DESC(supercompute_switcher, "Enable supercompute switcher");

static bool chip_relay = true;
module_param(chip_relay, bool, 0644);
MODULE_PARM_DESC(chip_relay, "Enable chip relay");

struct simplex_adalter_state {
	struct mutex lock;
	struct workqueue_struct *wq;
	struct work_struct recalc_work;

	struct simplex_adalter_cfg cfg;
	struct simplex_adalter_base base;
	struct simplex_adalter_report report;

	struct miscdevice misc;
};

static struct simplex_adalter_state g_sa;

static inline u32 sa_clamp_u32(u32 v, u32 lo, u32 hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static inline u32 sa_mul_ppm(u32 a, u32 b)
{
	u64 v = (u64)a * (u64)b;
	v += SIMPLEX_PPM / 2;
	do_div(v, SIMPLEX_PPM);
	return (u32)v;
}

static inline u32 sa_mul_q16(u32 a, u32 q16)
{
	u64 v = (u64)a * (u64)q16;
	v += SIMPLEX_Q16 / 2;
	v >>= 16;
	return (u32)v;
}

static inline void sa_build_9_mips(u32 base_retained_ppm, u32 out[SIMPLEX_ADALTER_MIPS])
{
	static const u32 weights_ppm[SIMPLEX_ADALTER_MIPS] = {
		2400000, 1850000, 1400000, 1050000, 800000, 600000, 450000, 300000, 200000
	};
	u32 i;

	for (i = 0; i < SIMPLEX_ADALTER_MIPS; ++i) {
		u64 v = (u64)base_retained_ppm * (u64)weights_ppm[i];
		v += SIMPLEX_PPM / 2;
		do_div(v, SIMPLEX_PPM);
		out[i] = sa_clamp_u32((u32)v, 1000, SIMPLEX_PPM);
	}
}

static void sa_fill_fib(u32 count, u32 out[SIMPLEX_ADALTER_MAX_LEVELS], u64 *total)
{
	u32 i;

	*total = 0;
	if (!count)
		return;

	out[0] = 1;
	*total += 1;
	if (count == 1)
		return;

	out[1] = 1;
	*total += 1;
	for (i = 2; i < count; ++i) {
		out[i] = out[i - 1] + out[i - 2];
		*total += out[i];
	}
}

static void sa_recompute_locked(struct simplex_adalter_state *st)
{
	u32 fib[SIMPLEX_ADALTER_MAX_LEVELS] = {0};
	u64 fib_total = 0;
	u32 levels, nodes, i, n;

	u64 acc_ret = 0, acc_red = 0, acc_cross = 0, acc_cold = 0, acc_energy = 0;

	memset(&st->report, 0, sizeof(st->report));
	strscpy(st->report.key, st->base.key, sizeof(st->report.key));
	st->report.final_base = st->base.final_base;

	levels = st->cfg.recursion_levels;
	nodes = st->cfg.nodes_per_level;

	if (levels == 0)
		levels = 22;
	if (levels > SIMPLEX_ADALTER_MAX_LEVELS)
		levels = SIMPLEX_ADALTER_MAX_LEVELS;

	if (nodes == 0)
		nodes = 12;
	if (nodes > SIMPLEX_ADALTER_NODES_PER_LEVEL)
		nodes = SIMPLEX_ADALTER_NODES_PER_LEVEL;

	st->report.recursion_levels = levels;
	st->report.nodes_per_level = nodes;

	sa_fill_fib(levels, fib, &fib_total);
	if (!fib_total)
		fib_total = 1;

	for (i = 0; i < levels; ++i) {
		struct simplex_adalter_level *lvl = &st->report.levels[i];
		u64 level_ret = 0, level_red = 0, level_cross = 0, level_cold = 0, level_energy = 0;
		u32 fib_weight_ppm;
		u32 level_scalar_ppm;

		lvl->level_index = i + 1;
		lvl->fib_value = fib[i];

		fib_weight_ppm = (u32)(((u64)fib[i] * SIMPLEX_PPM) / fib_total);
		lvl->fib_weight_ppm = fib_weight_ppm;

		/* 1.0 - fib_weight * level_gain */
		level_scalar_ppm = SIMPLEX_PPM - sa_mul_ppm(fib_weight_ppm, st->cfg.level_gain_ppm);

		for (n = 0; n < nodes; ++n) {
			u32 slot = n % 4;
			u32 lane = n % 3;
			u32 node_scalar_ppm;
			u32 phase_scalar_ppm;
			u32 v;
			u32 add_ppm;
			u32 cross_ppm;
			u32 cold_ppm;
			u32 energy_ppm;

			/* base -> ARM -> u_scalar */
			v = st->base.retained_ppm;
			v = sa_mul_q16(v, st->cfg.arm_q16);
			v = sa_mul_q16(v, st->cfg.u_scalar_q16);
			v = sa_clamp_u32(v, st->cfg.min_retained_ppm, st->cfg.max_retained_ppm);

			/* ADDER over 0.0102 with slot spread */
			add_ppm = 10200 + (slot * 1275);
			add_ppm = sa_mul_q16(add_ppm, st->cfg.adder_q16);
			v = sa_clamp_u32(v + add_ppm, st->cfg.min_retained_ppm, st->cfg.max_retained_ppm);

			/* Modifier */
			v = sa_mul_q16(v, st->cfg.modifier_q16);

			/* Fibonacci / node / phase shaping */
			if (nodes > 1)
				node_scalar_ppm = SIMPLEX_PPM - ((n * st->cfg.node_gain_ppm) / (nodes - 1));
			else
				node_scalar_ppm = SIMPLEX_PPM;

			phase_scalar_ppm = SIMPLEX_PPM -
				(((i + 1) * st->cfg.phase_gain_ppm) / 100) -
				((n * st->cfg.phase_gain_ppm) / 200);

			v = sa_mul_ppm(v, level_scalar_ppm);
			v = sa_mul_ppm(v, node_scalar_ppm);
			v = sa_mul_ppm(v, phase_scalar_ppm);

			if (st->cfg.flags & SIMPLEX_FLAG_SUPERCOMPUTE)
				v = sa_mul_ppm(v, 940000);
			if (st->cfg.flags & SIMPLEX_FLAG_CHIP_RELAY)
				v = sa_mul_ppm(v, 980000);

			v = sa_clamp_u32(v, st->cfg.min_retained_ppm, st->cfg.max_retained_ppm);

			cross_ppm = st->base.crosshatch_ppm;
			cross_ppm = sa_mul_ppm(cross_ppm, SIMPLEX_PPM + ((fib_weight_ppm * 120000) / SIMPLEX_PPM));
			cross_ppm = sa_mul_ppm(cross_ppm, SIMPLEX_PPM - (slot * 15000));
			cross_ppm = sa_clamp_u32(cross_ppm, 0, SIMPLEX_PPM);

			cold_ppm = st->base.coldness_ppm;
			cold_ppm = sa_mul_ppm(cold_ppm, SIMPLEX_PPM + ((fib_weight_ppm * 180000) / SIMPLEX_PPM));
			cold_ppm = sa_mul_ppm(cold_ppm, SIMPLEX_PPM + (lane * 10000));
			cold_ppm = sa_clamp_u32(cold_ppm, 0, SIMPLEX_PPM);

			energy_ppm = sa_mul_q16(v, st->cfg.knot_q16);

			level_ret += v;
			level_red += (SIMPLEX_PPM - v);
			level_cross += cross_ppm;
			level_cold += cold_ppm;
			level_energy += energy_ppm;
		}

		lvl->retained_ppm = (u32)(level_ret / nodes);
		lvl->reduction_ppm = (u32)(level_red / nodes);
		lvl->crosshatch_ppm = (u32)(level_cross / nodes);
		lvl->coldness_ppm = (u32)(level_cold / nodes);
		lvl->energy_ppm = (u32)(level_energy / nodes);
		sa_build_9_mips(lvl->retained_ppm, lvl->mip_retained_ppm);

		acc_ret += lvl->retained_ppm;
		acc_red += lvl->reduction_ppm;
		acc_cross += lvl->crosshatch_ppm;
		acc_cold += lvl->coldness_ppm;
		acc_energy += lvl->energy_ppm;
	}

	st->report.average_retained_ppm = (u32)(acc_ret / levels);
	st->report.average_reduction_ppm = (u32)(acc_red / levels);
	st->report.average_crosshatch_ppm = (u32)(acc_cross / levels);
	st->report.average_coldness_ppm = (u32)(acc_cold / levels);
	st->report.recursive_energy_ppm = (u32)(acc_energy / levels);
}

static void sa_recalc_workfn(struct work_struct *work)
{
	struct simplex_adalter_state *st = container_of(work, struct simplex_adalter_state, recalc_work);

	mutex_lock(&st->lock);
	sa_recompute_locked(st);
	mutex_unlock(&st->lock);
}

static ssize_t last_key_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t n;
	mutex_lock(&g_sa.lock);
	n = scnprintf(buf, PAGE_SIZE, "%s\n", g_sa.base.key);
	mutex_unlock(&g_sa.lock);
	return n;
}

static ssize_t last_key_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	size_t n = min_t(size_t, count, SIMPLEX_ADALTER_KEY_LEN - 1);

	mutex_lock(&g_sa.lock);
	memset(g_sa.base.key, 0, sizeof(g_sa.base.key));
	memcpy(g_sa.base.key, buf, n);
	if (n && g_sa.base.key[n - 1] == '\n')
		g_sa.base.key[n - 1] = '\0';
	mutex_unlock(&g_sa.lock);

	queue_work(g_sa.wq, &g_sa.recalc_work);
	flush_work(&g_sa.recalc_work);
	return count;
}

static ssize_t summary_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t n;
	mutex_lock(&g_sa.lock);
	n = scnprintf(buf, PAGE_SIZE,
		      "key=%s levels=%u nodes=%u retained_ppm=%u reduction_ppm=%u crosshatch_ppm=%u coldness_ppm=%u energy_ppm=%u\n",
		      g_sa.report.key,
		      g_sa.report.recursion_levels,
		      g_sa.report.nodes_per_level,
		      g_sa.report.average_retained_ppm,
		      g_sa.report.average_reduction_ppm,
		      g_sa.report.average_crosshatch_ppm,
		      g_sa.report.average_coldness_ppm,
		      g_sa.report.recursive_energy_ppm);
	mutex_unlock(&g_sa.lock);
	return n;
}

static ssize_t level0_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct simplex_adalter_level lvl;
	ssize_t n;

	mutex_lock(&g_sa.lock);
	lvl = g_sa.report.levels[0];
	mutex_unlock(&g_sa.lock);

	n = scnprintf(buf, PAGE_SIZE,
		      "fib=%u weight_ppm=%u retained_ppm=%u reduction_ppm=%u crosshatch_ppm=%u coldness_ppm=%u energy_ppm=%u\n",
		      lvl.fib_value, lvl.fib_weight_ppm, lvl.retained_ppm, lvl.reduction_ppm,
		      lvl.crosshatch_ppm, lvl.coldness_ppm, lvl.energy_ppm);
	return n;
}

static DEVICE_ATTR_RW(last_key);
static DEVICE_ATTR_RO(summary);
static DEVICE_ATTR_RO(level0);

static struct attribute *simplex_attrs[] = {
	&dev_attr_last_key.attr,
	&dev_attr_summary.attr,
	&dev_attr_level0.attr,
	NULL,
};

static const struct attribute_group simplex_group = {
	.attrs = simplex_attrs,
};

static const struct attribute_group *simplex_groups[] = {
	&simplex_group,
	NULL,
};

static long simplex_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct simplex_adalter_cfg cfg;
	struct simplex_adalter_base base;
	struct simplex_adalter_report report;
	int ret = 0;

	switch (cmd) {
	case SIMPLEX_ADALTER_IOC_SET_CFG:
		if (copy_from_user(&cfg, (void __user *)arg, sizeof(cfg)))
			return -EFAULT;

		if (cfg.version != 1)
			return -EINVAL;
		if (!cfg.recursion_levels || cfg.recursion_levels > SIMPLEX_ADALTER_MAX_LEVELS)
			return -EINVAL;
		if (!cfg.nodes_per_level || cfg.nodes_per_level > SIMPLEX_ADALTER_NODES_PER_LEVEL)
			return -EINVAL;

		mutex_lock(&g_sa.lock);
		g_sa.cfg = cfg;
		mutex_unlock(&g_sa.lock);

		queue_work(g_sa.wq, &g_sa.recalc_work);
		flush_work(&g_sa.recalc_work);
		return 0;

	case SIMPLEX_ADALTER_IOC_GET_CFG:
		mutex_lock(&g_sa.lock);
		cfg = g_sa.cfg;
		mutex_unlock(&g_sa.lock);

		if (copy_to_user((void __user *)arg, &cfg, sizeof(cfg)))
			return -EFAULT;
		return 0;

	case SIMPLEX_ADALTER_IOC_RUN_BASE:
		if (copy_from_user(&base, (void __user *)arg, sizeof(base)))
			return -EFAULT;

		mutex_lock(&g_sa.lock);
		g_sa.base = base;
		g_sa.base.key[SIMPLEX_ADALTER_KEY_LEN - 1] = '\0';
		mutex_unlock(&g_sa.lock);

		queue_work(g_sa.wq, &g_sa.recalc_work);
		flush_work(&g_sa.recalc_work);
		return 0;

	case SIMPLEX_ADALTER_IOC_GET_REPORT:
		mutex_lock(&g_sa.lock);
		report = g_sa.report;
		mutex_unlock(&g_sa.lock);

		if (copy_to_user((void __user *)arg, &report, sizeof(report)))
			return -EFAULT;
		return 0;

	default:
		ret = -ENOTTY;
	}

	return ret;
}

static const struct file_operations simplex_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = simplex_ioctl,
	.compat_ioctl   = simplex_ioctl,
	.llseek         = no_llseek,
};

static int __init simplex_init(void)
{
	int ret;

	memset(&g_sa, 0, sizeof(g_sa));
	mutex_init(&g_sa.lock);

	g_sa.cfg.version = 1;
	g_sa.cfg.recursion_levels = recursion_levels;
	g_sa.cfg.nodes_per_level = nodes_per_level;
	g_sa.cfg.min_retained_ppm = min_retained_ppm;
	g_sa.cfg.max_retained_ppm = max_retained_ppm;
	g_sa.cfg.level_gain_ppm = level_gain_ppm;
	g_sa.cfg.node_gain_ppm = node_gain_ppm;
	g_sa.cfg.phase_gain_ppm = phase_gain_ppm;
	g_sa.cfg.arm_q16 = arm_q16;
	g_sa.cfg.adder_q16 = adder_q16;
	g_sa.cfg.modifier_q16 = modifier_q16;
	g_sa.cfg.u_scalar_q16 = u_scalar_q16;
	g_sa.cfg.knot_q16 = knot_q16;
	g_sa.cfg.flags = 0;
	if (supercompute_switcher)
		g_sa.cfg.flags |= SIMPLEX_FLAG_SUPERCOMPUTE;
	if (chip_relay)
		g_sa.cfg.flags |= SIMPLEX_FLAG_CHIP_RELAY;

	strscpy(g_sa.base.key, "DAWNSTAR:page-0001", sizeof(g_sa.base.key));
	g_sa.base.final_base = 0x0F210144u;
	g_sa.base.retained_ppm = 10200;
	g_sa.base.crosshatch_ppm = 812500;
	g_sa.base.coldness_ppm = 904400;

	INIT_WORK(&g_sa.recalc_work, sa_recalc_workfn);

	g_sa.wq = alloc_workqueue("simplex_adalter_wq", WQ_UNBOUND | WQ_HIGHPRI, 1);
	if (!g_sa.wq)
		return -ENOMEM;

	g_sa.misc.minor = MISC_DYNAMIC_MINOR;
	g_sa.misc.name = "simplex_adalter";
	g_sa.misc.fops = &simplex_fops;
	g_sa.misc.mode = 0660;
	g_sa.misc.groups = simplex_groups;

	ret = misc_register(&g_sa.misc);
	if (ret) {
		destroy_workqueue(g_sa.wq);
		return ret;
	}

	queue_work(g_sa.wq, &g_sa.recalc_work);
	flush_work(&g_sa.recalc_work);

	pr_info("simplex_adalter: loaded /dev/simplex_adalter levels=%u nodes=%u\n",
		g_sa.cfg.recursion_levels, g_sa.cfg.nodes_per_level);
	return 0;
}

static void __exit simplex_exit(void)
{
	flush_workqueue(g_sa.wq);
	destroy_workqueue(g_sa.wq);
	misc_deregister(&g_sa.misc);
	pr_info("simplex_adalter: unloaded\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("Simplex Adalter kernel API bridge with Fibonacci recursion");
MODULE_VERSION("0.1.0");

module_init(simplex_init);
module_exit(simplex_exit);
