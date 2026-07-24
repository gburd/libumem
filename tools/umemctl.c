/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * umemctl -- live-process inspection CLI for libumem (the mdb replacement).
 *
 * Connects to a running process's introspection socket (opened when that
 * process runs with UMEM_OPTIONS=introspect=1) and drives the line protocol:
 *
 *   umemctl <pid> stats
 *   umemctl <pid> caches
 *   umemctl <pid> cache <name>
 *   umemctl <pid> whatis <addr>
 *   umemctl <pid> leaks
 *   umemctl <pid> logtail                 # stream live log-like events
 *   umemctl <pid> record <out.log>        # capture the log stream to a file
 *   umemctl <pid> record --learn-leaks <set>  # phase 1: learn leaked sigs
 *   umemctl <pid> break <predicate>       # arm a break (size=/cache=/seq=/token=BREAK/leaked)
 *   umemctl <pid> break leaked --set <set> # phase 2: stop before a leaked alloc
 *   umemctl <pid> continue                # resume a stopped thread
 *   umemctl <pid> monitor [--once]        # full-screen ANSI TUI
 *
 * Dependency-free: raw AF_UNIX socket + ANSI escape codes. No ncurses.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>

static int
connect_pid(long pid)
{
	char path[108];
	const char *env = getenv("UMEM_INTROSPECT_SOCK");
	struct sockaddr_un addr;
	int fd;

	if (env != NULL && env[0] != '\0')
		snprintf(path, sizeof (path), "%s", env);
	else
		snprintf(path, sizeof (path), "/tmp/umem.%ld.sock", pid);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return (-1);
	}
	memset(&addr, 0, sizeof (addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof (addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof (addr)) < 0) {
		fprintf(stderr, "umemctl: cannot connect to %s: %s\n",
		    path, strerror(errno));
		fprintf(stderr, "  (is the target running with "
		    "UMEM_OPTIONS=introspect=1?)\n");
		close(fd);
		return (-1);
	}
	return (fd);
}

/* Send one command line, print the response up to a lone "." terminator. */
static void
one_shot(FILE *sock, const char *cmd)
{
	char line[512];
	fprintf(sock, "%s\n", cmd);
	fflush(sock);
	while (fgets(line, sizeof (line), sock) != NULL) {
		if (strcmp(line, ".\n") == 0 || strcmp(line, ".\r\n") == 0)
			break;
		fputs(line, stdout);
	}
}

/* Stream everything until the socket closes (logtail / record). If out !=
 * NULL, also copy each line to the file (record). */
static int
stream(FILE *sock, FILE *out)
{
	char line[512];
	while (fgets(line, sizeof (line), sock) != NULL) {
		fputs(line, stdout);
		fflush(stdout);
		if (out != NULL) {
			fputs(line, out);
			fflush(out);
		}
	}
	return (0);
}

/* Read "sig size=.. pc=.." lines from a set file, push them to the server. */
static int
push_leakset(FILE *sock, const char *setfile)
{
	FILE *f = fopen(setfile, "r");
	char line[512], resp[512];
	int n = 0;
	if (f == NULL) {
		fprintf(stderr, "umemctl: cannot open leak set %s: %s\n",
		    setfile, strerror(errno));
		return (-1);
	}
	while (fgets(line, sizeof (line), f) != NULL) {
		if (strncmp(line, "sig ", 4) != 0)
			continue;
		line[strcspn(line, "\r\n")] = '\0';
		fprintf(sock, "%s\n", line);
		fflush(sock);
		/* consume the ok/.\n response */
		while (fgets(resp, sizeof (resp), sock) != NULL) {
			if (strcmp(resp, ".\n") == 0)
				break;
		}
		n++;
	}
	fclose(f);
	fprintf(stderr, "umemctl: pushed %d leak signatures\n", n);
	return (n);
}

/* ---- monitor: parse "stats" into fields and render a full-screen TUI ---- */

struct stats {
	long pid, caches, rss_kb;
	unsigned long long inuse, total, slab_create, slab_destroy;
	unsigned long long depot_contention, mag_reloads;
};

static void
fetch_stats(FILE *sock, struct stats *s)
{
	char line[256], key[64];
	unsigned long long v;
	memset(s, 0, sizeof (*s));
	fprintf(sock, "stats\n");
	fflush(sock);
	while (fgets(line, sizeof (line), sock) != NULL) {
		if (strcmp(line, ".\n") == 0)
			break;
		if (sscanf(line, "%63s %llu", key, &v) != 2)
			continue;
		if (!strcmp(key, "pid")) s->pid = (long)v;
		else if (!strcmp(key, "caches")) s->caches = (long)v;
		else if (!strcmp(key, "bufs_inuse")) s->inuse = v;
		else if (!strcmp(key, "bufs_total")) s->total = v;
		else if (!strcmp(key, "slab_create")) s->slab_create = v;
		else if (!strcmp(key, "slab_destroy")) s->slab_destroy = v;
		else if (!strcmp(key, "depot_contention")) s->depot_contention = v;
		else if (!strcmp(key, "mag_reloads")) s->mag_reloads = v;
		else if (!strcmp(key, "rss_kb")) s->rss_kb = (long)v;
	}
}

