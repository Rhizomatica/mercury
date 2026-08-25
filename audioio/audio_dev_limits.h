/* How much room a device id / name needs.
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
 * Its own header, with no dependencies, because the two sides that must agree
 * on this width cannot include each other: audioio.h pulls in ffbase, which is
 * not on the UI's include path (hence the hand-written get_soundcard_list
 * prototype in ui_communication.c), and audioio must not depend on the UI.
 *
 * Getting it wrong is not a compile error, it is a silently mangled device
 * name -- see issue #185, where 64 cut an IC-7300's PulseAudio node
 *
 *   alsa_input.usb-Burr-Brown_from_TI_USB_Audio_CODEC-00.analog-stereo
 *
 * to 63 characters.  That truncated id was written to mercury.ini and could
 * never match a real device again, so the backend fell back to the default
 * card and the radio appeared deaf.
 */

#ifndef AUDIO_DEV_LIMITS_H
#define AUDIO_DEV_LIMITS_H

/* PulseAudio/PipeWire node names reach ~70 characters for a plain USB codec
 * and longer for multi-profile cards; ALSA "plughw:CARD=..." and the WASAPI
 * "{GUID}.{GUID}" forms are comparable.  256 leaves real headroom without
 * making the enumeration arrays worth heap-allocating. */
#define AUDIO_DEV_STR_MAX 256

#endif /* AUDIO_DEV_LIMITS_H */
