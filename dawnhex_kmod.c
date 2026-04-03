// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include "dawnhex_duo_uapi.h"
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/device.h>

#include "dawnhex_uapi.h"

#define DAWNHEX_EVENT_Q 128
#define DAWNHEX_EVENT_LEN 160
#define DAWNHEX_DEFAULT_TICK_MS 500

#define DH_PPM 1000000U
#define DH_Q16 65536U

/* Simplex constants */
#define DH_SIMPLEX_MIN_RETAINED_PPM   10200U
#define DH_SIMPLEX_MAX_RETAINED_PPM  120000U

#define DH_SIMPLEX_LEVEL_GAIN_PPM    144000U
#define DH_SIMPLEX_NODE_GAIN_PPM      21000U
#define DH_SIMPLEX_PHASE_GAIN_PPM     10200U

#define DH_SIMPLEX_ARM_Q16            77857U   /* 1.188 */
#define DH_SIMPLEX_ADDER_Q16          27525U   /* 0.42  */
#define DH_SIMPLEX_MODIFIER_Q16       67830U   /* 1.035 */
#define DH_SIMPLEX_U_SCALAR_Q16      114688U   /* 1.75  */
#define DH_SIMPLEX_KNOT_Q16           32086U   /* ~0.4896 */

#define DH_SIMPLEX_FLAG_SUPERCOMPUTE  0x1
#define DH_SIMPLEX_FLAG_CHIP_RELAY    0x2

static u32 dh_duo_base_retained_ppm_locked(const struct dawnhex_duo_pair *p)
{
	u32 base;

	/*
	 * pair energy high  -> more aggressive compression
	 * pair pressure high -> preserve more
	 */
	base = DH_SIMPLEX_MIN_RETAINED_PPM;
	base += (1000000U - p->energy_ppm) / 20U;  /* up to ~50k */
	base += p->pressure_ppm / 25U;             /* up to ~40k */

	switch (p->mode) {
	case DAWNHEX_DUO_INOUT_INOUT_OUT:
		base = dh_mul_ppm(base, 980000U);
		break;
	case DAWNHEX_DUO_INOUT_INOUT_IN:
		base = dh_mul_ppm(base, 1020000U);
		break;
	case DAWNHEX_DUO_CROSSED:
		base = dh_mul_ppm(base, 960000U);
		break;
	default:
		break;
	}

	return dh_clamp_u32(base, DH_SIMPLEX_MIN_RETAINED_PPM, DH_SIMPLEX_MAX_RETAINED_PPM);
}

static void dh_duo_recompute_simplex_locked(struct dawnhex_duo_pair *p)
{
	u32 fib[DAWNHEX_SIMPLEX_LEVELS];
	u64 fib_total = 0;
	u64 acc_ret = 0, acc_red = 0, acc_cross = 0, acc_cold = 0, acc_energy = 0;
	u32 i, n;

	memset(&p->simplex, 0, sizeof(p->simplex));

	p->simplex.level_count = DAWNHEX_SIMPLEX_LEVELS;
	p->simplex.node_count = DAWNHEX_SIMPLEX_NODES_PER_LEVEL;
	p->simplex.flags = DH_SIMPLEX_FLAG_SUPERCOMPUTE | DH_SIMPLEX_FLAG_CHIP_RELAY;
	p->simplex.base_retained_ppm = dh_duo_base_retained_ppm_locked(p);

	dh_fill_fib22(fib, &fib_total);
	if (!fib_total)
		fib_total = 1;

	for (i = 0; i < DAWNHEX_SIMPLEX_LEVELS; ++i) {
		struct dawnhex_duo_simplex_level *lvl = &p->simplex.levels[i];
		u32 fib_weight_ppm;
		u32 level_scalar_ppm;
		u64 level_ret = 0, level_red = 0, level_cross = 0, level_cold = 0, level_energy = 0;

		lvl->level_index = i + 1;
		lvl->fib_value = fib[i];
		lvl->node_count = DAWNHEX_SIMPLEX_NODES_PER_LEVEL;

		fib_weight_ppm = (u32)(((u64)fib[i] * DH_PPM) / fib_total);
		lvl->fib_weight_ppm = fib_weight_ppm;

		level_scalar_ppm = DH_PPM - dh_mul_ppm(fib_weight_ppm, DH_SIMPLEX_LEVEL_GAIN_PPM);

		for (n = 0; n < DAWNHEX_SIMPLEX_NODES_PER_LEVEL; ++n) {
			u32 lane = n % 3;
			u32 slot = n % 4;
			u32 node_scalar_ppm;
			u32 phase_scalar_ppm;
			u32 v;
			u32 add_ppm;
			u32 cross_ppm;
			u32 cold_ppm;
			u32 energy_ppm;

			v = p->simplex.base_retained_ppm;
			v = dh_mul_q16(v, DH_SIMPLEX_ARM_Q16);
			v = dh_mul_q16(v, DH_SIMPLEX_U_SCALAR_Q16);
			v = dh_clamp_u32(v, DH_SIMPLEX_MIN_RETAINED_PPM, DH_SIMPLEX_MAX_RETAINED_PPM);

			add_ppm = 10200U + (slot * 1275U);
			add_ppm = dh_mul_q16(add_ppm, DH_SIMPLEX_ADDER_Q16);

			v = dh_clamp_u32(v + add_ppm, DH_SIMPLEX_MIN_RETAINED_PPM, DH_SIMPLEX_MAX_RETAINED_PPM);
			v = dh_mul_q16(v, DH_SIMPLEX_MODIFIER_Q16);

			if (DAWNHEX_SIMPLEX_NODES_PER_LEVEL > 1)
				node_scalar_ppm = DH_PPM - ((n * DH_SIMPLEX_NODE_GAIN_PPM) / (DAWNHEX_SIMPLEX_NODES_PER_LEVEL - 1));
			else
				node_scalar_ppm = DH_PPM;

			phase_scalar_ppm =
				DH_PPM -
				(((i + 1) * DH_SIMPLEX_PHASE_GAIN_PPM) / 100U) -
				((n * DH_SIMPLEX_PHASE_GAIN_PPM) / 200U);

			v = dh_mul_ppm(v, level_scalar_ppm);
			v = dh_mul_ppm(v, node_scalar_ppm);
			v = dh_mul_ppm(v, phase_scalar_ppm);

			/* Duo refraction and quotient bias */
			v = dh_mul_ppm(v, dh_clamp_u32(p->refraction_ppm, 900000U, 1200000U));
			v = dh_mul_ppm(v, dh_clamp_u32(p->quotient_ppm, 800000U, 1250000U));

			/* crossed mode tightens retention a bit */
			if (p->mode == DAWNHEX_DUO_CROSSED)
				v = dh_mul_ppm(v, 970000U);

			if (p->simplex.flags & DH_SIMPLEX_FLAG_SUPERCOMPUTE)
				v = dh_mul_ppm(v, 940000U);
			if (p->simplex.flags & DH_SIMPLEX_FLAG_CHIP_RELAY)
				v = dh_mul_ppm(v, 980000U);

			v = dh_clamp_u32(v, DH_SIMPLEX_MIN_RETAINED_PPM, DH_SIMPLEX_MAX_RETAINED_PPM);

			cross_ppm = p->quotient_ppm / 3U;
			cross_ppm += dh_clamp_u32(p->refraction_ppm, 0U, 1500000U) / 4U;
			cross_ppm += p->energy_ppm / 5U;
			cross_ppm += p->pressure_ppm / 5U;
			cross_ppm = dh_mul_ppm(cross_ppm, DH_PPM - (slot * 15000U));
			cross_ppm = dh_clamp_u32(cross_ppm, 0U, DH_PPM);

			cold_ppm = ((DH_PPM - p->energy_ppm) / 2U) + (p->pressure_ppm / 2U);
			cold_ppm = dh_mul_ppm(cold_ppm, DH_PPM + (fib_weight_ppm / 8U));
			cold_ppm = dh_mul_ppm(cold_ppm, DH_PPM + (lane * 10000U));
			cold_ppm = dh_clamp_u32(cold_ppm, 0U, DH_PPM);

			energy_ppm = dh_mul_q16(v, DH_SIMPLEX_KNOT_Q16);

			level_ret += v;
			level_red += (DH_PPM - v);
			level_cross += cross_ppm;
			level_cold += cold_ppm;
			level_energy += energy_ppm;
		}

		lvl->retained_ppm = (u32)(level_ret / DAWNHEX_SIMPLEX_NODES_PER_LEVEL);
		lvl->reduction_ppm = (u32)(level_red / DAWNHEX_SIMPLEX_NODES_PER_LEVEL);
		lvl->crosshatch_ppm = (u32)(level_cross / DAWNHEX_SIMPLEX_NODES_PER_LEVEL);
		lvl->coldness_ppm = (u32)(level_cold / DAWNHEX_SIMPLEX_NODES_PER_LEVEL);
		lvl->energy_ppm = (u32)(level_energy / DAWNHEX_SIMPLEX_NODES_PER_LEVEL);
		lvl->pressure_ppm = p->pressure_ppm;

		dh_build_9_mips(lvl->retained_ppm, lvl->mip_retained_ppm);

		acc_ret += lvl->retained_ppm;
		acc_red += lvl->reduction_ppm;
		acc_cross += lvl->crosshatch_ppm;
		acc_cold += lvl->coldness_ppm;
		acc_energy += lvl->energy_ppm;
	}

	p->simplex.recursive_retained_ppm = (u32)(acc_ret / DAWNHEX_SIMPLEX_LEVELS);
	p->simplex.recursive_reduction_ppm = (u32)(acc_red / DAWNHEX_SIMPLEX_LEVELS);
	p->simplex.recursive_crosshatch_ppm = (u32)(acc_cross / DAWNHEX_SIMPLEX_LEVELS);
	p->simplex.recursive_coldness_ppm = (u32)(acc_cold / DAWNHEX_SIMPLEX_LEVELS);
	p->simplex.recursive_energy_ppm = (u32)(acc_energy / DAWNHEX_SIMPLEX_LEVELS);
}

