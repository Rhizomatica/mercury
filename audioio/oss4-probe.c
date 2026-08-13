/* Build-time probe: does this system have the OSSv4 API?
 *
 * ffaudio's oss.c enumerates devices through the OSSv4 interface.  Stock Linux
 * ships only the OSS3 stub in linux/soundcard.h, which has none of the symbols
 * below, so oss.c cannot compile there at all.  The OSSv4 headers arrive with
 * 4Front OSS -- on Debian that is oss4-dev, which *diverts*
 * linux/soundcard.h -- and on FreeBSD they are the system default.
 *
 * audioio/Makefile compiles this file to decide whether to build the OSS
 * backend.  It is never linked into mercury.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <sys/soundcard.h>

int main(void)
{
    oss_audioinfo ai;
    oss_sysinfo si;

    (void) ai;
    (void) si;
    (void) SNDCTL_SYSINFO;
    (void) SNDCTL_AUDIOINFO_EX;
    (void) SNDCTL_DSP_HALT;
    (void) PCM_CAP_OUTPUT;
    (void) OPEN_READ;

    return 0;
}
