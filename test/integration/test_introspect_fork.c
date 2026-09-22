/*
 * test/integration/test_introspect_fork.c -- Phase 3 item 5 (clause B5).
 *
 * A fork child must not inherit an ARMED introspection break predicate.
 *
 * The child inherits the predicate state and the satisfied pthread_once, but
 * NOT the server thread that processes "continue".  So pre-fix, an armed child
 * stopped on its next matching allocation and waited forever on a condvar that
 * nothing in the child could ever signal -- and it could not even start a new
 * server, because the once-control was already satisfied.
 *
 * This test arms a predicate directly through the control channel, forks, and
 * has the child perform a matching allocation.  The fix
 * (umem_introspect_fork_child(), called from umem_fork.c's child handler)
 * disarms in the child, so the allocation completes.
 *
 * Build/run only makes sense under --enable-introspect; without it there is no
 * predicate to inherit and the test reports SKIP (automake status 77).
 */

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "umem.h"

#ifndef UMEM_INTROSPECT

int
main(void)
{
	printf("SKIP: built without --enable-introspect; "
	    "no break predicate can be inherited\n");
	return (77);
}

#else /* UMEM_INTROSPECT */

/* Send one command line to the control socket and read the terminator. */
static int
ctl(const char *path, const char *cmd)
{
	struct sockaddr_un addr;
	char buf[512];
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (fd < 0)
		return (-1);
	memset(&addr, 0, sizeof (addr));
	addr.sun_family = AF_UNIX;
	(void) strncpy(addr.sun_path, path, sizeof (addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof (addr)) != 0) {
		(void) close(fd);
		return (-1);
	}
	if (write(fd, cmd, strlen(cmd)) < 0) {
		(void) close(fd);
		return (-1);
	}
	/* Drain the response so the server finishes the command. */
	(void) read(fd, buf, sizeof (buf));
	(void) close(fd);
	return (0);
}

int
main(void)
{
	const char *sock = getenv("UMEM_INTROSPECT_SOCK");
	pid_t pid;
	int status;

	if (sock == NULL || sock[0] == '\0') {
		printf("SKIP: UMEM_INTROSPECT_SOCK not set\n");
		return (77);
	}
	if (getenv("UMEM_OPTIONS") == NULL) {
		printf("SKIP: run with UMEM_OPTIONS=introspect=1 so this "
		    "process has its own control channel\n");
		return (77);
	}

	/* Touch the allocator so the library initialises and the channel's
	 * server thread starts. */
	void *warm = umem_alloc(4096, UMEM_DEFAULT);
	if (warm != NULL)
		umem_free(warm, 4096);

	/* Give the lazily-spawned server thread a moment to bind. */
	for (int i = 0; i < 50; i++) {
		if (access(sock, F_OK) == 0)
			break;
		usleep(100 * 1000);
	}

	/*
	 * Arm a predicate that the child is guaranteed to hit.  THIS process
	 * stays armed across the fork: that is the condition under test.
	 */
	if (ctl(sock, "break size=4096\n") != 0) {
		printf("SKIP: could not reach the control socket at %s (%s)\n",
		    sock, strerror(errno));
		return (77);
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return (1);
	}

	if (pid == 0) {
		/*
		 * Child.  Pre-fix this allocation matches the inherited
		 * predicate and blocks forever: there is no server thread here
		 * to accept a "continue".  Post-fix the predicate was disarmed
		 * by umem_introspect_fork_child().
		 */
		void *p = umem_alloc(4096, UMEM_DEFAULT);
		if (p != NULL)
			umem_free(p, 4096);
		_exit(0);
	}

	/* Parent: the child must finish on its own.  The caller's timeout is
	 * the real assertion -- a hung child never reaches _exit. */
	if (waitpid(pid, &status, 0) < 0) {
		perror("waitpid");
		return (1);
	}

	/* Release the parent's own predicate so we do not stop on exit. */
	(void) ctl(sock, "continue\n");

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr,
		    "FAIL: child did not exit cleanly (status 0x%x)\n", status);
		return (1);
	}

	printf("B5: fork child completed a predicate-matching allocation "
	    "without stopping\n");
	return (0);
}

#endif /* UMEM_INTROSPECT */
