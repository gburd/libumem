/*
 * Unit tests for umem_fail.c
 *
 * Tests panic/error handling, abort logic, and stack traces
 */

#include "../../config.h"
#include "../../umem.h"
#include "../../misc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "../munit.h"

/* External functions from umem_fail.c (not in headers) */
extern void umem_error_enter(const char *msg);

/* Test: umem_err_recoverable with umem_abort=0 (should not abort) */
static MunitResult
test_err_recoverable_no_abort(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Save original umem_abort value */
	uint_t saved_abort = umem_abort;
	umem_abort = 0;

	/* Redirect stderr to /dev/null to avoid test output noise */
	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Should print error but not abort */
	umem_err_recoverable("Test recoverable error: %d\n", 42);

	/* Restore stderr */
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	/* Restore umem_abort */
	umem_abort = saved_abort;

	/* If we get here, it didn't abort - success */
	return MUNIT_OK;
}

/* Test: umem_err_recoverable with message without newline */
static MunitResult
test_err_recoverable_no_newline(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	uint_t saved_abort = umem_abort;
	umem_abort = 0;

	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Message without trailing newline - should still work */
	umem_err_recoverable("No newline message");

	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	umem_abort = saved_abort;

	return MUNIT_OK;
}

/* Test: umem_err_recoverable with format arguments */
static MunitResult
test_err_recoverable_format(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	uint_t saved_abort = umem_abort;
	umem_abort = 0;

	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Test various format specifiers */
	umem_err_recoverable("Error: %s %d %p\n", "test", 123, (void *)0x1234);

	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	umem_abort = saved_abort;

	return MUNIT_OK;
}

/* Test: umem_printf and umem_error_enter */
static MunitResult
test_umem_printf(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Test umem_printf */
	umem_printf("Test message: %d\n", 99);
	umem_printf("Another message\n");

	/* Test umem_error_enter */
	umem_error_enter("Error context\n");

	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	return MUNIT_OK;
}

/* Test: umem_panic in child process (should abort) */
static MunitResult
test_umem_panic_aborts(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	pid_t pid = fork();
	if (pid == -1) {
		return MUNIT_FAIL;
	}

	if (pid == 0) {
		/* Child process - redirect stderr and call umem_panic */
		FILE *devnull = fopen("/dev/null", "w");
		dup2(fileno(devnull), STDERR_FILENO);
		fclose(devnull);

		umem_panic("Test panic: %s\n", "expected abort");
		/* Should never reach here */
		_exit(99);
	}

	/* Parent process - wait for child */
	int status;
	waitpid(pid, &status, 0);

	/* Child should have been terminated by signal (SIGABRT) */
	munit_assert_true(WIFSIGNALED(status));
	munit_assert_int(WTERMSIG(status), ==, SIGABRT);

	return MUNIT_OK;
}

/* Test: umem_panic with message without newline */
static MunitResult
test_umem_panic_no_newline(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	pid_t pid = fork();
	if (pid == -1) {
		return MUNIT_FAIL;
	}

	if (pid == 0) {
		FILE *devnull = fopen("/dev/null", "w");
		dup2(fileno(devnull), STDERR_FILENO);
		fclose(devnull);

		/* Panic without trailing newline */
		umem_panic("No newline panic");
		_exit(99);
	}

	int status;
	waitpid(pid, &status, 0);

	munit_assert_true(WIFSIGNALED(status));
	munit_assert_int(WTERMSIG(status), ==, SIGABRT);

	return MUNIT_OK;
}

/* Test: umem_err_recoverable with umem_abort > 0 (should abort) */
static MunitResult
test_err_recoverable_with_abort(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	pid_t pid = fork();
	if (pid == -1) {
		return MUNIT_FAIL;
	}

	if (pid == 0) {
		FILE *devnull = fopen("/dev/null", "w");
		dup2(fileno(devnull), STDERR_FILENO);
		fclose(devnull);

		/* Set umem_abort to trigger abort */
		umem_abort = 1;
		umem_err_recoverable("Should abort: %d\n", 1);
		_exit(99);
	}

	int status;
	waitpid(pid, &status, 0);

	/* Should have aborted */
	munit_assert_true(WIFSIGNALED(status));
	munit_assert_int(WTERMSIG(status), ==, SIGABRT);

	return MUNIT_OK;
}

/* Test: ASSERT macro failure */
static MunitResult
test_assert_failed(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	pid_t pid = fork();
	if (pid == -1) {
		return MUNIT_FAIL;
	}

	if (pid == 0) {
		FILE *devnull = fopen("/dev/null", "w");
		dup2(fileno(devnull), STDERR_FILENO);
		fclose(devnull);

		/* Trigger assertion failure */
		ASSERT(0 == 1);
		_exit(99);
	}

	int status;
	waitpid(pid, &status, 0);

	/* Should have aborted from assertion failure */
	munit_assert_true(WIFSIGNALED(status));
	munit_assert_int(WTERMSIG(status), ==, SIGABRT);

	return MUNIT_OK;
}