/* Print the top-N caches by in-use count (a fresh "caches" query each tick). */
static void
print_top_caches(FILE *sock, int n)
{
	char line[256];
	fprintf(sock, "caches\n");
	fflush(sock);
	printf("\n  %-28s %10s %10s %10s\n", "cache", "bufsize", "inuse",
	    "total");
	printf("  ------------------------------------------------------"
	    "----------\n");
	/* ponytail: server already emits caches; we just show the first n
	 * non-header lines. Sorting by growth would need client-side state --
	 * add a --sort flag only if an operator asks. */
	int shown = 0;
	int first = 1;
	while (fgets(line, sizeof (line), sock) != NULL) {
		if (strcmp(line, ".\n") == 0)
			break;
		if (first) { first = 0; continue; }	/* skip header */
		if (shown < n) {
			char name[64]; unsigned long long bs, inuse, tot;
			unsigned flags;
			if (sscanf(line, "%63s %llu %llu %llu 0x%x",
			    name, &bs, &inuse, &tot, &flags) == 5 &&
			    inuse > 0) {
				printf("  %-28s %10llu %10llu %10llu\n",
				    name, bs, inuse, tot);
				shown++;
			}
		}
	}
}

static void
monitor(long pid, int once)
{
	for (;;) {
		int fd = connect_pid(pid);
		FILE *sock;
		struct stats s;
		time_t now = time(NULL);
		if (fd < 0)
			exit(1);
		sock = fdopen(fd, "r+");
		fetch_stats(sock, &s);

		if (!once)
			printf("\033[2J\033[H");	/* clear + home */
		printf("libumem monitor  pid=%ld  %.24s\n", s.pid,
		    ctime(&now));
		printf("=====================================================\n");
		printf("  caches            %ld\n", s.caches);
		printf("  bufs in-use       %llu\n", s.inuse);
		printf("  bufs total        %llu\n", s.total);
		printf("  slabs created     %llu\n", s.slab_create);
		printf("  slabs destroyed   %llu\n", s.slab_destroy);
		printf("  depot contention  %llu\n", s.depot_contention);
		printf("  magazine reloads  %llu\n", s.mag_reloads);
		printf("  RSS               %ld KiB\n", s.rss_kb);

		print_top_caches(sock, 12);
		fflush(stdout);
		fclose(sock);

		if (once)
			return;
		printf("\n(refresh 2 Hz -- Ctrl-C to quit)\n");
		fflush(stdout);
		usleep(500 * 1000);
	}
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: umemctl <pid> <command> [args]\n"
	    "  stats | caches | cache <name> | whatis <addr> | leaks\n"
	    "  logtail                     stream live log-like events\n"
	    "  record <out.log>            capture the log stream to a file\n"
	    "  record --learn-leaks <set>  phase 1: learn leaked signatures\n"
	    "  break <predicate>           size=<n>|cache=<n>|seq=<n>|token=BREAK|leaked\n"
	    "  break leaked --set <set>    phase 2: stop before a leaked alloc\n"
	    "  continue                    resume a stopped thread\n"
	    "  monitor [--once]            full-screen ANSI TUI\n");
}

int
main(int argc, char **argv)
{
	long pid;
	const char *cmd;
	int fd;
	FILE *sock;

	if (argc < 3) {
		usage();
		return (2);
	}
	pid = strtol(argv[1], NULL, 10);
	cmd = argv[2];

	if (strcmp(cmd, "monitor") == 0) {
		monitor(pid, argc > 3 && strcmp(argv[3], "--once") == 0);
		return (0);
	}

	fd = connect_pid(pid);
	if (fd < 0)
		return (1);
	sock = fdopen(fd, "r+");

	if (strcmp(cmd, "stats") == 0 || strcmp(cmd, "caches") == 0 ||
	    strcmp(cmd, "leaks") == 0 || strcmp(cmd, "continue") == 0) {
		one_shot(sock, cmd);
	} else if (strcmp(cmd, "cache") == 0 && argc > 3) {
		char buf[128];
		snprintf(buf, sizeof (buf), "cache %s", argv[3]);
		one_shot(sock, buf);
	} else if (strcmp(cmd, "whatis") == 0 && argc > 3) {
		char buf[128];
		snprintf(buf, sizeof (buf), "whatis %s", argv[3]);
		one_shot(sock, buf);
	} else if (strcmp(cmd, "logtail") == 0) {
		fprintf(sock, "logtail\n");
		fflush(sock);
		stream(sock, NULL);
	} else if (strcmp(cmd, "record") == 0) {
		if (argc > 4 && strcmp(argv[3], "--learn-leaks") == 0) {
			/* phase 1: dump leak signatures to the set file */
			FILE *out = fopen(argv[4], "w");
			char line[512];
			if (out == NULL) {
				perror("fopen");
				return (1);
			}
			fprintf(sock, "learn\n");
			fflush(sock);
			while (fgets(line, sizeof (line), sock) != NULL) {
				if (strcmp(line, ".\n") == 0)
					break;
				fputs(line, out);
				fputs(line, stdout);
			}
			fclose(out);
			fprintf(stderr, "umemctl: wrote leak set to %s\n",
			    argv[4]);
		} else if (argc > 3) {
			FILE *out = fopen(argv[3], "w");
			if (out == NULL) {
				perror("fopen");
				return (1);
			}
			fprintf(sock, "record\n");
			fflush(sock);
			stream(sock, out);
			fclose(out);
		} else {
			usage();
			return (2);
		}
	} else if (strcmp(cmd, "break") == 0 && argc > 3) {
		if (strcmp(argv[3], "leaked") == 0 && argc > 5 &&
		    strcmp(argv[4], "--set") == 0) {
			/* phase 2: push signatures then arm break leaked */
			if (push_leakset(sock, argv[5]) < 0)
				return (1);
			one_shot(sock, "break leaked");
		} else {
			char buf[256];
			snprintf(buf, sizeof (buf), "break %s", argv[3]);
			one_shot(sock, buf);
		}
	} else {
		usage();
		fclose(sock);
		return (2);
	}
	fclose(sock);
	return (0);
}
