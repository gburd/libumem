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
 * CDDL HEADER END
 */

/*
 * umem -- drive libumem introspection against a live pid or core.
 *
 * C reimplementation of the former tools/umem bash wrapper (same interface,
 * same behavior).  It builds a gdb batch command script that sources
 * tools/gdb/umem_gdb.py and runs the requested `umem <cmd>`, invokes gdb
 * against the target (live pid, or core+exe), and prints the output bracketed
 * by sentinels.  In --dump mode it execs the umem_dump_reader offline reader.
 *
 * Usage:
 *   umem --pid <pid>  <cmd> [args...]
 *   umem --core <core> --exe <bin> <cmd> [args...]
 *   umem --dump <file.ums>      <cmd> [args...]   # offline
 *   umem --exe  <bin> <cmd> [args...]
 *
 * Commands:
 *   findleaks [-f text|json] [-n N]
 *   log       [-f text|json] [-n N]
 *   status    [-f text|json]
 *   walk      [allocated|freed|log] [-f text|json] [-n N]
 *   whatis <addr>
 *   bufctl <addr>
 *   snapshot <path>      # path with .ums/.umsnap/.bin gets binary v2 format
 *
 * Environment:
 *   UMEM_TOOL_GDB=/path/to/gdb    override the gdb binary
 *
 * Notes:
 *   Live pid mode ptraces the target, pauses it for the duration of the
 *   query, then detaches.  Core mode is read-only.
 *
 *   For recurring automated checks (CI, monitoring), invoke with --pid
 *   and -f json and feed the output to jq.
 */

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define	SENTINEL_BEGIN	"__LIBUMEM_TOOL_BEGIN__"
#define	SENTINEL_END	"__LIBUMEM_TOOL_END__"

static const char *prog = "umem";

static void
usage(int code)
{
	fprintf(stderr,
"umem -- drive libumem introspection against a live pid or core.\n"
"\n"
"Usage:\n"
"  umem --pid <pid>  <cmd> [args...]\n"
"  umem --core <core> --exe <bin> <cmd> [args...]\n"
"  umem --dump <file.ums>      <cmd> [args...]   # offline\n"
"  umem --exe  <bin> <cmd> [args...]\n"
"\n"
"Commands:\n"
"  findleaks [-f text|json] [-n N]\n"
"  log       [-f text|json] [-n N]\n"
"  status    [-f text|json]\n"
"  walk      [allocated|freed|log] [-f text|json] [-n N]\n"
"  whatis <addr>\n"
"  bufctl <addr>\n"
"  snapshot <path>\n"
"\n"
"Environment:\n"
"  UMEM_TOOL_GDB=/path/to/gdb    override the gdb binary\n");
	exit(code);
}

static void
die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "%s: ", prog);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

/*
 * Absolute directory containing this executable, resolving symlinks, so we
 * can locate sibling files (gdb/umem_gdb.py, umem_dump_reader).  Mirrors the
 * bash `cd "$(dirname "$(readlink -f "$0")")" && pwd`.
 */
static char *
self_dir(const char *argv0)
{
	char resolved[PATH_MAX];
	char *rp = realpath("/proc/self/exe", resolved);
	if (rp == NULL) {
		/* Fallback: resolve argv0. */
		rp = realpath(argv0, resolved);
		if (rp == NULL)
			return (NULL);
	}
	/* dirname may modify its argument; operate on a copy. */
	char *copy = strdup(resolved);
	if (copy == NULL)
		return (NULL);
	char *dir = dirname(copy);
	char *out = strdup(dir);
	free(copy);
	return (out);
}

static int
is_cmd(const char *s)
{
	static const char *cmds[] = {
		"findleaks", "log", "status", "whatis",
		"snapshot", "bufctl", "walk", NULL
	};
	for (int i = 0; cmds[i] != NULL; i++)
		if (strcmp(s, cmds[i]) == 0)
			return (1);
	return (0);
}