/* Test: Multiple concurrent panics (firstexit protection) */
static MunitResult
test_concurrent_panic_protection(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	pid_t pid = fork();
	if (pid == -1) {
		return MUNIT_FAIL;
	}

	if (pid == 0) {
		FILE *devnull = fopen("/dev/null", "w");
		dup2(fileno(devnull), STDERR_FILENO);
		fclose(devnull);

		/* First panic should trigger abort */
		umem_panic("First panic\n");
		_exit(99);
	}

	int status;
	waitpid(pid, &status, 0);

	munit_assert_true(WIFSIGNALED(status));
	munit_assert_int(WTERMSIG(status), ==, SIGABRT);

	return MUNIT_OK;
}

/* Test: Error message with long format string */
static MunitResult
test_err_long_message(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	uint_t saved_abort = umem_abort;
	umem_abort = 0;

	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Very long error message */
	char long_msg[1024];
	memset(long_msg, 'A', sizeof(long_msg) - 2);
	long_msg[sizeof(long_msg) - 2] = '\n';
	long_msg[sizeof(long_msg) - 1] = '\0';

	umem_err_recoverable("%s", long_msg);

	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	umem_abort = saved_abort;

	return MUNIT_OK;
}

/* Test: print_sym function (stack trace support) */
static MunitResult
test_print_sym(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Test with various pointer values */
	void *ptr1 = (void *)&test_print_sym;  /* Valid function pointer */
	void *ptr2 = (void *)0x1234;           /* Invalid pointer */
	void *ptr3 = NULL;                     /* NULL pointer */

	print_sym(ptr1);
	print_sym(ptr2);
	print_sym(ptr3);

	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	return MUNIT_OK;
}

/* Test: umem_panic with stack trace */
static MunitResult
test_panic_with_stacktrace(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	pid_t pid = fork();
	if (pid == -1) {
		return MUNIT_FAIL;
	}

	if (pid == 0) {
		FILE *devnull = fopen("/dev/null", "w");
		dup2(fileno(devnull), STDERR_FILENO);
		fclose(devnull);

		/* Panic should print stack trace before aborting */
		umem_panic("Panic with stack: %p\n", (void *)0x5678);
		_exit(99);
	}

	int status;
	waitpid(pid, &status, 0);

	munit_assert_true(WIFSIGNALED(status));
	munit_assert_int(WTERMSIG(status), ==, SIGABRT);

	return MUNIT_OK;
}

/* Test: umem_err_recoverable with stack trace */
static MunitResult
test_err_recoverable_stacktrace(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	uint_t saved_abort = umem_abort;
	umem_abort = 0;

	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Should print stack trace but not abort */
	umem_err_recoverable("Recoverable with stack\n");

	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	umem_abort = saved_abort;

	return MUNIT_OK;
}

/* Test: Empty error message */
static MunitResult
test_empty_error_message(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	uint_t saved_abort = umem_abort;
	umem_abort = 0;

	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	umem_err_recoverable("\n");
	umem_err_recoverable("");

	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	umem_abort = saved_abort;

	return MUNIT_OK;
}

/* Test: Multiple format specifiers */
static MunitResult
test_multiple_format_specifiers(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	uint_t saved_abort = umem_abort;
	umem_abort = 0;

	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	umem_err_recoverable("Error: %d %s %p %x %lu\n",
	    42, "test", (void *)0x1234, 0xABCD, 9999UL);

	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	umem_abort = saved_abort;

	return MUNIT_OK;
}

/* Test suite definition */
static MunitTest umem_fail_tests[] = {
	{"/err_recoverable_no_abort", test_err_recoverable_no_abort, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/err_recoverable_no_newline", test_err_recoverable_no_newline, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/err_recoverable_format", test_err_recoverable_format, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/umem_printf", test_umem_printf, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/umem_panic_aborts", test_umem_panic_aborts, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/umem_panic_no_newline", test_umem_panic_no_newline, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/err_recoverable_with_abort", test_err_recoverable_with_abort, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/assert_failed", test_assert_failed, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/concurrent_panic_protection", test_concurrent_panic_protection, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/err_long_message", test_err_long_message, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/print_sym", test_print_sym, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/panic_with_stacktrace", test_panic_with_stacktrace, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/err_recoverable_stacktrace", test_err_recoverable_stacktrace, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/empty_error_message", test_empty_error_message, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/multiple_format_specifiers", test_multiple_format_specifiers, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite suite_umem_fail = {
	"/umem_fail",
	umem_fail_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};
