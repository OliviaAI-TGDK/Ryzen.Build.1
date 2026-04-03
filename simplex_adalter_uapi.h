/* simplex_adalter_uapi.h */
#ifndef SIMPLEX_ADALTER_UAPI_H
#define SIMPLEX_ADALTER_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SIMPLEX_ADALTER_IOC_MAGIC 0xB7
#define SIMPLEX_ADALTER_MAX_LEVELS 22
#define SIMPLEX_ADALTER_NODES_PER_LEVEL 12
#define SIMPLEX_ADALTER_MIPS 9
#define SIMPLEX_ADALTER_KEY_LEN 128

struct simplex_adalter_cfg {
	__u32 version;

	__u32 recursion_levels;      /* default 22 */
	__u32 nodes_per_level;       /* default 12 */

	/* ratios in ppm: 1.0 == 1000000 */
	__u32 min_retained_ppm;      /* 10200  => 0.0102 */
	__u32 max_retained_ppm;      /* 120000 => 0.12   */
	__u32 level_gain_ppm;        /* 144000 => 0.144  */
	__u32 node_gain_ppm;         /* 21000  => 0.021  */
	__u32 phase_gain_ppm;        /* 10200  => 0.0102 */

	/* q16 fixed-point: 1.0 == 65536 */
	__u32 arm_q16;               /* 1.188  */
	__u32 adder_q16;             /* 0.42   */
	__u32 modifier_q16;          /* 1.035  */
	__u32 u_scalar_q16;          /* 1.75   */
	__u32 knot_q16;              /* 0.4896 */

	__u32 flags;                 /* bit0 supercompute, bit1 chip_relay */
	__u32 reserved[4];
};

struct simplex_adalter_base {
	char  key[SIMPLEX_ADALTER_KEY_LEN];
	__u32 final_base;

	/* supplied by userspace, typically from XEM/OuijiHex policy */
	__u32 retained_ppm;
	__u32 crosshatch_ppm;
	__u32 coldness_ppm;
};

struct simplex_adalter_level {
	__u32 level_index;
	__u32 fib_value;
	__u32 fib_weight_ppm;

	__u32 retained_ppm;
	__u32 reduction_ppm;
	__u32 crosshatch_ppm;
	__u32 coldness_ppm;
	__u32 energy_ppm;

	__u32 mip_retained_ppm[SIMPLEX_ADALTER_MIPS];
};

struct simplex_adalter_report {
	char  key[SIMPLEX_ADALTER_KEY_LEN];
	__u32 final_base;

	__u32 average_retained_ppm;
	__u32 average_reduction_ppm;
	__u32 average_crosshatch_ppm;
	__u32 average_coldness_ppm;
	__u32 recursive_energy_ppm;

	__u32 recursion_levels;
	__u32 nodes_per_level;

	struct simplex_adalter_level levels[SIMPLEX_ADALTER_MAX_LEVELS];
};

#define SIMPLEX_ADALTER_IOC_SET_CFG   _IOW(SIMPLEX_ADALTER_IOC_MAGIC, 0x10, struct simplex_adalter_cfg)
#define SIMPLEX_ADALTER_IOC_GET_CFG   _IOR(SIMPLEX_ADALTER_IOC_MAGIC, 0x11, struct simplex_adalter_cfg)
#define SIMPLEX_ADALTER_IOC_RUN_BASE  _IOW(SIMPLEX_ADALTER_IOC_MAGIC, 0x12, struct simplex_adalter_base)
#define SIMPLEX_ADALTER_IOC_GET_REPORT _IOR(SIMPLEX_ADALTER_IOC_MAGIC, 0x13, struct simplex_adalter_report)

#endif