/* Build the "umem <cmd> ..." gdb command line from the subcommand + args. */
static void
build_gdb_cmd(char *out, size_t outsz, const char *cmd,
    char **args, int nargs)
{
	if (strcmp(cmd, "findleaks") == 0 || strcmp(cmd, "log") == 0) {
		const char *fmt = "text";
		const char *n = (strcmp(cmd, "findleaks") == 0) ? "50" : "0";
		for (int i = 0; i < nargs; ) {
			if ((strcmp(args[i], "-f") == 0 ||
			    strcmp(args[i], "--format") == 0) && i + 1 < nargs) {
				fmt = args[i + 1]; i += 2;
			} else if (strcmp(args[i], "-n") == 0 && i + 1 < nargs) {
				n = args[i + 1]; i += 2;
			} else {
				die("unknown flag %s", args[i]);
			}
		}
		snprintf(out, outsz, "umem %s -f %s -n %s", cmd, fmt, n);
	} else if (strcmp(cmd, "status") == 0) {
		const char *fmt = "text";
		for (int i = 0; i < nargs; ) {
			if ((strcmp(args[i], "-f") == 0 ||
			    strcmp(args[i], "--format") == 0) && i + 1 < nargs) {
				fmt = args[i + 1]; i += 2;
			} else {
				die("unknown flag %s", args[i]);
			}
		}
		snprintf(out, outsz, "umem status -f %s", fmt);
	} else if (strcmp(cmd, "whatis") == 0 || strcmp(cmd, "bufctl") == 0) {
		if (nargs != 1)
			die("%s needs exactly one address", cmd);
		/* bash mapped both to `umem whatis <addr>` */
		snprintf(out, outsz, "umem whatis %s", args[0]);
	} else if (strcmp(cmd, "snapshot") == 0) {
		if (nargs != 1)
			die("snapshot needs exactly one path");
		snprintf(out, outsz, "umem snapshot %s", args[0]);
	} else if (strcmp(cmd, "walk") == 0) {
		const char *kind = "allocated";
		const char *fmt = "text";
		const char *n = "0";
		for (int i = 0; i < nargs; ) {
			if (strcmp(args[i], "allocated") == 0 ||
			    strcmp(args[i], "freed") == 0 ||
			    strcmp(args[i], "log") == 0) {
				kind = args[i]; i += 1;
			} else if ((strcmp(args[i], "-f") == 0 ||
			    strcmp(args[i], "--format") == 0) && i + 1 < nargs) {
				fmt = args[i + 1]; i += 2;
			} else if (strcmp(args[i], "-n") == 0 && i + 1 < nargs) {
				n = args[i + 1]; i += 2;
			} else {
				die("unknown flag %s", args[i]);
			}
		}
		snprintf(out, outsz, "umem walk %s -f %s -n %s", kind, fmt, n);
	} else {
		die("unknown command: %s", cmd);
	}
}

/* Filter gdb stdout, printing only lines strictly between the sentinels. */
static int
run_gdb_filtered(char **gdb_argv)
{
	int pipefd[2];
	if (pipe(pipefd) != 0)
		die("pipe: %s", strerror(errno));

	pid_t child = fork();
	if (child < 0)
		die("fork: %s", strerror(errno));

	if (child == 0) {
		/* Child: stdout -> pipe, stderr -> /dev/null (bash: 2>/dev/null) */
		dup2(pipefd[1], STDOUT_FILENO);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
		close(pipefd[0]);
		close(pipefd[1]);
		execvp(gdb_argv[0], gdb_argv);
		/* execvp failed */
		_exit(127);
	}

	/* Parent: read child stdout, filter between sentinels. */
	close(pipefd[1]);
	FILE *in = fdopen(pipefd[0], "r");
	if (in == NULL)
		die("fdopen: %s", strerror(errno));

	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	int started = 0;
	while ((len = getline(&line, &cap, in)) != -1) {
		/* strip trailing newline for sentinel comparison */
		char *nl = strchr(line, '\n');
		size_t cmplen = (nl != NULL) ? (size_t)(nl - line) : (size_t)len;
		if (cmplen == strlen(SENTINEL_BEGIN) &&
		    strncmp(line, SENTINEL_BEGIN, cmplen) == 0) {
			started = 1;
			continue;
		}
		if (cmplen == strlen(SENTINEL_END) &&
		    strncmp(line, SENTINEL_END, cmplen) == 0) {
			started = 0;
			continue;
		}
		if (started)
			fputs(line, stdout);
	}
	free(line);
	fclose(in);

	int status;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR)
		;
	if (gdb_argv[0] != NULL && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 127)
		die("failed to exec gdb (%s); set UMEM_TOOL_GDB",
		    gdb_argv[0]);
	return (0);
}

