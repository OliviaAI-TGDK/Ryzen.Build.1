#ifndef DAWNHEX_UAPI_H
#define DAWNHEX_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define DAWNHEX_IOC_MAGIC 0xD4
#define DAWNHEX_MAX_NODES 64
#define DAWNHEX_NAME_LEN 32

struct dawnhex_node_uapi {
	__u32 id;
	char  name[DAWNHEX_NAME_LEN];
	__u32 present;
	__u32 online;
	__u32 energy_ppm;   /* 1.0 == 1000000 */
	__u32 health_ppm;   /* 1.0 == 1000000 */
	__u64 links_mask;   /* one bit per node id 0..63 */
};

struct dawnhex_cluster_uapi {
	__u32 node_count;
	__u32 link_count;
	__u32 generation;
	__u32 interactive;
	__u32 tick_ms;
	__u32 reserved[3];
	struct dawnhex_node_uapi nodes[DAWNHEX_MAX_NODES];
};

struct dawnhex_node_query {
	__u32 id;
	struct dawnhex_node_uapi node;
};

#define DAWNHEX_IOC_GET_CLUSTER _IOR(DAWNHEX_IOC_MAGIC, 0x20, struct dawnhex_cluster_uapi)
#define DAWNHEX_IOC_GET_NODE    _IOWR(DAWNHEX_IOC_MAGIC, 0x21, struct dawnhex_node_query)
#define DAWNHEX_IOC_STEP        _IO(DAWNHEX_IOC_MAGIC, 0x22)

#endif
