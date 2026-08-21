/* HERMES Modem - CM108-class USB sound-chip GPIO PTT backend
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Many cheap USB sound interfaces put PTT on a GPIO pin of the CM108/CM119
 * family, keyed by a 5-byte HID output report.  The All-In-One Cable does this
 * too (1209:7388), which matters because it gives an AIOC user a second way in
 * when their kernel enumerates the CDC serial port awkwardly.
 *
 * Two transports, chosen at build time:
 *
 *   HAVE_HIDAPI  - hidapi, when pkg-config finds it.  Preferred, because it is
 *                  what makes this backend work on Windows and macOS as well.
 *   __linux__    - otherwise, talk to /dev/hidraw directly.  The report is five
 *                  bytes and enumeration is a sysfs walk, so a Raspberry Pi
 *                  build needs no extra package to key a radio; hamlib drives
 *                  CM108 the same way.
 *
 * Neither is a hard dependency.  On a platform with neither, cm108_ptt_open()
 * says so rather than failing obscurely.
 *
 * Permissions: /dev/hidraw* is usually root-only.  Mercury already runs as
 * root on the field stations; elsewhere a udev rule is needed, so the open
 * error explicitly names EACCES rather than leaving the operator guessing.
 */

#include "cm108_ptt.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../common/hermes_log.h"

#define CM108_LOG_TAG "cm108-ptt"

/* Every chip here shares the CM108 GPIO HID command set. */
struct cm108_variant {
    unsigned vid;
    unsigned pid;
    const char *name;
};

static const struct cm108_variant CM108_VARIANTS[] = {
    { 0x0D8C, 0x0008, "CM108/109/119 (legacy)" },
    { 0x0D8C, 0x0009, "CM108/109/119 (legacy)" },
    { 0x0D8C, 0x000A, "CM108/109/119 (legacy)" },
    { 0x0D8C, 0x000B, "CM108/109/119 (legacy)" },
    { 0x0D8C, 0x000C, "CM108/109/119 (legacy)" },
    { 0x0D8C, 0x000D, "CM108/109/119 (legacy)" },
    { 0x0D8C, 0x000E, "CM108/109/119 (legacy)" },
    { 0x0D8C, 0x000F, "CM108/109/119 (legacy)" },
    { 0x0D8C, 0x0139, "CM108AH" },
    { 0x0D8C, 0x013A, "CM119A" },
    { 0x0D8C, 0x0013, "CM119B" },
    { 0x0D8C, 0x0012, "CM108B" },
    { 0x1209, 0x7388, "AIOC" },
};

static const size_t CM108_VARIANT_COUNT =
    sizeof(CM108_VARIANTS) / sizeof(CM108_VARIANTS[0]);

static const char *variant_name(unsigned vid, unsigned pid)
{
    for (size_t i = 0; i < CM108_VARIANT_COUNT; i++)
        if (CM108_VARIANTS[i].vid == vid && CM108_VARIANTS[i].pid == pid)
            return CM108_VARIANTS[i].name;
    return NULL;
}

/* GPIO n is bit n-1 of the mask byte.  Byte 2 and byte 3 both carry the mask:
 * byte 2 is the "GPIO output" register and byte 3 the "GPIO direction/enable",
 * and the CM108 family wants both driven for the pin to actually move. */
int cm108_ptt_report(bool on, int gpio, unsigned char out[5])
{
    if (!out || gpio < 1 || gpio > 4)
        return -1;

    unsigned char mask = on ? (unsigned char)(1u << (gpio - 1)) : 0x00;
    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = mask;
    out[3] = mask;
    out[4] = 0x00;
    return 0;
}

#if defined(HAVE_HIDAPI)

#include <hidapi.h>

static hid_device *g_hid = NULL;
static int g_gpio = PTT_CM108_GPIO_DEFAULT;

int cm108_ptt_list(char *buf, size_t buf_size)
{
    if (buf && buf_size)
        buf[0] = '\0';

    if (hid_init() != 0)
        return 0;

    int found = 0;
    struct hid_device_info *devs = hid_enumerate(0x0, 0x0);
    for (struct hid_device_info *d = devs; d; d = d->next)
    {
        const char *chip = variant_name(d->vendor_id, d->product_id);
        if (!chip)
            continue;
        found++;
        if (buf && buf_size)
        {
            size_t used = strlen(buf);
            snprintf(buf + used, buf_size > used ? buf_size - used : 0,
                     "%s  %04x:%04x  %s\n",
                     d->path ? d->path : "?", d->vendor_id, d->product_id, chip);
        }
    }
    hid_free_enumeration(devs);
    return found;
}

