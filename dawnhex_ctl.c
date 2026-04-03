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