static inline u32 dh_mul_ppm(u32 a, u32 b)
{
	u64 v = (u64)a * (u64)b;
	v += DH_PPM / 2;
	v /= DH_PPM;
	return (u32)v;
}

static inline u32 dh_mul_q16(u32 a, u32 q16)
{
	u64 v = (u64)a * (u64)q16;
	v += DH_Q16 / 2;
	v >>= 16;
	return (u32)v;
}

static inline void dh_build_9_mips(u32 base_retained_ppm, u32 out[DAWNHEX_SIMPLEX_MIPS])
{
	static const u32 weights_ppm[DAWNHEX_SIMPLEX_MIPS] = {
		2400000U, 1850000U, 1400000U, 1050000U,
		 800000U,  600000U,  450000U,  300000U, 200000U
	};
	u32 i;

	for (i = 0; i < DAWNHEX_SIMPLEX_MIPS; ++i) {
		u64 v = (u64)base_retained_ppm * (u64)weights_ppm[i];
		v += DH_PPM / 2;
		v /= DH_PPM;
		out[i] = dh_clamp_u32((u32)v, 1000U, DH_PPM);
	}
}

static void dh_fill_fib22(u32 out[DAWNHEX_SIMPLEX_LEVELS], u64 *total)
{
	u32 i;

	memset(out, 0, sizeof(u32) * DAWNHEX_SIMPLEX_LEVELS);
	*total = 0;

	out[0] = 1;
	out[1] = 1;
	*total = 2;

	for (i = 2; i < DAWNHEX_SIMPLEX_LEVELS; ++i) {
		out[i] = out[i - 1] + out[i - 2];
		*total += out[i];
	}
}


int dawnhex_duo_hook_register(struct dawnhex_duo_hook *hook)
{
	if (!hook || !hook->fn || !hook->name)
		return -EINVAL;

	mutex_lock(&g_dawnhex.lock);
	list_add_tail(&hook->node, &g_dawnhex.duo.hooks);
	dh_emit_event_locked("duo_hook_register name=%s", hook->name);
	mutex_unlock(&g_dawnhex.lock);
	return 0;
}
EXPORT_SYMBOL_GPL(dawnhex_duo_hook_register);

void dawnhex_duo_hook_unregister(struct dawnhex_duo_hook *hook)
{
	if (!hook)
		return;

	mutex_lock(&g_dawnhex.lock);
	list_del_init(&hook->node);
	dh_emit_event_locked("duo_hook_unregister name=%s", hook->name ? hook->name : "unknown");
	mutex_unlock(&g_dawnhex.lock);
}
EXPORT_SYMBOL_GPL(dawnhex_duo_hook_unregister);

