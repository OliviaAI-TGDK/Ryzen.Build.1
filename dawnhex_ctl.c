/* dawnhex_ctl.c
 *
 * Build:
 *   gcc -O2 dawnhex_ctl.c -o dawnhex_ctl
 *
 * Examples:
 *   ./dawnhex_ctl cmd "add 3 mesh-c"
 *   ./dawnhex_ctl cmd "link 1 3"
 *   ./dawnhex_ctl read
 *   ./dawnhex_ctl cluster
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "dawnhex_uapi.h"
#include "dawnhex_duo_uapi.h"

static void die(const char *msg) {
	perror(msg);
	exit(1);
}

int main(int argc, char **argv)
{
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: %s cmd <text> | read | cluster | node <id> | step\n", argv[0]);
		return 1;
	}

	fd = open("/dev/dawnhex", O_RDWR);
	if (fd < 0)
		die("open /dev/dawnhex");

	if (!strcmp(argv[1], "cmd")) {
		if (argc < 3) {
			fprintf(stderr, "missing command text\n");
			return 1;
		}
		if (write(fd, argv[2], strlen(argv[2])) < 0)
			die("write");
		return 0;
	}

	if (!strcmp(argv[1], "read")) {
		char buf[256];
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		if (n < 0)
			die("read");
		buf[n] = '\0';
		printf("%s\n", buf);
		return 0;
	}

	if (!strcmp(argv[1], "duo-add")) {
		struct dawnhex_duo_add_uapi add;
		if (argc < 5) {
			fprintf(stderr, "usage: %s duo-add <a> <b> <mode>\n", argv[0]);
			return 1;
		}
		memset(&add, 0, sizeof(add));
		add.a = (unsigned)strtoul(argv[2], NULL, 10);
		add.b = (unsigned)strtoul(argv[3], NULL, 10);
		add.mode = (unsigned)strtoul(argv[4], NULL, 10);

		if (ioctl(fd, DAWNHEX_DUO_IOC_ADD_PAIR, &add) < 0)
			die("ioctl DUO_ADD_PAIR");
		printf("pair_id=%u\n", add.out_pair_id);
		return 0;
	}

	if (!strcmp(argv[1], "duo-del")) {
		struct dawnhex_duo_del_uapi del;
		if (argc < 3) {
			fprintf(stderr, "usage: %s duo-del <pair_id>\n", argv[0]);
			return 1;
		}
		memset(&del, 0, sizeof(del));
		del.pair_id = (unsigned)strtoul(argv[2], NULL, 10);

		if (ioctl(fd, DAWNHEX_DUO_IOC_DEL_PAIR, &del) < 0)
			die("ioctl DUO_DEL_PAIR");
		return 0;
	}

	if (!strcmp(argv[1], "duo-pulse")) {
		struct dawnhex_duo_pulse_uapi pulse;
		if (argc < 4) {
			fprintf(stderr, "usage: %s duo-pulse <pair_id> <value_ppm>\n", argv[0]);
			return 1;
		}
		memset(&pulse, 0, sizeof(pulse));
		pulse.pair_id = (unsigned)strtoul(argv[2], NULL, 10);
		pulse.value_ppm = (unsigned)strtoul(argv[3], NULL, 10);

		if (ioctl(fd, DAWNHEX_DUO_IOC_PULSE, &pulse) < 0)
			die("ioctl DUO_PULSE");
		return 0;
	}

	if (!strcmp(argv[1], "duo-get")) {
		struct dawnhex_duo_get_uapi get;
		if (argc < 3) {
			fprintf(stderr, "usage: %s duo-get <pair_id>\n", argv[0]);
			return 1;
		}
		memset(&get, 0, sizeof(get));
		get.pair_id = (unsigned)strtoul(argv[2], NULL, 10);

		if (ioctl(fd, DAWNHEX_DUO_IOC_GET_PAIR, &get) < 0)
			die("ioctl DUO_GET_PAIR");

		printf("pair=%u a=%u b=%u mode=%u active=%u quotient=%u refraction=%u energy=%u pressure=%u pulses=%u last=%u\n",
		       get.pair.pair_id, get.pair.a, get.pair.b, get.pair.mode,
		       get.pair.active, get.pair.quotient_ppm, get.pair.refraction_ppm,
		       get.pair.energy_ppm, get.pair.pressure_ppm,
		       get.pair.pulses, get.pair.last_value_ppm);
		return 0;
	}

	if (!strcmp(argv[1], "duo-dump")) {
		struct dawnhex_duo_dump_uapi dump;
		memset(&dump, 0, sizeof(dump));

		if (ioctl(fd, DAWNHEX_DUO_IOC_DUMP, &dump) < 0)
			die("ioctl DUO_DUMP");

		printf("pair_count=%u\n", dump.pair_count);
		for (unsigned i = 0; i < dump.pair_count; ++i) {
			printf("pair=%u a=%u b=%u mode=%u active=%u quotient=%u refraction=%u energy=%u pressure=%u pulses=%u last=%u\n",
			       dump.pairs[i].pair_id, dump.pairs[i].a, dump.pairs[i].b,
			       dump.pairs[i].mode, dump.pairs[i].active,
			       dump.pairs[i].quotient_ppm, dump.pairs[i].refraction_ppm,
			       dump.pairs[i].energy_ppm, dump.pairs[i].pressure_ppm,
			       dump.pairs[i].pulses, dump.pairs[i].last_value_ppm);
		}
		return 0;
	}

	if (!strcmp(argv[1], "cluster")) {
		struct dawnhex_cluster_uapi c;
		memset(&c, 0, sizeof(c));
		if (ioctl(fd, DAWNHEX_IOC_GET_CLUSTER, &c) < 0)
			die("ioctl GET_CLUSTER");

		printf("generation=%u nodes=%u links=%u interactive=%u tick_ms=%u\n",
		       c.generation, c.node_count, c.link_count, c.interactive, c.tick_ms);

		for (int i = 0; i < DAWNHEX_MAX_NODES; ++i) {
			if (!c.nodes[i].present)
				continue;
			printf("id=%u name=%s online=%u energy=%u health=%u links=0x%016llx\n",
			       c.nodes[i].id, c.nodes[i].name, c.nodes[i].online,
			       c.nodes[i].energy_ppm, c.nodes[i].health_ppm,
			       (unsigned long long)c.nodes[i].links_mask);
		}
		return 0;
	}

	if (!strcmp(argv[1], "node")) {
		struct dawnhex_node_query q;
		if (argc < 3) {
			fprintf(stderr, "missing node id\n");
			return 1;
		}
		memset(&q, 0, sizeof(q));
		q.id = (unsigned)strtoul(argv[2], NULL, 10);

		if (ioctl(fd, DAWNHEX_IOC_GET_NODE, &q) < 0)
			die("ioctl GET_NODE");

		printf("id=%u name=%s online=%u energy=%u health=%u links=0x%016llx\n",
		       q.node.id, q.node.name, q.node.online,
		       q.node.energy_ppm, q.node.health_ppm,
		       (unsigned long long)q.node.links_mask);
		return 0;
	}

	if (!strcmp(argv[1], "step")) {
		if (ioctl(fd, DAWNHEX_IOC_STEP) < 0)
			die("ioctl STEP");
		return 0;
	}

	fprintf(stderr, "unknown command\n");
	return 1;
}
