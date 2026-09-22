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
 *   umem --dump <file.ums>      <cmd> [args...]   # offline
 *
 * --core is ACCEPTED BUT REFUSED: see the rejection in main().  Every command
 * runs by calling umem_inspect(3) entry points inside the target, and a core
 * file has no process to call into.  It used to exit 0 printing nothing.
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
 *   query, then detaches.
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
"umem -- drive libumem introspection against a live pid or a snapshot.\n"
"\n"
"Usage:\n"
"  umem --pid <pid>  <cmd> [args...]\n"
"  umem --dump <file.ums>      <cmd> [args...]   # offline\n"
"\n"
"  --core <core> --exe <bin>   NOT SUPPORTED; refused with an explanation.\n"
"                              Use --dump with a snapshot taken while the\n"
"                              process was alive.\n"
"\n"
"Commands:\n"
"  findleaks [-f text|json] [-n N]   outstanding allocations by stack\n"
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

/*
 * Reject an argument that cannot be safely interpolated into a gdb command
 * file.  The file is line-oriented, so a newline in an argument injects a new
 * gdb command -- and gdb commands include `shell`.  With an untrusted binary
 * path or argument that is arbitrary code execution.
 *
 * Whitelist rather than escape: these arguments are addresses, format names,
 * counts and paths.  Nothing legitimate needs a control character, a quote, a
 * backslash, a dollar or a backtick.
 */
static void
check_gdb_safe(const char *what, const char *s)
{
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p < 0x20 || *p == 0x7f)
			die("%s contains a control character (0x%02x); "
			    "refusing to build a gdb command from it",
			    what, *p);
		if (strchr("\"\\$`", *p) != NULL)
			die("%s contains %c, which cannot be safely passed "
			    "to gdb", what, *p);
	}
}