/* config->device, when set, is matched against the hidapi path or the USB
 * serial number -- a path is stable per port, a serial number per cable. */
static hid_device *open_selected(const ptt_config_t *config)
{
    if (hid_init() != 0)
    {
        HLOGE(CM108_LOG_TAG, "hid_init() failed");
        return NULL;
    }

    struct hid_device_info *devs = hid_enumerate(0x0, 0x0);
    struct hid_device_info *chosen = NULL;
    for (struct hid_device_info *d = devs; d; d = d->next)
    {
        if (!variant_name(d->vendor_id, d->product_id))
            continue;
        if (!config->device[0])
        {
            chosen = d;
            break;
        }
        if (d->path && !strcmp(d->path, config->device))
        {
            chosen = d;
            break;
        }
        if (d->serial_number)
        {
            char sn[128];
            if (wcstombs(sn, d->serial_number, sizeof(sn)) != (size_t)-1 &&
                !strcmp(sn, config->device))
            {
                chosen = d;
                break;
            }
        }
    }

    hid_device *h = NULL;
    if (chosen)
    {
        HLOGI(CM108_LOG_TAG, "Using %s (%04x:%04x %s)",
              chosen->path ? chosen->path : "?", chosen->vendor_id,
              chosen->product_id,
              variant_name(chosen->vendor_id, chosen->product_id));
        h = hid_open_path(chosen->path);
        if (!h)
            HLOGE(CM108_LOG_TAG, "Cannot open the CM108 HID endpoint. "
                                 "It is usually root-only; run as root or add "
                                 "a udev rule.");
    }
    else if (config->device[0])
        HLOGE(CM108_LOG_TAG, "CM108 device '%s' not found", config->device);
    else
        HLOGE(CM108_LOG_TAG, "No CM108-class PTT device found "
                             "(set ptt.device to a path or USB serial to override)");

    hid_free_enumeration(devs);
    return h;
}

static int write_report(bool on)
{
    if (!g_hid)
        return -1;
    unsigned char report[5];
    if (cm108_ptt_report(on, g_gpio, report) != 0)
        return -1;
    if (hid_write(g_hid, report, sizeof(report)) < 0)
    {
        HLOGE(CM108_LOG_TAG, "HID write failed");
        return -1;
    }
    return 0;
}

int cm108_ptt_open(const ptt_config_t *config)
{
    if (!config)
        return -1;

    g_gpio = config->cm108_gpio;
    if (g_gpio < 1 || g_gpio > 4)
    {
        HLOGW(CM108_LOG_TAG, "cm108_gpio=%d out of range 1..4; using %d",
              g_gpio, PTT_CM108_GPIO_DEFAULT);
        g_gpio = PTT_CM108_GPIO_DEFAULT;
    }

    g_hid = open_selected(config);
    if (!g_hid)
        return -1;

    HLOGI(CM108_LOG_TAG, "CM108 PTT via hidapi, GPIO%d", g_gpio);
    if (write_report(false) != 0)   /* park un-keyed */
    {
        cm108_ptt_close();
        return -1;
    }
    return 0;
}

int cm108_ptt_set(bool on)
{
    return write_report(on);
}

void cm108_ptt_close(void)
{
    if (!g_hid)
        return;
    (void)write_report(false);
    hid_close(g_hid);
    g_hid = NULL;
    hid_exit();
}

#elif defined(__linux__)

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

static int g_hid_fd = -1;
static int g_gpio = PTT_CM108_GPIO_DEFAULT;

/* Read "HID_ID=0003:00000D8C:00000013" out of a hidraw device's uevent. */
static bool hidraw_ids(const char *name, unsigned *vid, unsigned *pid,
                       char *desc, size_t desc_size)
{
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", name);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    bool got = false;
    char line[256];
    if (desc && desc_size)
        desc[0] = '\0';
    while (fgets(line, sizeof(line), f))
    {
        unsigned bus;
        if (sscanf(line, "HID_ID=%x:%x:%x", &bus, vid, pid) == 3)
            got = true;
        else if (desc && desc_size && strncmp(line, "HID_NAME=", 9) == 0)
        {
            snprintf(desc, desc_size, "%s", line + 9);
            desc[strcspn(desc, "\r\n")] = '\0';
        }
    }
    fclose(f);
    return got;
}