int
main(int argc, char **argv)
{
	const char *pid = NULL, *core = NULL, *exe = NULL, *dump = NULL;
	const char *cmd = NULL;
	char **args = NULL;
	int nargs = 0;

	int i = 1;
	for (; i < argc; i++) {
		if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
			pid = argv[++i];
		} else if (strcmp(argv[i], "--core") == 0 && i + 1 < argc) {
			core = argv[++i];
		} else if (strcmp(argv[i], "--exe") == 0 && i + 1 < argc) {
			exe = argv[++i];
		} else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
			dump = argv[++i];
		} else if (strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0) {
			usage(0);
		} else if (is_cmd(argv[i])) {
			cmd = argv[i];
			args = &argv[i + 1];
			nargs = argc - (i + 1);
			break;
		} else {
			fprintf(stderr, "%s: unknown argument: %s\n",
			    prog, argv[i]);
			usage(2);
		}
	}

	if (cmd == NULL)
		die("no command given");

	char *dir = self_dir(argv[0]);
	if (dir == NULL)
		die("cannot determine own directory");

	/* Offline mode: exec the python dump reader. */
	if (dump != NULL) {
		if (pid != NULL || core != NULL)
			die("--dump cannot combine with --pid/--core");
		char reader[PATH_MAX];
		snprintf(reader, sizeof (reader), "%s/umem_dump_reader", dir);
		if (access(reader, X_OK) != 0)
			die("cannot find %s", reader);
		/* argv: reader --dump <dump> <cmd> [args...] */
		char **ra = calloc((size_t)nargs + 5, sizeof (char *));
		if (ra == NULL)
			die("out of memory");
		int k = 0;
		ra[k++] = reader;
		ra[k++] = "--dump";
		ra[k++] = (char *)dump;
		ra[k++] = (char *)cmd;
		for (int j = 0; j < nargs; j++)
			ra[k++] = args[j];
		ra[k] = NULL;
		execv(reader, ra);
		die("failed to exec %s: %s", reader, strerror(errno));
	}

	if (pid != NULL && core != NULL)
		die("--pid and --core are mutually exclusive");
	if (pid == NULL && core == NULL) {
		fprintf(stderr, "%s: need --pid or --core\n", prog);
		usage(2);
	}

	char gdb_py[PATH_MAX];
	snprintf(gdb_py, sizeof (gdb_py), "%s/gdb/umem_gdb.py", dir);
	if (access(gdb_py, R_OK) != 0)
		die("cannot find %s", gdb_py);

	char gdb_cmd[4096];
	build_gdb_cmd(gdb_cmd, sizeof (gdb_cmd), cmd, args, nargs);

	/* Write the gdb command script to a temp file. */
	char tmpl[] = "/tmp/umem_tool_XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0)
		die("mkstemp: %s", strerror(errno));
	FILE *tf = fdopen(fd, "w");
	if (tf == NULL)
		die("fdopen: %s", strerror(errno));
	fprintf(tf,
	    "set pagination off\n"
	    "set confirm off\n"
	    "set print elements 0\n"
	    "set print inferior-events off\n"
	    "set print thread-events off\n"
	    "source %s\n"
	    "echo %s\\n\n"
	    "%s\n"
	    "echo %s\\n\n",
	    gdb_py, SENTINEL_BEGIN, gdb_cmd, SENTINEL_END);
	fclose(tf);

	const char *gdb = getenv("UMEM_TOOL_GDB");
	if (gdb == NULL || gdb[0] == '\0')
		gdb = "gdb";

	/*
	 * Base gdb args (batch, quiet, auto-load/debuginfod tuning applied
	 * before -p so they affect libthread_db resolution at attach).
	 */
	char *base[] = {
		(char *)gdb, "--batch", "--quiet",
		"-iex", "set auto-load safe-path /",
		"-iex", "set debuginfod enabled off",
		"-iex", "set print inferior-events off",
		"-iex", "set print thread-events off",
		NULL
	};
	int nbase = 0;
	while (base[nbase] != NULL)
		nbase++;

	/* Assemble the final argv. */
	char *gdb_argv[32];
	int g = 0;
	for (int j = 0; j < nbase; j++)
		gdb_argv[g++] = base[j];

	if (pid != NULL) {
		if (exe != NULL)
			gdb_argv[g++] = (char *)exe;
		gdb_argv[g++] = "-p";
		gdb_argv[g++] = (char *)pid;
		gdb_argv[g++] = "-x";
		gdb_argv[g++] = tmpl;
	} else { /* core */
		if (exe == NULL)
			die("--core requires --exe");
		gdb_argv[g++] = (char *)exe;
		char coreflag[PATH_MAX + 8];
		snprintf(coreflag, sizeof (coreflag), "--core=%s", core);
		/* need a persistent copy */
		gdb_argv[g++] = strdup(coreflag);
		gdb_argv[g++] = "-x";
		gdb_argv[g++] = tmpl;
	}
	gdb_argv[g] = NULL;

	int rc = run_gdb_filtered(gdb_argv);

	unlink(tmpl);
	free(dir);
	return (rc);
}