int dawnhex_duo_pulse_kernel(u32 pair_id, u32 value_ppm)
{
	int ret;

	mutex_lock(&g_dawnhex.lock);
	ret = dh_duo_pulse_locked(pair_id, value_ppm);
	mutex_unlock(&g_dawnhex.lock);

	return ret;
}
EXPORT_SYMBOL_GPL(dawnhex_duo_pulse_kernel);


struct dawnhex_duo_simplex_level {
	u32 level_index;
	u32 fib_value;
	u32 fib_weight_ppm;

	u32 node_count;

	u32 retained_ppm;
	u32 reduction_ppm;
	u32 crosshatch_ppm;
	u32 coldness_ppm;
	u32 energy_ppm;
	u32 pressure_ppm;

	u32 mip_retained_ppm[DAWNHEX_SIMPLEX_MIPS];
};

struct dawnhex_duo_simplex_state {
	u32 level_count;
	u32 node_count;

	u32 base_retained_ppm;
	u32 recursive_retained_ppm;
	u32 recursive_reduction_ppm;
	u32 recursive_crosshatch_ppm;
	u32 recursive_coldness_ppm;
	u32 recursive_energy_ppm;

	u32 flags;
	struct dawnhex_duo_simplex_level levels[DAWNHEX_SIMPLEX_LEVELS];
};

struct dawnhex_duo_pair {
	u32 pair_id;
	u32 a;
	u32 b;
	u32 mode;
	bool active;

	u32 quotient_ppm;
	u32 refraction_ppm;
	u32 energy_ppm;
	u32 pressure_ppm;

	u32 pulses;
	u32 last_value_ppm;

	struct dawnhex_duo_simplex_state simplex;
};

struct dawnhex_duo_hook_ctx {
	u32 pair_id;
	u32 a;
	u32 b;
	u32 mode;
	u32 value_ppm;
	u32 quotient_ppm;
	u32 refraction_ppm;
	u32 energy_ppm;
	u32 pressure_ppm;

	/* new Simplex/Fibonacci summary */
	u32 simplex_base_retained_ppm;
	u32 simplex_recursive_retained_ppm;
	u32 simplex_recursive_reduction_ppm;
	u32 simplex_recursive_energy_ppm;
};
struct dawnhex_duo_pair {
	u32 pair_id;
	u32 a;
	u32 b;
	u32 mode;
	bool active;

	u32 quotient_ppm;
	u32 refraction_ppm;
	u32 energy_ppm;
	u32 pressure_ppm;

	u32 pulses;
	u32 last_value_ppm;
};

struct dawnhex_duo_hook_ctx {
	u32 pair_id;
	u32 a;
	u32 b;
	u32 mode;
	u32 value_ppm;
	u32 quotient_ppm;
	u32 refraction_ppm;
	u32 energy_ppm;
	u32 pressure_ppm;
};

typedef int (*dawnhex_duo_hook_fn)(const struct dawnhex_duo_hook_ctx *ctx, void *priv);

struct dawnhex_duo_hook {
	struct list_head node;
	const char *name;
	dawnhex_duo_hook_fn fn;
	void *priv;
};

struct dawnhex_duo_state {
	struct dawnhex_duo_pair pairs[DAWNHEX_DUO_MAX_PAIRS];
	u32 pair_count;
	u32 next_pair_id;
	struct list_head hooks;
};

static struct dawnhex_duo_pair *dh_duo_lookup_pair_locked(u32 pair_id)
{
	u32 i;

	for (i = 0; i < g_dawnhex.duo.pair_count; ++i) {
		if (g_dawnhex.duo.pairs[i].pair_id == pair_id)
			return &g_dawnhex.duo.pairs[i];
	}
	return NULL;
}

static u32 dh_duo_default_quotient_ppm(u32 mode)
{
	switch (mode) {
	case DAWNHEX_DUO_INOUT_INOUT_OUT:
		return 1000000;
	case DAWNHEX_DUO_INOUT_INOUT_IN:
		return 1000000;
	case DAWNHEX_DUO_CROSSED:
		return 1250000;
	default:
		return 0;
	}
}

static u32 dh_duo_default_refraction_ppm(u32 mode)
{
	switch (mode) {
	case DAWNHEX_DUO_INOUT_INOUT_OUT:
		return 1010200; /* 1.0102 */
	case DAWNHEX_DUO_INOUT_INOUT_IN:
		return 1020300; /* 1.0203 */
	case DAWNHEX_DUO_CROSSED:
		return 1044400; /* 1.0444 */
	default:
		return 1000000;
	}
}

static void dh_duo_emit_hooks_locked(const struct dawnhex_duo_hook_ctx *ctx)
{
	struct dawnhex_duo_hook *hook;

	list_for_each_entry(hook, &g_dawnhex.duo.hooks, node) {
		if (hook->fn)
			hook->fn(ctx, hook->priv);
	}
}

static int dh_duo_add_pair_locked(u32 a, u32 b, u32 mode, u32 *out_pair_id)
{
	struct dawnhex_duo_pair *p;
	struct dawnhex_node *na, *nb;

	if (g_dawnhex.duo.pair_count >= DAWNHEX_DUO_MAX_PAIRS)
		return -ENOSPC;

	if (a == b)
		return -EINVAL;

	na = dh_lookup_node_locked(a);
	nb = dh_lookup_node_locked(b);
	if (!na || !nb)
		return -ENOENT;

	p = &g_dawnhex.duo.pairs[g_dawnhex.duo.pair_count++];
	memset(p, 0, sizeof(*p));

	p->pair_id = ++g_dawnhex.duo.next_pair_id;
	p->a = a;
	p->b = b;
	p->mode = mode;
	p->active = true;
	p->quotient_ppm = dh_duo_default_quotient_ppm(mode);
	p->refraction_ppm = dh_duo_default_refraction_ppm(mode);
	p->energy_ppm = 500000;
	p->pressure_ppm = 500000;

	dh_duo_recompute_simplex_locked(p);

	if (out_pair_id)
		*out_pair_id = p->pair_id;

	dh_emit_event_locked("duo_add pair=%u a=%u b=%u mode=%u",
			     p->pair_id, a, b, mode);
	return 0;
}

static int dh_duo_del_pair_locked(u32 pair_id)
{
	u32 i;

	for (i = 0; i < g_dawnhex.duo.pair_count; ++i) {
		if (g_dawnhex.duo.pairs[i].pair_id == pair_id) {
			memmove(&g_dawnhex.duo.pairs[i],
				&g_dawnhex.duo.pairs[i + 1],
				(g_dawnhex.duo.pair_count - i - 1) *
				sizeof(struct dawnhex_duo_pair));
			g_dawnhex.duo.pair_count--;
			dh_emit_event_locked("duo_del pair=%u", pair_id);
			return 0;
		}
	}
	return -ENOENT;
}