int cm108_ptt_list(char *buf, size_t buf_size)
{
    if (buf && buf_size)
        buf[0] = '\0';

    DIR *d = opendir("/sys/class/hidraw");
    if (!d)
        return 0;

    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (strncmp(e->d_name, "hidraw", 6) != 0)
            continue;
        unsigned vid = 0, pid = 0;
        char desc[128];
        if (!hidraw_ids(e->d_name, &vid, &pid, desc, sizeof(desc)))
            continue;
        const char *chip = variant_name(vid, pid);
        if (!chip)
            continue;
        found++;
        if (buf && buf_size)
        {
            size_t used = strlen(buf);
            snprintf(buf + used, buf_size > used ? buf_size - used : 0,
                     "/dev/%s  %04x:%04x  %s  %s\n",
                     e->d_name, vid, pid, chip, desc);
        }
    }
    closedir(d);
    return found;
}

/* Pick an explicit device, or the first CM108-class one present. */
static int resolve_device(const ptt_config_t *config, char *out, size_t out_size)
{
    if (config->device[0])
    {
        snprintf(out, out_size, "%s", config->device);
        return 0;
    }

    DIR *d = opendir("/sys/class/hidraw");
    if (!d)
    {
        HLOGE(CM108_LOG_TAG, "No /sys/class/hidraw; is this a Linux kernel with hidraw?");
        return -1;
    }

    int rc = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (strncmp(e->d_name, "hidraw", 6) != 0)
            continue;
        unsigned vid = 0, pid = 0;
        if (!hidraw_ids(e->d_name, &vid, &pid, NULL, 0))
            continue;
        const char *chip = variant_name(vid, pid);
        if (!chip)
            continue;
        snprintf(out, out_size, "/dev/%s", e->d_name);
        HLOGI(CM108_LOG_TAG, "Using %s (%04x:%04x %s)", out, vid, pid, chip);
        rc = 0;
        break;
    }
    closedir(d);

    if (rc != 0)
        HLOGE(CM108_LOG_TAG, "No CM108-class PTT device found "
                             "(set ptt.device to a /dev/hidrawN to override)");
    return rc;
}

static int write_report(bool on)
{
    if (g_hid_fd < 0)
        return -1;

    unsigned char report[5];
    if (cm108_ptt_report(on, g_gpio, report) != 0)
        return -1;

    ssize_t n = write(g_hid_fd, report, sizeof(report));
    if (n != (ssize_t)sizeof(report))
    {
        HLOGE(CM108_LOG_TAG, "HID write failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int cm108_ptt_open(const ptt_config_t *config)
{
    if (!config)
        return -1;

    g_gpio = config->cm108_gpio;
    if (g_gpio < 1 || g_gpio > 4)
    {
        HLOGW(CM108_LOG_TAG, "cm108_gpio=%d out of range 1..4; using %d",
              g_gpio, PTT_CM108_GPIO_DEFAULT);
        g_gpio = PTT_CM108_GPIO_DEFAULT;
    }

    char path[PTT_DEVICE_PATH_MAX];
    if (resolve_device(config, path, sizeof(path)) != 0)
        return -1;

    g_hid_fd = open(path, O_RDWR);
    if (g_hid_fd < 0)
    {
        if (errno == EACCES)
            HLOGE(CM108_LOG_TAG, "Cannot open %s: permission denied. "
                                 "/dev/hidraw* is root-only by default; run as "
                                 "root or add a udev rule.", path);
        else
            HLOGE(CM108_LOG_TAG, "Cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    HLOGI(CM108_LOG_TAG, "CM108 PTT on %s, GPIO%d", path, g_gpio);
    if (write_report(false) != 0)   /* park un-keyed */
    {
        cm108_ptt_close();
        return -1;
    }
    return 0;
}

int cm108_ptt_set(bool on)
{
    return write_report(on);
}

void cm108_ptt_close(void)
{
    if (g_hid_fd < 0)
        return;
    (void)write_report(false);
    close(g_hid_fd);
    g_hid_fd = -1;
}

#else  /* !__linux__ */

int cm108_ptt_open(const ptt_config_t *config)
{
    (void)config;
    HLOGE(CM108_LOG_TAG,
          "CM108 PTT is implemented for Linux only (it talks to /dev/hidraw). "
          "Use the serial or hamlib PTT method on this platform.");
    return -1;
}

int cm108_ptt_set(bool on)      { (void)on; return -1; }
void cm108_ptt_close(void)      { }
int cm108_ptt_list(char *buf, size_t buf_size)
{
    if (buf && buf_size) buf[0] = '\0';
    return 0;
}

#endif /* __linux__ */
