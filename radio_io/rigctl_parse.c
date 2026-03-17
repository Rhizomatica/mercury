/*
 * Based on rigctl_parse.c, by
 *                  (C) Stephane Fillod 2000-2011
 *                  (C) Nate Bargmann 2003,2006,2008,2010,2011,2012,2013
 *                  (C) Terry Embry 2008-2009
 *                  (C) The Hamlib Group 2002,2006,2007,2008,2009,2010,2011
 *                  (C) Rafael Diniz 2024
 *
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

#ifdef HAVE_HAMLIB

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hamlib/rig.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif


#include "rigctl_parse.h"

struct mod_entry
{
    int id;
    char mfg_name[32];
    char model_name[32];
    char version[32];
    char status[32];
    char macro_name[32];
};

static struct mod_entry *g_models = NULL;
static int g_count = 0;
static int g_capacity = 0;

static int collect_model(const struct rig_caps *caps, void *data)
{
    (void)data;

    if (g_count >= g_capacity)
    {
        g_capacity = g_capacity ? g_capacity * 2 : 512;
        g_models = realloc(g_models, g_capacity * sizeof(struct mod_entry));
        if (!g_models)
        {
            fprintf(stderr, "Out of memory listing models\n");
            exit(EXIT_FAILURE);
        }
    }

    struct mod_entry *e = &g_models[g_count++];
    e->id = caps->rig_model;
    snprintf(e->mfg_name, sizeof(e->mfg_name), "%s", caps->mfg_name);
    snprintf(e->model_name, sizeof(e->model_name), "%s", caps->model_name);
    snprintf(e->version, sizeof(e->version), "%s", caps->version);
    snprintf(e->macro_name, sizeof(e->macro_name), "%s", caps->macro_name);
    snprintf(e->status, sizeof(e->status), "%s", rig_strstatus(caps->status));

    return 1;  /* !=0, we want them all */
}

static int cmp_model_id(const void *a, const void *b)
{
    const struct mod_entry *ea = a;
    const struct mod_entry *eb = b;
    return (ea->id > eb->id) - (ea->id < eb->id);
}

void list_models(void)
{
    int status;

    rig_load_all_backends();

    printf(" Rig #  Mfg                    Model                   Version         Status      Macro\n");
    status = rig_list_foreach(collect_model, NULL);

    if (status != RIG_OK)
    {
        printf("rig_list_foreach: error = %s\n", rigerror(status));
        exit(2);
    }

    qsort(g_models, g_count, sizeof(struct mod_entry), cmp_model_id);

    for (int i = 0; i < g_count; i++)
    {
        struct mod_entry *e = &g_models[i];
        printf("%6d  %-23s%-24s%-16s%-12s%s\n",
               e->id,
               e->mfg_name,
               e->model_name,
               e->version,
               e->status,
               e->macro_name);
    }

    free(g_models);
    g_models = NULL;
    g_count = 0;
    g_capacity = 0;
}

int get_radio_list(char ids[][16], char names[][64], int max_count)
{
    /* Load backends quietly — redirect stdout & stderr to /dev/null
     * so the hamlib "initrigs4_*" messages don't pollute the console. */
    if (g_count == 0)
    {
#ifndef _WIN32
        fflush(stdout);
        fflush(stderr);
        int saved_stdout = dup(STDOUT_FILENO);
        int saved_stderr = dup(STDERR_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
#endif

        rig_load_all_backends();

#ifndef _WIN32
        /* Restore original stdout / stderr */
        fflush(stdout);
        fflush(stderr);
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdout);
        close(saved_stderr);
#endif
        int status = rig_list_foreach(collect_model, NULL);
        if (status != RIG_OK)
        {
            fprintf(stderr, "rig_list_foreach: error = %s\n", rigerror(status));
            return 0;
        }

        qsort(g_models, g_count, sizeof(struct mod_entry), cmp_model_id);
    }

    int n = g_count < max_count ? g_count : max_count;
    for (int i = 0; i < n; i++)
    {
        struct mod_entry *e = &g_models[i];
        snprintf(ids[i], 16, "%d", e->id);
        snprintf(names[i], 64, "%s %s", e->mfg_name, e->model_name);
    }
    return n;
}

#endif /* HAVE_HAMLIB */
