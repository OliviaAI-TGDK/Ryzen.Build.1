#ifndef DSVRAM_UAPI_H
#define DSVRAM_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define DSVRAM_IOCTL_MAGIC 0xDA

struct dsvram_cfg {
    __u32 version;
    __u32 target_pool_gb;       // 98, 264, etc.
    __u32 physical_vram_gb;     // informational only
    __u32 flags;                // bit0=enable, bit1=compress, bit2=prefetch
    __u32 compression_ppm;      // retained ratio in ppm, 10200 = 0.0102
    __u32 mip_levels;           // 9
    __u32 page_kb;              // 4
    __u32 reserved0;
    __u64 virtual_pool_bytes;
    __u64 hotset_bytes;
    __u64 coldset_bytes;
};

struct dsvram_stats {
    __u64 target_pool_bytes;
    __u64 mapped_bytes;
    __u64 compressed_bytes;
    __u64 evicted_bytes;
    __u64 faults_simulated;
    __u64 prefetch_ops;
};

#define DSVRAM_IOC_SET_CFG   _IOW(DSVRAM_IOCTL_MAGIC, 0x01, struct dsvram_cfg)
#define DSVRAM_IOC_GET_CFG   _IOR(DSVRAM_IOCTL_MAGIC, 0x02, struct dsvram_cfg)
#define DSVRAM_IOC_GET_STATS _IOR(DSVRAM_IOCTL_MAGIC, 0x03, struct dsvram_stats)

#endif