static int dh_duo_pulse_locked(u32 pair_id, u32 value_ppm)
{
	struct dawnhex_duo_pair *p;
	struct dawnhex_node *na, *nb;
	struct dawnhex_duo_hook_ctx ctx;
	u32 base_energy;
	u32 base_pressure;

	p = dh_duo_lookup_pair_locked(pair_id);
	if (!p || !p->active)
		return -ENOENT;

	na = dh_lookup_node_locked(p->a);
	nb = dh_lookup_node_locked(p->b);
	if (!na || !nb)
		return -ENOENT;

	value_ppm = dh_clamp_u32(value_ppm, 0, 1000000);

	base_energy = (na->energy_ppm + nb->energy_ppm) / 2;
	base_pressure = (na->health_ppm + nb->health_ppm) / 2;

	p->last_value_ppm = value_ppm;
	p->pulses++;

	/* duo crossed transform */
	p->energy_ppm = dh_clamp_u32((base_energy + value_ppm) / 2, 0, 1000000);
	p->pressure_ppm = dh_clamp_u32((base_pressure + (1000000 - value_ppm)) / 2, 0, 1000000);

	/* feed back into nodes */
	na->energy_ppm = dh_clamp_u32((na->energy_ppm + p->energy_ppm) / 2, 0, 1000000);
	nb->energy_ppm = dh_clamp_u32((nb->energy_ppm + p->energy_ppm) / 2, 0, 1000000);

    dh_duo_recompute_simplex_locked(p);
	
	memset(&ctx, 0, sizeof(ctx));
	ctx.pair_id = p->pair_id;
	ctx.a = p->a;
	ctx.b = p->b;
	ctx.mode = p->mode;
	ctx.value_ppm = p->last_value_ppm;
	ctx.quotient_ppm = p->quotient_ppm;
	ctx.refraction_ppm = p->refraction_ppm;
	ctx.energy_ppm = p->energy_ppm;
	ctx.pressure_ppm = p->pressure_ppm;
	ctx.simplex_base_retained_ppm = p->simplex.base_retained_ppm;
	ctx.simplex_recursive_retained_ppm = p->simplex.recursive_retained_ppm;
	ctx.simplex_recursive_reduction_ppm = p->simplex.recursive_reduction_ppm;
	ctx.simplex_recursive_energy_ppm = p->simplex.recursive_energy_ppm;

	dh_duo_emit_hooks_locked(&ctx);

	dh_emit_event_locked("duo_pulse pair=%u value=%u energy=%u pressure=%u",
			     p->pair_id, p->last_value_ppm,
			     p->energy_ppm, p->pressure_ppm);
	return 0;
}

struct dawnhex_node {
	u32 id;
	char name[DAWNHEX_NAME_LEN];
	bool present;
	bool online;
	u32 energy_ppm;
	u32 health_ppm;
	u64 links_mask;
};

struct dawnhex_state {
	struct mutex lock;

	struct miscdevice misc;

	wait_queue_head_t event_wq;
	char events[DAWNHEX_EVENT_Q][DAWNHEX_EVENT_LEN];
	u32 ev_head;
	u32 ev_tail;

	struct delayed_work tick_work;
	u32 tick_ms;
	bool interactive;
	u32 generation;

	struct dawnhex_node nodes[DAWNHEX_MAX_NODES];
};

static struct dawnhex_state g_dawnhex;

static inline u32 dh_clamp_u32(u32 v, u32 lo, u32 hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static void dh_emit_event_locked(const char *fmt, ...)
{
	va_list ap;
	char tmp[DAWNHEX_EVENT_LEN];
	u32 next;

	va_start(ap, fmt);
	vscnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);

	next = (g_dawnhex.ev_head + 1) % DAWNHEX_EVENT_Q;
	if (next == g_dawnhex.ev_tail)
		g_dawnhex.ev_tail = (g_dawnhex.ev_tail + 1) % DAWNHEX_EVENT_Q;

	strscpy(g_dawnhex.events[g_dawnhex.ev_head], tmp, DAWNHEX_EVENT_LEN);
	g_dawnhex.ev_head = next;

	wake_up_interruptible(&g_dawnhex.event_wq);
}

static struct dawnhex_node *dh_lookup_node_locked(u32 id)
{
	if (id >= DAWNHEX_MAX_NODES)
		return NULL;
	if (!g_dawnhex.nodes[id].present)
		return NULL;
	return &g_dawnhex.nodes[id];
}

static u32 dh_count_nodes_locked(void)
{
	u32 i, count = 0;
	for (i = 0; i < DAWNHEX_MAX_NODES; ++i)
		if (g_dawnhex.nodes[i].present)
			count++;
	return count;
}

static u32 dh_count_links_locked(void)
{
	u32 i, links = 0;

	for (i = 0; i < DAWNHEX_MAX_NODES; ++i) {
		if (!g_dawnhex.nodes[i].present)
			continue;
		links += hweight64(g_dawnhex.nodes[i].links_mask);
	}

	return links / 2;
}

static int dh_add_node_locked(u32 id, const char *name)
{
	struct dawnhex_node *n;

	if (id >= DAWNHEX_MAX_NODES)
		return -EINVAL;

	n = &g_dawnhex.nodes[id];
	if (n->present)
		return -EEXIST;

	memset(n, 0, sizeof(*n));
	n->id = id;
	n->present = true;
	n->online = true;
	n->energy_ppm = 500000;
	n->health_ppm = 1000000;
	strscpy(n->name, name, sizeof(n->name));

	g_dawnhex.generation++;
	dh_emit_event_locked("add id=%u name=%s", id, n->name);
	return 0;
}

static int dh_del_node_locked(u32 id)
{
	u32 i;

	if (id >= DAWNHEX_MAX_NODES)
		return -EINVAL;
	if (!g_dawnhex.nodes[id].present)
		return -ENOENT;

	for (i = 0; i < DAWNHEX_MAX_NODES; ++i)
		g_dawnhex.nodes[i].links_mask &= ~(1ULL << id);

	memset(&g_dawnhex.nodes[id], 0, sizeof(g_dawnhex.nodes[id]));
	g_dawnhex.generation++;
	dh_emit_event_locked("del id=%u", id);
	return 0;
}

