#ifndef DAWNHEX_DUO_UAPI_H
#define DAWNHEX_DUO_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define DAWNHEX_DUO_IOC_MAGIC 0xD5
#define DAWNHEX_DUO_MAX_PAIRS 128

enum dawnhex_duo_mode {
	DAWNHEX_DUO_DISABLED = 0,
	DAWNHEX_DUO_INOUT_INOUT_OUT = 1,
	DAWNHEX_DUO_INOUT_INOUT_IN  = 2,
	DAWNHEX_DUO_CROSSED         = 3,
};

struct dawnhex_duo_pair_uapi {
	__u32 pair_id;
	__u32 a;
	__u32 b;
	__u32 mode;
	__u32 active;

	__u32 quotient_ppm;     /* 1.0 == 1000000 */
	__u32 refraction_ppm;
	__u32 energy_ppm;
	__u32 pressure_ppm;

	__u32 pulses;
	__u32 last_value_ppm;
};

struct dawnhex_duo_cfg_uapi {
	__u32 version;
	__u32 pair_count;
	__u32 reserved[6];
};

struct dawnhex_duo_add_uapi {
	__u32 a;
	__u32 b;
	__u32 mode;
	__u32 reserved;
	__u32 out_pair_id;
};

struct dawnhex_duo_del_uapi {
	__u32 pair_id;
};

struct dawnhex_duo_pulse_uapi {
	__u32 pair_id;
	__u32 value_ppm;
};

struct dawnhex_duo_get_uapi {
	__u32 pair_id;
	struct dawnhex_duo_pair_uapi pair;
};

struct dawnhex_duo_dump_uapi {
	__u32 pair_count;
	struct dawnhex_duo_pair_uapi pairs[DAWNHEX_DUO_MAX_PAIRS];
};

#define DAWNHEX_DUO_IOC_ADD_PAIR  _IOWR(DAWNHEX_DUO_IOC_MAGIC, 0x30, struct dawnhex_duo_add_uapi)
#define DAWNHEX_DUO_IOC_DEL_PAIR  _IOW(DAWNHEX_DUO_IOC_MAGIC, 0x31, struct dawnhex_duo_del_uapi)
#define DAWNHEX_DUO_IOC_PULSE     _IOW(DAWNHEX_DUO_IOC_MAGIC, 0x32, struct dawnhex_duo_pulse_uapi)
#define DAWNHEX_DUO_IOC_GET_PAIR  _IOWR(DAWNHEX_DUO_IOC_MAGIC, 0x33, struct dawnhex_duo_get_uapi)
#define DAWNHEX_DUO_IOC_DUMP      _IOR(DAWNHEX_DUO_IOC_MAGIC, 0x34, struct dawnhex_duo_dump_uapi)

#endif
