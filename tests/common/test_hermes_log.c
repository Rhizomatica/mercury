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
#include <stdbool.h>
#include <sys/stat.h>

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
    TEST_ASSERT_NULL(hermes_log_file_status());
    hermes_log_shutdown();
}

/* An unopenable path reports the path and the reason on stderr, and leaves the
 * same message in the status the GUI reads (its stderr is invisible). */
void test_set_file_unopenable_path_warns_on_stderr(void)
{
    const char *bad = "/nonexistent-mercury-test-dir/session.log";
    char msg[512];

    TEST_ASSERT_EQUAL_INT(0, hermes_log_init(64));
    TEST_ASSERT_EQUAL_INT(-1, set_file_capturing_stderr(bad, msg, sizeof(msg)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(msg, bad), "warning does not name the path");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(msg, "cannot open"),
                                 "warning does not say the file could not be opened");

    const char *status = hermes_log_file_status();
    TEST_ASSERT_NOT_NULL(status);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(status, bad),
                                 "status does not name the path");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(status, "cannot open"),
                                 "status does not say the file could not be opened");
    hermes_log_shutdown();
}

/* The happy path still works end to end: an entry logged after set_file()
 * lands in the file, and the status names the (absolute) file. */
void test_set_file_records_entries(void)
{
    char tmpl[] = "/tmp/mercury_log_XXXXXX";
    int fd = mkstemp(tmpl);
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);

    TEST_ASSERT_EQUAL_INT(0, hermes_log_init(64));
    TEST_ASSERT_EQUAL_INT(0, hermes_log_set_file(tmpl, HERMES_LOG_LEVEL_DEBUG, false));

    /* The status must carry an absolute path: a relative -L that resolved
     * somewhere unexpected is only diagnosable if we say where it went. */
    const char *status = hermes_log_file_status();
    TEST_ASSERT_NOT_NULL(status);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(status, "writing log to"),
                                 "status does not say the log was opened");
    TEST_ASSERT_EQUAL_INT_MESSAGE('/', status[strlen("writing log to ")],
                                 "reported log path is not absolute");

    /* Keep INFO lines off stderr so this test does not clutter the run; they
     * still reach the file, whose sink is at DEBUG. */
    hermes_log_set_level(HERMES_LOG_LEVEL_ERROR);
    HLOGI("test", "hello-from-hermes-log");
    hermes_log_shutdown();

    FILE *fp = fopen(tmpl, "r");
    TEST_ASSERT_NOT_NULL(fp);
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    unlink(tmpl);

    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "hello-from-hermes-log"),
                                 "logged entry missing from the file sink");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "writing log to"),
                                 "the file does not record its own location");
}

/* Log a line with main:log_utc as given and return the file's contents. */
static void log_one_line(bool utc, char *buf, size_t len)
{
    char tmpl[] = "/tmp/mercury_log_XXXXXX";
    int fd = mkstemp(tmpl);
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);
    hermes_log_set_utc(utc);
    TEST_ASSERT_EQUAL_INT(0, hermes_log_init(64));
    TEST_ASSERT_EQUAL_INT(0, hermes_log_set_file(tmpl, HERMES_LOG_LEVEL_DEBUG, false));
    hermes_log_set_level(HERMES_LOG_LEVEL_ERROR);
    HLOGI("test", "stamp-check");
    hermes_log_shutdown();
    hermes_log_set_utc(false);
    FILE *fp = fopen(tmpl, "r");
    TEST_ASSERT_NOT_NULL(fp);
    size_t n = fread(buf, 1, len - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    unlink(tmpl);
}

/* Local time by default (no Z); UTC with a Z when main:log_utc is set.  The
 * stamp is followed by a space and the uptime, as utils/onair_logs.py reads it. */
void test_timestamps_local_by_default_utc_with_z(void)
{
    char buf[2048];
    log_one_line(false, buf, sizeof buf);
    TEST_ASSERT_NOT_NULL(strstr(buf, "stamp-check"));
    TEST_ASSERT_NULL_MESSAGE(strstr(buf, "Z [+"), "a local-time stamp carries a Z");

    log_one_line(true, buf, sizeof buf);
    TEST_ASSERT_NOT_NULL(strstr(buf, "stamp-check"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "Z [+"), "a UTC stamp has no Z before the uptime");
}

/* The daemon sets the same path twice (main() then engine init); that must not
 * duplicate the "writing log to" header. */
void test_set_file_same_path_announced_once(void)
{
    char tmpl[] = "/tmp/mercury_log_XXXXXX";
    int fd = mkstemp(tmpl);
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);

    TEST_ASSERT_EQUAL_INT(0, hermes_log_init(64));
    TEST_ASSERT_EQUAL_INT(0, hermes_log_set_file(tmpl, HERMES_LOG_LEVEL_DEBUG, false));
    TEST_ASSERT_EQUAL_INT(0, hermes_log_set_file(tmpl, HERMES_LOG_LEVEL_DEBUG, false));
    hermes_log_shutdown();

    FILE *fp = fopen(tmpl, "r");
    TEST_ASSERT_NOT_NULL(fp);
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    unlink(tmpl);

    int count = 0;
    for (const char *p = buf; (p = strstr(p, "writing log to")) != NULL; p++)
        count++;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count, "log location announced more than once");
}