static int dh_link_locked(u32 a, u32 b, bool enable)
{
	struct dawnhex_node *na, *nb;

	if (a == b)
		return -EINVAL;

	na = dh_lookup_node_locked(a);
	nb = dh_lookup_node_locked(b);
	if (!na || !nb)
		return -ENOENT;

	if (enable) {
		na->links_mask |= (1ULL << b);
		nb->links_mask |= (1ULL << a);
		dh_emit_event_locked("link a=%u b=%u", a, b);
	} else {
		na->links_mask &= ~(1ULL << b);
		nb->links_mask &= ~(1ULL << a);
		dh_emit_event_locked("unlink a=%u b=%u", a, b);
	}

	g_dawnhex.generation++;
	return 0;
}

static void dh_tick_once_locked(void)
{
	u32 i;

	g_dawnhex.generation++;

	for (i = 0; i < DAWNHEX_MAX_NODES; ++i) {
		struct dawnhex_node *n = &g_dawnhex.nodes[i];
		u32 link_factor;

		if (!n->present || !n->online)
			continue;

		link_factor = hweight64(n->links_mask) * 5000;
		n->energy_ppm = dh_clamp_u32(n->energy_ppm + 1000 + link_factor, 10000, 1000000);

		if (n->health_ppm > 250000)
			n->health_ppm -= 250;

		dh_emit_event_locked("tick id=%u energy=%u health=%u links=%u",
				     n->id, n->energy_ppm, n->health_ppm,
				     hweight64(n->links_mask));
	}
}

static void dh_tick_workfn(struct work_struct *work)
{
	mutex_lock(&g_dawnhex.lock);
	if (g_dawnhex.interactive)
		dh_tick_once_locked();
	mutex_unlock(&g_dawnhex.lock);

	if (g_dawnhex.interactive)
		schedule_delayed_work(&g_dawnhex.tick_work, msecs_to_jiffies(g_dawnhex.tick_ms));
}

static ssize_t dh_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
	char tmp[DAWNHEX_EVENT_LEN];
	size_t n;
	int ret;

	if (!len)
		return 0;

	ret = wait_event_interruptible(
		g_dawnhex.event_wq,
		g_dawnhex.ev_head != g_dawnhex.ev_tail
	);
	if (ret)
		return ret;

	mutex_lock(&g_dawnhex.lock);

	if (g_dawnhex.ev_head == g_dawnhex.ev_tail) {
		mutex_unlock(&g_dawnhex.lock);
		return 0;
	}

	strscpy(tmp, g_dawnhex.events[g_dawnhex.ev_tail], sizeof(tmp));
	g_dawnhex.ev_tail = (g_dawnhex.ev_tail + 1) % DAWNHEX_EVENT_Q;
	mutex_unlock(&g_dawnhex.lock);

	n = strnlen(tmp, sizeof(tmp));
	if (n > len)
		n = len;

	if (copy_to_user(buf, tmp, n))
		return -EFAULT;

	return n;
}

static __poll_t dh_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;

	poll_wait(file, &g_dawnhex.event_wq, wait);

	mutex_lock(&g_dawnhex.lock);
	if (g_dawnhex.ev_head != g_dawnhex.ev_tail)
		mask |= EPOLLIN | EPOLLRDNORM;
	mutex_unlock(&g_dawnhex.lock);

	return mask;
}

