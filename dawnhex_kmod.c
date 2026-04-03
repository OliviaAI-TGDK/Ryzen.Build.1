// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
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

	return -ENOTTY;
}

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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("DawnHex interactive connected kernel cluster");
MODULE_VERSION("0.1.0");

module_init(dawnhex_init);
module_exit(dawnhex_exit);
