/*
 * hermes_log file-sink unit tests
 *
 * Regression guard for a silent -L failure: hermes_log_set_file() must return
 * -1 AND name the offending path on stderr when the file cannot be opened.
 * Before this, an operator who ran `mercury -v -L session.log` with a bad or
 * unwritable path got no file and no warning, so there was nothing to send
 * when something went wrong.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "hermes_log.h"

/* Run hermes_log_set_file() with fd 2 redirected to a scratch file and copy
 * what it wrote into `out`.  Restores stderr before returning, so a failed
 * assertion below still reaches the test output.  Returns -2 if the capture
 * plumbing itself failed. */
static int set_file_capturing_stderr(const char *path, char *out, size_t outlen)
{
    char tmpl[] = "/tmp/mercury_stderr_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0)
    {
        if (outlen) out[0] = '\0';
        return -2;
    }

    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    if (saved < 0 || dup2(fd, STDERR_FILENO) < 0)
    {
        if (saved >= 0) close(saved);
        close(fd);
        unlink(tmpl);
        if (outlen) out[0] = '\0';
        return -2;
    }

    int rc = hermes_log_set_file(path, HERMES_LOG_LEVEL_DEBUG, false);

    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);

    lseek(fd, 0, SEEK_SET);
    ssize_t n = outlen ? read(fd, out, outlen - 1) : 0;
    if (n < 0) n = 0;
    if (outlen) out[(size_t)n] = '\0';
    close(fd);
    unlink(tmpl);
    return rc;
}

void setUp(void) {}
void tearDown(void) {}

/* A NULL path is rejected quietly; there is nothing useful to name. */
void test_set_file_null_path_is_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(0, hermes_log_init(64));
    TEST_ASSERT_EQUAL_INT(-1, hermes_log_set_file(NULL, HERMES_LOG_LEVEL_DEBUG, false));
    hermes_log_shutdown();
}

/* An unopenable path reports the path and the reason on stderr -- the fix. */
void test_set_file_unopenable_path_warns_on_stderr(void)
{
    const char *bad = "/nonexistent-mercury-test-dir/session.log";
    char msg[512];

    TEST_ASSERT_EQUAL_INT(0, hermes_log_init(64));
    TEST_ASSERT_EQUAL_INT(-1, set_file_capturing_stderr(bad, msg, sizeof(msg)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(msg, bad), "warning does not name the path");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(msg, "cannot open"),
                                 "warning does not say the file could not be opened");
    hermes_log_shutdown();
}

/* The happy path still works end to end: an entry logged after set_file()
 * lands in the file. */
void test_set_file_records_entries(void)
{
    char tmpl[] = "/tmp/mercury_log_XXXXXX";
    int fd = mkstemp(tmpl);
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);

    TEST_ASSERT_EQUAL_INT(0, hermes_log_init(64));
    TEST_ASSERT_EQUAL_INT(0, hermes_log_set_file(tmpl, HERMES_LOG_LEVEL_DEBUG, false));

    /* Keep INFO lines off stderr so this test does not clutter the run; they
     * still reach the file, whose sink is at DEBUG. */
    hermes_log_set_level(HERMES_LOG_LEVEL_ERROR);
    HLOGI("test", "hello-from-hermes-log");
    hermes_log_shutdown();

    FILE *fp = fopen(tmpl, "r");
    TEST_ASSERT_NOT_NULL(fp);
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    unlink(tmpl);

    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "hello-from-hermes-log"),
                                 "logged entry missing from the file sink");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_set_file_null_path_is_rejected);
    RUN_TEST(test_set_file_unopenable_path_warns_on_stderr);
    RUN_TEST(test_set_file_records_entries);
    return UNITY_END();
}