static int dh_handle_command_locked(char *cmd)
{
	u32 a, b, v;
	char name[DAWNHEX_NAME_LEN];
    case DAWNHEX_DUO_IOC_ADD_PAIR: {
		struct dawnhex_duo_add_uapi add;
		u32 pair_id = 0;

		if (copy_from_user(&add, (void __user *)arg, sizeof(add)))
			return -EFAULT;

		mutex_lock(&g_dawnhex.lock);
		ret = dh_duo_add_pair_locked(add.a, add.b, add.mode, &pair_id);
		mutex_unlock(&g_dawnhex.lock);
		if (ret)
			return ret;

		add.out_pair_id = pair_id;
		if (copy_to_user((void __user *)arg, &add, sizeof(add)))
			return -EFAULT;
		return 0;
	}

	case DAWNHEX_DUO_IOC_DEL_PAIR: {
		struct dawnhex_duo_del_uapi del;

		if (copy_from_user(&del, (void __user *)arg, sizeof(del)))
			return -EFAULT;

		mutex_lock(&g_dawnhex.lock);
		ret = dh_duo_del_pair_locked(del.pair_id);
		mutex_unlock(&g_dawnhex.lock);
		return ret;
	}

	case DAWNHEX_DUO_IOC_PULSE: {
		struct dawnhex_duo_pulse_uapi pulse;

		if (copy_from_user(&pulse, (void __user *)arg, sizeof(pulse)))
			return -EFAULT;

		mutex_lock(&g_dawnhex.lock);
		ret = dh_duo_pulse_locked(pulse.pair_id, pulse.value_ppm);
		mutex_unlock(&g_dawnhex.lock);
		return ret;
	}

	case DAWNHEX_DUO_IOC_GET_PAIR: {
		struct dawnhex_duo_get_uapi get;
		struct dawnhex_duo_pair *p;

		if (copy_from_user(&get, (void __user *)arg, sizeof(get)))
			return -EFAULT;

		mutex_lock(&g_dawnhex.lock);
		p = dh_duo_lookup_pair_locked(get.pair_id);
		if (!p) {
			mutex_unlock(&g_dawnhex.lock);
			return -ENOENT;
		}

		get.pair.pair_id = p->pair_id;
		get.pair.a = p->a;
		get.pair.b = p->b;
		get.pair.mode = p->mode;
		get.pair.active = p->active;
		get.pair.quotient_ppm = p->quotient_ppm;
		get.pair.refraction_ppm = p->refraction_ppm;
		get.pair.energy_ppm = p->energy_ppm;
		get.pair.pressure_ppm = p->pressure_ppm;
		get.pair.pulses = p->pulses;
		get.pair.last_value_ppm = p->last_value_ppm;
		mutex_unlock(&g_dawnhex.lock);

		if (copy_to_user((void __user *)arg, &get, sizeof(get)))
			return -EFAULT;
		return 0;
	}

	case DAWNHEX_DUO_IOC_DUMP: {
		struct dawnhex_duo_dump_uapi dump;
		u32 i;

		memset(&dump, 0, sizeof(dump));

		mutex_lock(&g_dawnhex.lock);
		dump.pair_count = g_dawnhex.duo.pair_count;
		for (i = 0; i < g_dawnhex.duo.pair_count; ++i) {
			struct dawnhex_duo_pair *p = &g_dawnhex.duo.pairs[i];
			dump.pairs[i].pair_id = p->pair_id;
			dump.pairs[i].a = p->a;
			dump.pairs[i].b = p->b;
			dump.pairs[i].mode = p->mode;
			dump.pairs[i].active = p->active;
			dump.pairs[i].quotient_ppm = p->quotient_ppm;
			dump.pairs[i].refraction_ppm = p->refraction_ppm;
			dump.pairs[i].energy_ppm = p->energy_ppm;
			dump.pairs[i].pressure_ppm = p->pressure_ppm;
			dump.pairs[i].pulses = p->pulses;
			dump.pairs[i].last_value_ppm = p->last_value_ppm;
		}
		mutex_unlock(&g_dawnhex.lock);

		if (copy_to_user((void __user *)arg, &dump, sizeof(dump)))
			return -EFAULT;
		return 0;
	}
	if (sscanf(cmd, "add %u %31s", &a, name) == 2)
		return dh_add_node_locked(a, name);

	if (sscanf(cmd, "del %u", &a) == 1)
		return dh_del_node_locked(a);

	if (sscanf(cmd, "link %u %u", &a, &b) == 2)
		return dh_link_locked(a, b, true);

	if (sscanf(cmd, "unlink %u %u", &a, &b) == 2)
		return dh_link_locked(a, b, false);

	if (sscanf(cmd, "online %u %u", &a, &v) == 2) {
		struct dawnhex_node *n = dh_lookup_node_locked(a);
		if (!n)
			return -ENOENT;
		n->online = !!v;
		g_dawnhex.generation++;
		dh_emit_event_locked("online id=%u value=%u", a, !!v);
		return 0;
	}
    if (sscanf(cmd, "duo add %u %u %u", &a, &b, &v) == 3) {
		u32 pair_id;
		return dh_duo_add_pair_locked(a, b, v, &pair_id);
	}

	if (sscanf(cmd, "duo del %u", &a) == 1)
		return dh_duo_del_pair_locked(a);

	if (sscanf(cmd, "duo pulse %u %u", &a, &v) == 2)
		return dh_duo_pulse_locked(a, v);

	if (!strncmp(cmd, "duo snapshot", 12)) {
		dh_emit_event_locked("duo_snapshot pairs=%u", g_dawnhex.duo.pair_count);
		return 0;
	}
	if (sscanf(cmd, "energy %u %u", &a, &v) == 2) {
		struct dawnhex_node *n = dh_lookup_node_locked(a);
		if (!n)
			return -ENOENT;
		n->energy_ppm = dh_clamp_u32(v, 0, 1000000);
		g_dawnhex.generation++;
		dh_emit_event_locked("energy id=%u value=%u", a, n->energy_ppm);
		return 0;
	}

	if (sscanf(cmd, "health %u %u", &a, &v) == 2) {
		struct dawnhex_node *n = dh_lookup_node_locked(a);
		if (!n)
			return -ENOENT;
		n->health_ppm = dh_clamp_u32(v, 0, 1000000);
		g_dawnhex.generation++;
		dh_emit_event_locked("health id=%u value=%u", a, n->health_ppm);
		return 0;
	}

	if (!strncmp(cmd, "tick", 4)) {
		dh_tick_once_locked();
		return 0;
	}

	if (sscanf(cmd, "interactive %u", &v) == 1) {
		g_dawnhex.interactive = !!v;
		if (g_dawnhex.interactive)
			schedule_delayed_work(&g_dawnhex.tick_work, msecs_to_jiffies(g_dawnhex.tick_ms));
		else
			cancel_delayed_work_sync(&g_dawnhex.tick_work);
		dh_emit_event_locked("interactive value=%u", !!v);
		return 0;
	}

	if (sscanf(cmd, "tick_ms %u", &v) == 1) {
		g_dawnhex.tick_ms = dh_clamp_u32(v, 10, 60000);
		dh_emit_event_locked("tick_ms value=%u", g_dawnhex.tick_ms);
		return 0;
	}

	if (!strncmp(cmd, "snapshot", 8)) {
		dh_emit_event_locked("snapshot generation=%u nodes=%u links=%u",
				     g_dawnhex.generation,
				     dh_count_nodes_locked(),
				     dh_count_links_locked());
		return 0;
	}

	if (!strncmp(cmd, "help", 4)) {
		dh_emit_event_locked("cmds: add del link unlink online energy health tick interactive tick_ms snapshot");
		return 0;
	}

	return -EINVAL;
}

static ssize_t dh_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
	char kbuf[128];
	size_t n;
	int ret;

	n = min_t(size_t, len, sizeof(kbuf) - 1);
	if (copy_from_user(kbuf, buf, n))
		return -EFAULT;
	kbuf[n] = '\0';
	strreplace(kbuf, '\n', '\0');

	mutex_lock(&g_dawnhex.lock);
	ret = dh_handle_command_locked(kbuf);
	mutex_unlock(&g_dawnhex.lock);

	if (ret)
		return ret;
	return len;
}

