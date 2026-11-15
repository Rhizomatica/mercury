/* Per-peer rung memory across sessions: a small flat file, one line per
 * peer, read at car_start, written at teardown.  The receiver chooses the
 * rung, so the hint never crosses the wire -- a station with memory and one
 * without interoperate, in both directions (docs/EFFICIENCY-FOLLOWUPS.md,
 * A).  MERCURY_RUNG_MEMORY=0 never calls either half (arq_fsm.c), and
 * MERCURY_RUNG_HINTS points the file somewhere else, which is how the sim
 * and the tests keep their A/B runs isolated. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "rung_hint.h"

#if defined(_WIN32)
#define strtok_r(s, d, p) strtok_s(s, d, p)
#endif

#define RUNG_HINT_PEERS 64
#define RUNG_HINT_NAME  16          /* callsign, as the FSM keeps it */

typedef struct
{
    char   peer[RUNG_HINT_NAME];
    car_rung_hint_t hint;
} rung_hint_entry_t;

static char rung_hint_file[512];

void rung_hint_set_path(const char *path)
{
    snprintf(rung_hint_file, sizeof(rung_hint_file), "%s", path ? path : "");
}

static void rung_hint_default_path(char *buf, size_t bufsz)
{
    const char *base = NULL;
#if defined(_WIN32)
    base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("APPDATA");
    if (base && base[0])
        snprintf(buf, bufsz, "%s\\mercury\\rung_hints", base);
    else
        snprintf(buf, bufsz, "rung_hints");
#elif defined(__APPLE__)
    base = getenv("HOME");
    if (base && base[0])
        snprintf(buf, bufsz, "%s/Library/Application Support/mercury/rung_hints", base);
    else
        snprintf(buf, bufsz, "rung_hints");
#else
    base = getenv("XDG_STATE_HOME");
    if (base && base[0])
        snprintf(buf, bufsz, "%s/mercury/rung_hints", base);
    else if ((base = getenv("HOME")) && base[0])
        snprintf(buf, bufsz, "%s/.local/state/mercury/rung_hints", base);
    else
        snprintf(buf, bufsz, "rung_hints");
#endif
}

static const char *rung_hint_path(void)
{
    if (!rung_hint_file[0])
    {
        const char *env = getenv("MERCURY_RUNG_HINTS");
        if (env && env[0])
            rung_hint_set_path(env);
        else
        {
            char buf[512];
            rung_hint_default_path(buf, sizeof(buf));
            rung_hint_set_path(buf);
        }
    }
    return rung_hint_file;
}

/* "CALL sent0 lost0 ... sent6 lost6": doubles, two per rung.  A line that
 * does not parse is skipped, not fatal -- the file is disposable state. */
static bool rung_hint_parse(char *line, rung_hint_entry_t *e)
{
    char *save = NULL;
    char *tok = strtok_r(line, " \t\r\n", &save);
    if (!tok || strlen(tok) >= RUNG_HINT_NAME) return false;
    for (int lv = 0; lv < CAR_NLEVELS; lv++)
    {
        char *end = NULL;
        char *s = strtok_r(NULL, " \t\r\n", &save);
        char *l = strtok_r(NULL, " \t\r\n", &save);
        if (!s || !l) return false;
        e->hint.sent[lv] = strtod(s, &end);
        if (!end || *end) return false;
        e->hint.lost[lv] = strtod(l, &end);
        if (!end || *end) return false;
    }
    snprintf(e->peer, sizeof(e->peer), "%s", tok);
    return true;
}

static int rung_hint_read(rung_hint_entry_t *out, int max)
{
    FILE *f = fopen(rung_hint_path(), "r");
    int n = 0;
    if (!f) return 0;
    char line[512];
    while (n < max && fgets(line, sizeof(line), f))
    {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        if (rung_hint_parse(line, &out[n])) n++;
    }
    fclose(f);
    return n;
}

static void rung_hint_mkdirs(char *path)
{
    for (char *p = path + 1; *p; p++)
    {
        if (*p == '/' || *p == '\\')
        {
            char saved = *p;
            *p = '\0';
#if defined(_WIN32)
            (void)_mkdir(path);
#else
            (void)mkdir(path, 0755);
#endif
            *p = saved;
        }
    }
}

bool rung_hint_load(const char *peer, car_rung_hint_t *h)
{
    if (!peer || !peer[0]) return false;
    rung_hint_entry_t entries[RUNG_HINT_PEERS];
    int n = rung_hint_read(entries, RUNG_HINT_PEERS);
    for (int i = 0; i < n; i++)
    {
        if (!strcmp(entries[i].peer, peer))
        {
            bool any = false;
            for (int lv = 0; lv < CAR_NLEVELS; lv++)
                if (entries[i].hint.sent[lv] >= 1.0) any = true;
            if (!any) return false;
            *h = entries[i].hint;
            return true;
        }
    }
    return false;
}

void rung_hint_store(const char *peer, const car_rung_hint_t *h)
{
    if (!peer || !peer[0] || strlen(peer) >= RUNG_HINT_NAME) return;
    rung_hint_entry_t entries[RUNG_HINT_PEERS];
    int n = rung_hint_read(entries, RUNG_HINT_PEERS);
    int mine = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(entries[i].peer, peer)) { mine = i; break; }
    if (mine < 0 && n >= RUNG_HINT_PEERS)
    {
        /* Full: drop the oldest line's worth of evidence rather than a peer
         * that is in the file -- a hint gone stale costs one probe frame. */
        n = RUNG_HINT_PEERS - 1;
        memmove(entries, entries + 1, n * sizeof(entries[0]));
    }
    if (mine < 0)
    {
        mine = n++;
        snprintf(entries[mine].peer, sizeof(entries[mine].peer), "%s", peer);
    }
    entries[mine].hint = *h;

    char tmp[560];
    snprintf(tmp, sizeof(tmp), "%s.tmp", rung_hint_path());
    FILE *f = fopen(tmp, "w");
    if (!f) return;                        /* the hint was best-effort anyway */
    for (int i = 0; i < n; i++)
    {
        fprintf(f, "%s", entries[i].peer);
        for (int lv = 0; lv < CAR_NLEVELS; lv++)
            fprintf(f, " %.2f %.2f", entries[i].hint.sent[lv], entries[i].hint.lost[lv]);
        fputc('\n', f);
    }
    fclose(f);
    char resolved[560];
    snprintf(resolved, sizeof(resolved), "%s", rung_hint_path());
    rung_hint_mkdirs(resolved);
    remove(rung_hint_path());
#if defined(_WIN32)
    if (rename(tmp, rung_hint_path()) != 0) remove(tmp);
#else
    if (rename(tmp, rung_hint_path()) != 0) remove(tmp);
#endif
}
