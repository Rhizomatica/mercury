/*
 * UI device-list labelling
 *
 * Issue #189: an operator with an IC-7300 and an IC-9700 sees two sound cards
 * that both call themselves "PCM2901 Audio Codec Analog Stereo".  Every UI
 * here is label-driven — the dropdown lists names and maps the chosen label
 * back to a device id — so identical labels make one of the two radios
 * unreachable no matter which entry is picked.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "unity.h"
#include "ui_status.h"
#include "ui_communication.h"

void setUp(void) {}
void tearDown(void) {}

static void set_dev(ui_device_t *d, const char *id, const char *name)
{
    memset(d, 0, sizeof(*d));
    snprintf(d->id, sizeof(d->id), "%s", id);
    snprintf(d->name, sizeof(d->name), "%s", name);
}

/* Every label must be unique, or the UI cannot express the choice. */
void test_colliding_names_become_distinguishable(void)
{
    ui_device_t devs[2];
    set_dev(&devs[0], "alsa_input.usb-Burr-Brown_from_TI_USB_Audio_CODEC-00.analog-stereo",
            "PCM2901 Audio Codec Analog Stereo");
    set_dev(&devs[1], "alsa_input.usb-Burr-Brown_from_TI_USB_Audio_CODEC-00.2.analog-stereo",
            "PCM2901 Audio Codec Analog Stereo");

    ui_devices_disambiguate(devs, 2);

    TEST_ASSERT_TRUE_MESSAGE(strcmp(devs[0].name, devs[1].name) != 0,
        "two devices still share a label: the operator cannot pick between them");
}

/* The distinguishing part has to be the id, and it has to survive whole —
 * these ids share a 51-character prefix, so a truncated one would still tie. */
void test_label_carries_the_full_id(void)
{
    ui_device_t devs[2];
    set_dev(&devs[0], "alsa_input.usb-Burr-Brown_from_TI_USB_Audio_CODEC-00.analog-stereo",
            "PCM2901 Audio Codec Analog Stereo");
    set_dev(&devs[1], "alsa_input.usb-Burr-Brown_from_TI_USB_Audio_CODEC-00.2.analog-stereo",
            "PCM2901 Audio Codec Analog Stereo");

    ui_devices_disambiguate(devs, 2);

    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(devs[0].name, devs[0].id),
                                 "label does not carry its own device id");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(devs[1].name, devs[1].id),
                                 "label does not carry its own device id");
}

/* The ordinary one-radio case must still read as plain English. */
void test_unique_names_are_left_alone(void)
{
    ui_device_t devs[2];
    set_dev(&devs[0], "alsa_input.usb-ICOM_IC-7300", "IC-7300 Analog Stereo");
    set_dev(&devs[1], "alsa_input.pci-0000_00_1f.3", "Built-in Audio Analog Stereo");

    ui_devices_disambiguate(devs, 2);

    TEST_ASSERT_EQUAL_STRING("IC-7300 Analog Stereo", devs[0].name);
    TEST_ASSERT_EQUAL_STRING("Built-in Audio Analog Stereo", devs[1].name);
}

/* Three of a kind: all three must end up distinct, not just the first pair. */
void test_three_way_collision(void)
{
    ui_device_t devs[3];
    set_dev(&devs[0], "id-a", "USB Audio CODEC");
    set_dev(&devs[1], "id-b", "USB Audio CODEC");
    set_dev(&devs[2], "id-c", "USB Audio CODEC");

    ui_devices_disambiguate(devs, 3);

    TEST_ASSERT_TRUE(strcmp(devs[0].name, devs[1].name) != 0);
    TEST_ASSERT_TRUE(strcmp(devs[0].name, devs[2].name) != 0);
    TEST_ASSERT_TRUE(strcmp(devs[1].name, devs[2].name) != 0);
}

/* Degenerate inputs must not crash: called on every list refresh. */
void test_degenerate_inputs(void)
{
    ui_device_t one;
    set_dev(&one, "id", "Only Device");
    ui_devices_disambiguate(NULL, 5);
    ui_devices_disambiguate(&one, 0);
    ui_devices_disambiguate(&one, 1);
    TEST_ASSERT_EQUAL_STRING("Only Device", one.name);
}

/* A device with no id cannot be disambiguated; it must be left as-is rather
 * than gaining an empty "[]" suffix. */
void test_missing_id_is_left_alone(void)
{
    ui_device_t devs[2];
    set_dev(&devs[0], "", "Same Name");
    set_dev(&devs[1], "real-id", "Same Name");

    ui_devices_disambiguate(devs, 2);

    TEST_ASSERT_EQUAL_STRING("Same Name", devs[0].name);
    TEST_ASSERT_NOT_NULL(strstr(devs[1].name, "real-id"));
}

/* Long names must not eat the id.  UI_DEV_NAME_MAX is 256 and the label is
 * "<name> [<id>]", so a wordy product string plus a long PulseAudio node name
 * overflows the field.  Truncating from the right cuts the id, and two ids
 * that differ only past the cut collapse to the same label — the fix undoing
 * itself, silently.  The id has to survive; the name is what gives way. */
void test_long_name_does_not_truncate_the_id(void)
{
    char longname[200];
    memset(longname, 'X', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    memcpy(longname, "USB Audio CODEC ", 16);

    char id1[240], id2[242];
    snprintf(id1, sizeof(id1), "alsa_input.usb-vendor_%0*d-00.analog-stereo", 200, 0);
    snprintf(id2, sizeof(id2), "alsa_input.usb-vendor_%0*d-00.2.analog-stereo", 200, 0);

    ui_device_t devs[2];
    set_dev(&devs[0], id1, longname);
    set_dev(&devs[1], id2, longname);

    ui_devices_disambiguate(devs, 2);

    TEST_ASSERT_TRUE_MESSAGE(strcmp(devs[0].name, devs[1].name) != 0,
        "labels collapsed to the same string: the id was truncated away");
}

/* Whatever happens, the label must stay inside the field and NUL-terminated. */
void test_label_always_fits_the_field(void)
{
    char maxid[UI_DEV_ID_MAX];
    memset(maxid, 'z', sizeof(maxid) - 1);
    maxid[sizeof(maxid) - 1] = '\0';

    ui_device_t devs[2];
    set_dev(&devs[0], maxid, "Same Name");
    maxid[0] = 'a';
    set_dev(&devs[1], maxid, "Same Name");

    ui_devices_disambiguate(devs, 2);

    TEST_ASSERT_TRUE(strlen(devs[0].name) < UI_DEV_NAME_MAX);
    TEST_ASSERT_TRUE(strlen(devs[1].name) < UI_DEV_NAME_MAX);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_colliding_names_become_distinguishable);
    RUN_TEST(test_label_carries_the_full_id);
    RUN_TEST(test_unique_names_are_left_alone);
    RUN_TEST(test_three_way_collision);
    RUN_TEST(test_degenerate_inputs);
    RUN_TEST(test_missing_id_is_left_alone);
    RUN_TEST(test_long_name_does_not_truncate_the_id);
    RUN_TEST(test_label_always_fits_the_field);
    return UNITY_END();
}