static long dh_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct dawnhex_cluster_uapi cluster;
	struct dawnhex_node_query query;
	u32 i;

	switch (cmd) {
	case DAWNHEX_IOC_GET_CLUSTER:
		memset(&cluster, 0, sizeof(cluster));

		mutex_lock(&g_dawnhex.lock);
		cluster.node_count = dh_count_nodes_locked();
		cluster.link_count = dh_count_links_locked();
		cluster.generation = g_dawnhex.generation;
		cluster.interactive = g_dawnhex.interactive;
		cluster.tick_ms = g_dawnhex.tick_ms;

		for (i = 0; i < DAWNHEX_MAX_NODES; ++i) {
			cluster.nodes[i].id = g_dawnhex.nodes[i].id;
			cluster.nodes[i].present = g_dawnhex.nodes[i].present;
			cluster.nodes[i].online = g_dawnhex.nodes[i].online;
			cluster.nodes[i].energy_ppm = g_dawnhex.nodes[i].energy_ppm;
			cluster.nodes[i].health_ppm = g_dawnhex.nodes[i].health_ppm;
			cluster.nodes[i].links_mask = g_dawnhex.nodes[i].links_mask;
			strscpy(cluster.nodes[i].name, g_dawnhex.nodes[i].name,
				sizeof(cluster.nodes[i].name));
		}
		mutex_unlock(&g_dawnhex.lock);

		if (copy_to_user((void __user *)arg, &cluster, sizeof(cluster)))
			return -EFAULT;
		return 0;

	case DAWNHEX_IOC_GET_NODE:
		if (copy_from_user(&query, (void __user *)arg, sizeof(query)))
			return -EFAULT;

		mutex_lock(&g_dawnhex.lock);
		if (query.id >= DAWNHEX_MAX_NODES || !g_dawnhex.nodes[query.id].present) {
			mutex_unlock(&g_dawnhex.lock);
			return -ENOENT;
		}

		query.node.id = g_dawnhex.nodes[query.id].id;
		query.node.present = g_dawnhex.nodes[query.id].present;
		query.node.online = g_dawnhex.nodes[query.id].online;
		query.node.energy_ppm = g_dawnhex.nodes[query.id].energy_ppm;
		query.node.health_ppm = g_dawnhex.nodes[query.id].health_ppm;
		query.node.links_mask = g_dawnhex.nodes[query.id].links_mask;
		strscpy(query.node.name, g_dawnhex.nodes[query.id].name,
			sizeof(query.node.name));
		mutex_unlock(&g_dawnhex.lock);

		if (copy_to_user((void __user *)arg, &query, sizeof(query)))
			return -EFAULT;
		return 0;

	case DAWNHEX_IOC_STEP:
		mutex_lock(&g_dawnhex.lock);
		dh_tick_once_locked();
		mutex_unlock(&g_dawnhex.lock);
		return 0;
	}

    case DAWNHEX_DUO_IOC_GET_SIMPLEX: {
		struct dawnhex_duo_simplex_get_uapi get;
		struct dawnhex_duo_pair *p;
		u32 i, m;

		if (copy_from_user(&get, (void __user *)arg, sizeof(get)))
			return -EFAULT;

		mutex_lock(&g_dawnhex.lock);
		p = dh_duo_lookup_pair_locked(get.pair_id);
		if (!p) {
			mutex_unlock(&g_dawnhex.lock);
			return -ENOENT;
		}

		memset(&get.simplex, 0, sizeof(get.simplex));
		get.simplex.pair_id = p->pair_id;
		get.simplex.level_count = p->simplex.level_count;
		get.simplex.node_count = p->simplex.node_count;
		get.simplex.base_retained_ppm = p->simplex.base_retained_ppm;
		get.simplex.recursive_retained_ppm = p->simplex.recursive_retained_ppm;
		get.simplex.recursive_reduction_ppm = p->simplex.recursive_reduction_ppm;
		get.simplex.recursive_crosshatch_ppm = p->simplex.recursive_crosshatch_ppm;
		get.simplex.recursive_coldness_ppm = p->simplex.recursive_coldness_ppm;
		get.simplex.recursive_energy_ppm = p->simplex.recursive_energy_ppm;

		for (i = 0; i < p->simplex.level_count; ++i) {
			get.simplex.levels[i].level_index = p->simplex.levels[i].level_index;
			get.simplex.levels[i].fib_value = p->simplex.levels[i].fib_value;
			get.simplex.levels[i].fib_weight_ppm = p->simplex.levels[i].fib_weight_ppm;
			get.simplex.levels[i].node_count = p->simplex.levels[i].node_count;
			get.simplex.levels[i].retained_ppm = p->simplex.levels[i].retained_ppm;
			get.simplex.levels[i].reduction_ppm = p->simplex.levels[i].reduction_ppm;
			get.simplex.levels[i].crosshatch_ppm = p->simplex.levels[i].crosshatch_ppm;
			get.simplex.levels[i].coldness_ppm = p->simplex.levels[i].coldness_ppm;
			get.simplex.levels[i].energy_ppm = p->simplex.levels[i].energy_ppm;
			get.simplex.levels[i].pressure_ppm = p->simplex.levels[i].pressure_ppm;

			for (m = 0; m < DAWNHEX_SIMPLEX_MIPS; ++m)
				get.simplex.levels[i].mip_retained_ppm[m] =
					p->simplex.levels[i].mip_retained_ppm[m];
		}
		mutex_unlock(&g_dawnhex.lock);

		if (copy_to_user((void __user *)arg, &get, sizeof(get)))
			return -EFAULT;
		return 0;
	}
	
	return -ENOTTY;
}

static ssize_t duo_simplex_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t n = 0;
	u32 i;

	mutex_lock(&g_dawnhex.lock);
	for (i = 0; i < g_dawnhex.duo.pair_count; ++i) {
		struct dawnhex_duo_pair *p = &g_dawnhex.duo.pairs[i];

		n += scnprintf(buf + n, PAGE_SIZE - n,
			       "pair=%u base_ret=%u rec_ret=%u rec_red=%u rec_cross=%u rec_cold=%u rec_energy=%u levels=%u\n",
			       p->pair_id,
			       p->simplex.base_retained_ppm,
			       p->simplex.recursive_retained_ppm,
			       p->simplex.recursive_reduction_ppm,
			       p->simplex.recursive_crosshatch_ppm,
			       p->simplex.recursive_coldness_ppm,
			       p->simplex.recursive_energy_ppm,
			       p->simplex.level_count);

		if (p->simplex.level_count > 0) {
			struct dawnhex_duo_simplex_level *l0 = &p->simplex.levels[0];
			n += scnprintf(buf + n, PAGE_SIZE - n,
				       "  level0 fib=%u retained=%u reduction=%u cross=%u cold=%u energy=%u\n",
				       l0->fib_value,
				       l0->retained_ppm,
				       l0->reduction_ppm,
				       l0->crosshatch_ppm,
				       l0->coldness_ppm,
				       l0->energy_ppm);
		}

		if (n >= PAGE_SIZE)
			break;
	}
	mutex_unlock(&g_dawnhex.lock);

	return n;
}

static DEVICE_ATTR_RO(duo_simplex);

static const struct file_operations dawnhex_fops = {
	.owner          = THIS_MODULE,
	.read           = dh_read,
	.write          = dh_write,
	.poll           = dh_poll,
	.unlocked_ioctl = dh_ioctl,
	.compat_ioctl   = dh_ioctl,
	.llseek         = no_llseek,
};