/* A logger already running belongs to whoever started it: a second init says
 * so (1), and leaves it running with its settings -- the embedded engine under
 * a host that runs its own (hermes-radio-daemon). */
void test_init_reports_a_logger_already_running(void)
{
    TEST_ASSERT_EQUAL_INT(0, hermes_log_init(64));
    hermes_log_set_level(HERMES_LOG_LEVEL_WARN);
    TEST_ASSERT_EQUAL_INT(1, hermes_log_init(64));
    TEST_ASSERT_EQUAL_INT(1, hermes_log_init(1024));
    hermes_log_shutdown();
    TEST_ASSERT_EQUAL_INT(0, hermes_log_init(64));   /* stopped: starts again */
    hermes_log_shutdown();
}

/* Count the lines of a file holding a marker, and whether it holds another. */
static int count_lines_with(const char *path, const char *marker, const char *also, bool *has_also)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    char line[1024];
    int n = 0;
    *has_also = false;
    while (fgets(line, sizeof line, fp))
    {
        if (strstr(line, marker)) n++;
        if (also && strstr(line, also)) *has_also = true;
    }
    fclose(fp);
    return n;
}

/* Past the cap the file moves to <path>.1 and a new one starts, each saying
 * where the other is; no line is lost, and neither file outgrows the cap by
 * more than a line.  Cap 0: one file, however long. */
void test_file_rotates_past_the_cap(void)
{
    char tmpl[] = "/tmp/mercury_log_XXXXXX";
    int fd = mkstemp(tmpl);
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);
    char old[sizeof tmpl + 2];
    snprintf(old, sizeof old, "%s.1", tmpl);
    unlink(old);

    const unsigned long long cap = 4096;
    hermes_log_set_max_bytes(cap);
    TEST_ASSERT_EQUAL_INT(0, hermes_log_init(64));
    TEST_ASSERT_EQUAL_INT(0, hermes_log_set_file(tmpl, HERMES_LOG_LEVEL_DEBUG, false));
    hermes_log_set_level(HERMES_LOG_LEVEL_ERROR);
    const int lines = 100;   /* ~10 kB: rotates twice, and .1 is the second */
    for (int i = 0; i < lines; i++)
        HLOGI("test", "rotation-line %03d ................................", i);
    hermes_log_shutdown();
    hermes_log_set_max_bytes(0);

    bool cur_note = false, old_note = false;
    int in_cur = count_lines_with(tmpl, "rotation-line", "earlier lines are in", &cur_note);
    int in_old = count_lines_with(old, "rotation-line", "continued in", &old_note);
    TEST_ASSERT_TRUE_MESSAGE(in_old > 0, "nothing was moved to <path>.1");
    TEST_ASSERT_TRUE_MESSAGE(in_cur > 0, "no new file after the move");
    TEST_ASSERT_TRUE_MESSAGE(cur_note, "the new file does not say where the earlier lines are");
    TEST_ASSERT_TRUE_MESSAGE(old_note, "the moved file does not say where the log went on");

    /* The newest lines, in order and with none missing across the two files:
     * the last line of .1 is followed by the first line of the new file. */
    FILE *fp = fopen(tmpl, "r");
    char line[1024];
    int first = -1, last = -1;
    while (fgets(line, sizeof line, fp))
    {
        const char *m = strstr(line, "rotation-line ");
        if (!m) continue;
        int v = atoi(m + strlen("rotation-line "));
        if (first < 0) first = v;
        TEST_ASSERT_TRUE(last < 0 || v == last + 1);
        last = v;
    }
    fclose(fp);
    TEST_ASSERT_EQUAL_INT(lines - 1, last);
    TEST_ASSERT_EQUAL_INT(lines - in_cur, first);

    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(old, &st));
    TEST_ASSERT_TRUE(st.st_size < (off_t)(cap + 1024));
    unlink(tmpl);
    unlink(old);

    /* No cap: a single file with every line. */
    strcpy(tmpl + strlen(tmpl) - 6, "XXXXXX");
    fd = mkstemp(tmpl);
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);
    snprintf(old, sizeof old, "%s.1", tmpl);
    TEST_ASSERT_EQUAL_INT(0, hermes_log_init(64));
    TEST_ASSERT_EQUAL_INT(0, hermes_log_set_file(tmpl, HERMES_LOG_LEVEL_DEBUG, false));
    hermes_log_set_level(HERMES_LOG_LEVEL_ERROR);
    for (int i = 0; i < lines; i++)
        HLOGI("test", "rotation-line %03d ................................", i);
    hermes_log_shutdown();
    TEST_ASSERT_EQUAL_INT(lines, count_lines_with(tmpl, "rotation-line", NULL, &cur_note));
    TEST_ASSERT_EQUAL_INT(-1, stat(old, &st));
    unlink(tmpl);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_set_file_null_path_is_rejected);
    RUN_TEST(test_init_reports_a_logger_already_running);
    RUN_TEST(test_set_file_unopenable_path_warns_on_stderr);
    RUN_TEST(test_set_file_records_entries);
    RUN_TEST(test_set_file_same_path_announced_once);
    RUN_TEST(test_timestamps_local_by_default_utc_with_z);
    RUN_TEST(test_file_rotates_past_the_cap);
    return UNITY_END();
}