/* Build the "umem <cmd> ..." gdb command line from the subcommand + args. */
static void
build_gdb_cmd(char *out, size_t outsz, const char *cmd,
    char **args, int nargs)
{
	/* Everything below is interpolated into the gdb command file. */
	for (int i = 0; i < nargs; i++)
		check_gdb_safe("argument", args[i]);
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

/*
 * Filter gdb stdout, printing only lines strictly between the sentinels.
 *
 * Returns 0 only if gdb ran, exited 0, AND produced a sentinel-delimited
 * report.  Anything else is a failure: this used to discard stderr and treat
 * every exit status except 127 as success, so a gdb that could not attach, or
 * could not call into the target at all (core files), exited 0 here while
 * printing nothing -- indistinguishable from "nothing found".
 */
static int
run_gdb_filtered(char **gdb_argv)
{
	int pipefd[2];
	int errfd[2];
	if (pipe(pipefd) != 0)
		die("pipe: %s", strerror(errno));
	if (pipe(errfd) != 0)
		die("pipe: %s", strerror(errno));

	pid_t child = fork();
	if (child < 0)
		die("fork: %s", strerror(errno));

	if (child == 0) {
		/* Child: stdout and stderr each to their own pipe.  stderr is
		 * CAPTURED, not discarded -- it is where gdb explains why it
		 * could not do what was asked. */
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(errfd[1], STDERR_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		close(errfd[0]);
		close(errfd[1]);
		execvp(gdb_argv[0], gdb_argv);
		/* execvp failed */
		_exit(127);
	}

	/* Parent: read child stdout, filter between sentinels. */
	close(pipefd[1]);
	close(errfd[1]);
	FILE *in = fdopen(pipefd[0], "r");
	if (in == NULL)
		die("fdopen: %s", strerror(errno));

	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	int started = 0;
	int saw_begin = 0;
	int saw_end = 0;
	size_t body_lines = 0;
	while ((len = getline(&line, &cap, in)) != -1) {
		/* strip trailing newline for sentinel comparison */
		char *nl = strchr(line, '\n');
		size_t cmplen = (nl != NULL) ? (size_t)(nl - line) : (size_t)len;
		if (cmplen == strlen(SENTINEL_BEGIN) &&
		    strncmp(line, SENTINEL_BEGIN, cmplen) == 0) {
			started = 1;
			saw_begin = 1;
			continue;
		}
		if (cmplen == strlen(SENTINEL_END) &&
		    strncmp(line, SENTINEL_END, cmplen) == 0) {
			started = 0;
			saw_end = 1;
			continue;
		}
		if (started) {
			fputs(line, stdout);
			body_lines++;
		}
	}
	free(line);
	fclose(in);

	/* Drain gdb's stderr so we can report it on failure. */
	char errbuf[4096];
	size_t errlen = 0;
	for (;;) {
		ssize_t r = read(errfd[0], errbuf + errlen,
		    sizeof (errbuf) - 1 - errlen);
		if (r <= 0)
			break;
		errlen += (size_t)r;
		if (errlen >= sizeof (errbuf) - 1)
			break;
	}
	errbuf[errlen] = '\0';
	close(errfd[0]);

	int status;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR)
		;

	if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
		die("failed to exec gdb (%s); set UMEM_TOOL_GDB",
		    gdb_argv[0]);

	if (WIFSIGNALED(status)) {
		fprintf(stderr, "%s: gdb was killed by signal %d\n",
		    prog, WTERMSIG(status));
		if (errlen > 0)
			fprintf(stderr, "--- gdb stderr ---\n%s", errbuf);
		return (2);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "%s: gdb exited %d\n", prog,
		    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		if (errlen > 0)
			fprintf(stderr, "--- gdb stderr ---\n%s", errbuf);
		return (2);
	}

	/*
	 * gdb exited 0 but produced no report.  This is the --core case: every
	 * command is implemented by CALLING umem_inspect(3) entry points inside
	 * the target, and a core file has no process to run them, so the
	 * command silently yields nothing.  Fail loudly instead of exiting 0
	 * with empty output.
	 */
	if (!saw_begin || !saw_end || body_lines == 0) {
		fprintf(stderr,
		    "%s: gdb produced no report (%s).\n", prog,
		    !saw_begin ? "the command never started" :
		    !saw_end ? "the command did not complete" :
		    "the command produced no output");
		if (errlen > 0)
			fprintf(stderr, "--- gdb stderr ---\n%s", errbuf);
		return (2);
	}

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

	/*
	 * --core CANNOT WORK, and must say so rather than exiting 0 with no
	 * output.
	 *
	 * Every command is implemented by CALLING umem_inspect(3) entry points
	 * inside the target (tools/gdb/umem_gdb.py uses gdb's expression
	 * evaluator).  A core file is a memory image with no process, so there
	 * is nothing to call.  Verified 2026-09-21: the same command reported
	 * 200 outstanding buffers against a live process and zero bytes with
	 * exit status 0 against that process's own core
	 * (docs/results/2026-09-21-core-mode-produces-no-report.log).
	 *
	 * Making this work needs a passive reader that parses allocator
	 * structures out of the core's memory image; that is not implemented.
	 * Refusing up front beats silently reporting "no leaks found" from a
	 * mode that cannot find any.
	 */
	if (core != NULL) {
		fprintf(stderr,
"%s: --core is not supported and cannot be made to work as implemented.\n"
"\n"
"  Every command is executed by CALLING libumem's umem_inspect(3) entry\n"
"  points inside the target process.  A core file has no process, so the\n"
"  calls cannot run and the report would be empty -- indistinguishable\n"
"  from \"nothing found\".  This used to exit 0 with no output; it now\n"
"  fails instead of lying.\n"
"\n"
"  For post-mortem analysis, take a snapshot while the process is alive:\n"
"\n"
"      /* in the target, or from gdb against the live process */\n"
"      umem_inspect_snapshot(\"/tmp/state.ums\");\n"
"\n"
"      $ umem --dump /tmp/state.ums findleaks\n"
"\n"
"  See umem(1) MODES and tools/DEBUGGING.md.\n", prog);
		return (2);
	}

	char gdb_py[PATH_MAX];
	snprintf(gdb_py, sizeof (gdb_py), "%s/gdb/umem_gdb.py", dir);
	if (access(gdb_py, R_OK) != 0)
		die("cannot find %s", gdb_py);
	/* Interpolated into the command file; so is the helper's own path. */
	check_gdb_safe("the umem_gdb.py path", gdb_py);
	if (exe != NULL)
		check_gdb_safe("--exe", exe);
	if (core != NULL)
		check_gdb_safe("--core", core);
	if (pid != NULL)
		check_gdb_safe("--pid", pid);

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
	 *
	 * auto-load safe-path is set to OUR OWN helper directory, not "/".
	 * "/" disables gdb's protection against executable-associated scripts
	 * entirely: attaching to an untrusted binary would then auto-load and
	 * execute any -gdb.py sitting beside it.  We only need our own
	 * umem_gdb.py to be loadable, and that is what this permits.  The
	 * script is passed explicitly with -x, which does not consult
	 * safe-path, so this is belt-and-braces for the auto-load case.
	 */
	char safe_path[PATH_MAX + 32];
	snprintf(safe_path, sizeof (safe_path), "set auto-load safe-path %s",
	    dir);

	char *base[] = {
		(char *)gdb, "--batch", "--quiet",
		"-iex", safe_path,
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