static ssize_t summary_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t n;

	mutex_lock(&g_dawnhex.lock);
	n = scnprintf(buf, PAGE_SIZE,
		      "generation=%u nodes=%u links=%u interactive=%u tick_ms=%u\n",
		      g_dawnhex.generation,
		      dh_count_nodes_locked(),
		      dh_count_links_locked(),
		      g_dawnhex.interactive,
		      g_dawnhex.tick_ms);
	mutex_unlock(&g_dawnhex.lock);

	return n;
}

static ssize_t topology_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t n = 0;
	u32 i;

	mutex_lock(&g_dawnhex.lock);
	for (i = 0; i < DAWNHEX_MAX_NODES; ++i) {
		struct dawnhex_node *node = &g_dawnhex.nodes[i];

		if (!node->present)
			continue;

		n += scnprintf(buf + n, PAGE_SIZE - n,
			       "id=%u name=%s online=%u energy=%u health=%u links=0x%016llx\n",
			       node->id, node->name, node->online,
			       node->energy_ppm, node->health_ppm,
			       (unsigned long long)node->links_mask);
		if (n >= PAGE_SIZE)
			break;
	}
	mutex_unlock(&g_dawnhex.lock);

	return n;
}

static ssize_t tick_ms_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t n;

	mutex_lock(&g_dawnhex.lock);
	n = scnprintf(buf, PAGE_SIZE, "%u\n", g_dawnhex.tick_ms);
	mutex_unlock(&g_dawnhex.lock);

	return n;
}

static ssize_t tick_ms_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	u32 v;

	if (kstrtou32(buf, 10, &v))
		return -EINVAL;

	mutex_lock(&g_dawnhex.lock);
	g_dawnhex.tick_ms = dh_clamp_u32(v, 10, 60000);
	dh_emit_event_locked("tick_ms value=%u", g_dawnhex.tick_ms);
	mutex_unlock(&g_dawnhex.lock);

	return count;
}

static ssize_t interactive_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t n;

	mutex_lock(&g_dawnhex.lock);
	n = scnprintf(buf, PAGE_SIZE, "%u\n", g_dawnhex.interactive);
	mutex_unlock(&g_dawnhex.lock);

	return n;
}

static ssize_t interactive_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	bool v;

	if (kstrtobool(buf, &v))
		return -EINVAL;

	mutex_lock(&g_dawnhex.lock);
	g_dawnhex.interactive = v;
	if (g_dawnhex.interactive)
		schedule_delayed_work(&g_dawnhex.tick_work, msecs_to_jiffies(g_dawnhex.tick_ms));
	else
		cancel_delayed_work_sync(&g_dawnhex.tick_work);
	dh_emit_event_locked("interactive value=%u", g_dawnhex.interactive);
	mutex_unlock(&g_dawnhex.lock);

	return count;
}

static DEVICE_ATTR_RO(summary);
static DEVICE_ATTR_RO(topology);
static DEVICE_ATTR_RW(tick_ms);
static DEVICE_ATTR_RW(interactive);

static struct attribute *dawnhex_attrs[] = {
    &dev_attr_duo.attr,
    &dev_attr_duo_simplex.attr,
	&dev_attr_summary.attr,
	&dev_attr_topology.attr,
	&dev_attr_tick_ms.attr,
	&dev_attr_interactive.attr,
	NULL,
};

static const struct attribute_group dawnhex_group = {
	.attrs = dawnhex_attrs,
};

static const struct attribute_group *dawnhex_groups[] = {
	&dawnhex_group,
	NULL,
};

static int __init dawnhex_init(void)
{
	int ret;

	memset(&g_dawnhex, 0, sizeof(g_dawnhex));
	mutex_init(&g_dawnhex.lock);
	init_waitqueue_head(&g_dawnhex.event_wq);

	g_dawnhex.tick_ms = DAWNHEX_DEFAULT_TICK_MS;
	g_dawnhex.interactive = true;
	INIT_DELAYED_WORK(&g_dawnhex.tick_work, dh_tick_workfn);
    INIT_LIST_HEAD(&g_dawnhex.duo.hooks);
	g_dawnhex.duo.pair_count = 0;
	g_dawnhex.duo.next_pair_id = 0;
	g_dawnhex.misc.minor = MISC_DYNAMIC_MINOR;
	g_dawnhex.misc.name = "dawnhex";
	g_dawnhex.misc.fops = &dawnhex_fops;
	g_dawnhex.misc.mode = 0660;
	g_dawnhex.misc.groups = dawnhex_groups;

	ret = misc_register(&g_dawnhex.misc);
	if (ret)
		return ret;

	mutex_lock(&g_dawnhex.lock);
	dh_add_node_locked(0, "core");
	dh_add_node_locked(1, "mesh-a");
	dh_add_node_locked(2, "mesh-b");
	dh_link_locked(0, 1, true);
	dh_link_locked(0, 2, true);
	mutex_unlock(&g_dawnhex.lock);

	schedule_delayed_work(&g_dawnhex.tick_work, msecs_to_jiffies(g_dawnhex.tick_ms));

	pr_info("dawnhex: loaded /dev/dawnhex\n");
	return 0;
}

static void __exit dawnhex_exit(void)
{
	cancel_delayed_work_sync(&g_dawnhex.tick_work);
	misc_deregister(&g_dawnhex.misc);
	pr_info("dawnhex: unloaded\n");
}

static ssize_t duo_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t n = 0;
	u32 i;

	mutex_lock(&g_dawnhex.lock);
	n += scnprintf(buf + n, PAGE_SIZE - n, "pair_count=%u\n", g_dawnhex.duo.pair_count);
	for (i = 0; i < g_dawnhex.duo.pair_count; ++i) {
		struct dawnhex_duo_pair *p = &g_dawnhex.duo.pairs[i];
		n += scnprintf(buf + n, PAGE_SIZE - n,
			       "pair=%u a=%u b=%u mode=%u active=%u quotient=%u refraction=%u energy=%u pressure=%u pulses=%u last=%u\n",
			       p->pair_id, p->a, p->b, p->mode, p->active,
			       p->quotient_ppm, p->refraction_ppm,
			       p->energy_ppm, p->pressure_ppm,
			       p->pulses, p->last_value_ppm);
		if (n >= PAGE_SIZE)
			break;
	}
	mutex_unlock(&g_dawnhex.lock);

	return n;
}

static DEVICE_ATTR_RO(duo);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("DawnHex interactive connected kernel cluster");
MODULE_VERSION("0.1.0");

module_init(dawnhex_init);
module_exit(dawnhex_exit);
