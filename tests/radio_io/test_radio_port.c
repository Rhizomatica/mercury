/* radio_device vs the rig's actual port type (issue #179).
 *
 * Copyright (C) 2026 Rhizomatica
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * ---------------------------------------------------------------------------
 *
 * A FlexRadio (SmartSDR, model 23005) is a network rig.  Configured with
 * radio_device = COM4, Hamlib built "COM4:4992", failed to resolve it, and
 * reported "Invalid parameter" -- which sent the reporter looking at baud rates
 * and CAT ports rather than at the address he actually needed.
 *
 * These are shape checks on user-supplied strings, and their real risk is the
 * false positive: a heuristic that cries wrong about a working configuration is
 * worse than no heuristic.  Hence the emphasis below on what must stay silent.
 */

#include "unity.h"
#include "radio_port.h"

/* --- the reported case ------------------------------------------------- */

void test_network_rig_with_a_com_port_is_flagged(void)
{
    /* Exactly issue #179. */
    TEST_ASSERT_EQUAL(RADIO_PORT_WANTS_NETWORK,
                      radio_port_check(RADIO_PORT_KIND_NETWORK, "COM4"));
    TEST_ASSERT_NOT_NULL(radio_port_advice(
        radio_port_check(RADIO_PORT_KIND_NETWORK, "COM4")));
}

void test_network_rig_with_a_tty_is_flagged(void)
{
    TEST_ASSERT_EQUAL(RADIO_PORT_WANTS_NETWORK,
                      radio_port_check(RADIO_PORT_KIND_NETWORK, "/dev/ttyUSB0"));
}

/* --- what must NOT be flagged ------------------------------------------ */

void test_network_rig_with_an_address_is_accepted(void)
{
    const char *good[] = {
        "192.168.1.50",         /* what fixed the reporter's setup     */
        "192.168.1.50:4992",    /* explicit port                       */
        "localhost",
        "localhost:4532",       /* rigctld                             */
        "flex.local",
        "::1",                  /* bare IPv6 loopback                  */
        "[fe80::1]:4992",       /* bracketed IPv6 with port            */
    };
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++)
        TEST_ASSERT_EQUAL_MESSAGE(RADIO_PORT_OK,
            radio_port_check(RADIO_PORT_KIND_NETWORK, good[i]), good[i]);
}

void test_serial_rig_with_a_port_name_is_accepted(void)
{
    const char *good[] = {
        "COM4", "COM12", "com4",        /* Windows, incl. lower case   */
        "\\\\.\\COM12",                 /* \\.\COM12 for ports > 9     */
        "/dev/ttyUSB0",
        "/dev/cu.usbserial-A600eHIS",   /* macOS                       */
        "/dev/serial/by-id/usb-FTDI",   /* stable udev path            */
    };
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++)
        TEST_ASSERT_EQUAL_MESSAGE(RADIO_PORT_OK,
            radio_port_check(RADIO_PORT_KIND_SERIAL, good[i]), good[i]);
}

/* An empty device means "use Hamlib's default", which is a legitimate choice
 * for either kind of rig and must never draw a warning. */
void test_unset_device_is_never_flagged(void)
{
    TEST_ASSERT_EQUAL(RADIO_PORT_OK, radio_port_check(RADIO_PORT_KIND_NETWORK, ""));
    TEST_ASSERT_EQUAL(RADIO_PORT_OK, radio_port_check(RADIO_PORT_KIND_SERIAL, ""));
    TEST_ASSERT_EQUAL(RADIO_PORT_OK, radio_port_check(RADIO_PORT_KIND_NETWORK, NULL));
    TEST_ASSERT_EQUAL(RADIO_PORT_OK, radio_port_check(RADIO_PORT_KIND_SERIAL, NULL));
}

/* Dummy rigs, USB-native rigs and anything else we do not model must pass
 * whatever the user wrote: we have no opinion, so we hold none. */
void test_other_port_kinds_are_never_flagged(void)
{
    TEST_ASSERT_EQUAL(RADIO_PORT_OK, radio_port_check(RADIO_PORT_KIND_OTHER, "COM4"));
    TEST_ASSERT_EQUAL(RADIO_PORT_OK, radio_port_check(RADIO_PORT_KIND_OTHER, "192.168.1.50"));
    TEST_ASSERT_NULL(radio_port_advice(RADIO_PORT_OK));
}

/* --- the serial-side mirror -------------------------------------------- */

void test_serial_rig_with_an_address_is_flagged(void)
{
    TEST_ASSERT_EQUAL(RADIO_PORT_WANTS_SERIAL,
                      radio_port_check(RADIO_PORT_KIND_SERIAL, "192.168.1.50"));
    TEST_ASSERT_NOT_NULL(radio_port_advice(
        radio_port_check(RADIO_PORT_KIND_SERIAL, "192.168.1.50")));
}

/* --- the classifier itself --------------------------------------------- */

void test_com_prefix_matching_is_not_greedy(void)
{
    /* "COM" must be followed by digits and nothing else, or a hostname that
     * merely starts with those letters gets misread as a serial port. */
    TEST_ASSERT_TRUE(radio_port_path_is_serial("COM4"));
    TEST_ASSERT_TRUE(radio_port_path_is_serial("COM10"));

    TEST_ASSERT_FALSE(radio_port_path_is_serial("COM4x"));
    TEST_ASSERT_FALSE(radio_port_path_is_serial("COM"));
    TEST_ASSERT_FALSE(radio_port_path_is_serial("COMPUTER"));
    TEST_ASSERT_FALSE(radio_port_path_is_serial("com.example.net"));
    TEST_ASSERT_FALSE(radio_port_path_is_serial("commsrv:4992"));
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_network_rig_with_a_com_port_is_flagged);
    RUN_TEST(test_network_rig_with_a_tty_is_flagged);
    RUN_TEST(test_network_rig_with_an_address_is_accepted);
    RUN_TEST(test_serial_rig_with_a_port_name_is_accepted);
    RUN_TEST(test_unset_device_is_never_flagged);
    RUN_TEST(test_other_port_kinds_are_never_flagged);
    RUN_TEST(test_serial_rig_with_an_address_is_flagged);
    RUN_TEST(test_com_prefix_matching_is_not_greedy);
    return UNITY_END();
}
