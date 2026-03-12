/*
 * rigctl_parse.h - (C) Stephane Fillod 2000-2010
 *                  (C) Rafael Diniz 2024
 *
 * This program test/control a radio using Hamlib.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, write to the Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#ifndef RIGCTL_PARSE_H
#define RIGCTL_PARSE_H

#ifdef HAVE_HAMLIB

#include <stdio.h>
#include <hamlib/rig.h>

void list_models(void);

/* Populate arrays with radio IDs (as strings) and display names ("Mfg Model").
 * Returns the number of entries written (up to max_count). */
int get_radio_list(char ids[][16], char names[][64], int max_count);

#endif /* HAVE_HAMLIB */

#endif  /* RIGCTL_PARSE_H */
