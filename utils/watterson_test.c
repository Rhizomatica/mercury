/* Watterson HF Channel Simulation Utility
 *
 * Copyright (C) 2025-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Command-line tool to apply the Watterson HF ionospheric channel model
 * (ITU-R F.1487) to a stream of real 16-bit integer samples. Based on the
 * FreeDV ch.c channel simulator by David Rowe.
 *
 * Pipeline:
 *   real int16 input -> Gain -> Hilbert Transform -> Clipper ->
 *   Frequency Shift -> Watterson Model -> AWGN -> SSB Filter ->
 *   real int16 output
 */

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/watterson.h"
#include "modem/freedv/comp.h"
#include "modem/freedv/comp_prim.h"

/* Hilbert transform and SSB filter coefficients (from freedv) */
#include "modem/freedv/ht_coeff.h"
#include "modem/freedv/ssbfilt_coeff.h"

#define BUF_N  160

/* Every entry of watterson_presets() is also a long option (--ccir-poor, --poor,
 * ...).  Their option values are PRESET_OPT_BASE + table index, so each entry
 * has its own value: getopt_long() then reports an abbreviation that fits more
 * than one name (--itur-m) as ambiguous instead of running the first match. */
#define PRESET_OPT_BASE 1000

static const char *USAGE_HEAD =
    "Command line HF channel simulation tool using the Watterson model.\n"
    "\n"
    "usage: %s InputFile OutputFile [Options]\n"
    "       %s --list-presets\n"
    "       %s --help\n"
    "\n"
    "  real int16 input -> Gain -> Hilbert Transform -> Clipper ->\n"
    "  Frequency Shift -> Watterson Model -> AWGN -> SSB Filter ->\n"
    "  real int16 output\n"
    "\n"
    "  Options may come before or after the two file names.  Use '-' for stdin\n"
    "  or stdout.  Every option name must be typed in full: an abbreviation such\n"
    "  as --itur-m is rejected.\n"
    "\n"
    "Options:\n"
    "  --Fs SampleRateHz      Sample rate (default 8000).  The channel is built\n"
    "                         after all options are read, so --Fs may stand in\n"
    "                         any position.  A path delay is rounded UP to a\n"
    "                         whole sample: ceil(delay_ms * Fs / 1000).  Every\n"
    "                         preset delay is a whole sample at 8000, 12000,\n"
    "                         16000 and 48000 Hz.\n"
    "  --gain G               Linear gain (default 1.0)\n"
    "  --freq FoffHz          Frequency offset of the whole signal, applied\n"
    "                         before the channel (default 0 Hz)\n"
    "  --No dBHz              AWGN noise density in dB/Hz (default -100)\n"
    "  --clip int16           Hilbert clipper magnitude (default 32767)\n"
    "  --ssbfilt 0|1          SSB bandwidth filter (default 1 on)\n"
    "  --complexout           Output complex int16 IQ (default real int16)\n"
    "\n"
    "  The printed SNR3k uses the ch.c convention: the faded analytic-signal\n"
    "  power over the complex noise power in 3 kHz, measured before the SSB\n"
    "  filter.  It equals the in-band SNR of the output with --ssbfilt 1 (the\n"
    "  default) or with --complexout.  With --ssbfilt 0 and real output it\n"
    "  reads 3.01 dB high.\n"
    "\n"
    "Standard presets (ITU-R F.1487 and CCIR 520-2 cases; Gaussian Doppler\n"
    "spectrum; doppler is the F.1487 spread = 2 sigma; two equal-power paths).\n"
    "Select one with --NAME or --preset NAME:\n";

static const char *USAGE_LEGACY =
    "\n"
    "Legacy presets (the 2nd-order Butterworth taps this tool shipped with up to\n"
    "and including release 1.9.18; doppler is the filter's 3 dB cutoff, which is\n"
    "the spectrum's rms width, so a legacy \"1 Hz\" fades about twice as fast as\n"
    "a standard 1 Hz preset; their delay line is one sample shorter than\n"
    "ceil(delay_ms * Fs / 1000), as it was).  Kept so that older results can be\n"
    "reproduced:\n";

static const char *USAGE_TAIL =
    "\n"
    "  Custom paths (repeat for up to %d paths in total, presets included):\n"
    "  --path delay_ms,doppler_hz,freq_hz,gain\n"
    "                         e.g. --path 2.0,1.0,0.0,0.5\n"
    "                         doppler_hz is the ITU-R Doppler spread (2 sigma of\n"
    "                         the Gaussian Doppler spectrum); 0 is a static path.\n"
    "  --legacy               Build every --path with the legacy Butterworth\n"
    "                         taps; doppler_hz is then the filter cutoff.  Works\n"
    "                         in any position.  Not allowed with a standard\n"
    "                         preset.  All paths of one run must use one\n"
    "                         spectrum, otherwise the tool stops with an error.\n"
    "  --preset NAME          Same as --NAME\n"
    "  --list-presets         Print the preset table to stdout and exit 0\n"
    "  --help                 Show this message and exit 0\n"
    "\n"
    "  The path report on stderr prints each path's delay as configured and as\n"
    "  realised (samples and ms) and the spectrum it was built with.\n"
    "\n"
    "Examples:\n"
    "  Basic AWGN only:  %s in.raw out.raw --No -95\n"
    "  CCIR Poor:        %s in.raw out.raw --ccir-poor --No -95\n"
    "  Custom 3 paths:   %s in.raw out.raw --path 0,0.5,0,0.4 --path 2,0.5,0,0.3 --path 4,1.0,0,0.2 --No -95\n";

/* The preset table, one group per spectrum, as "  name  title" lines. */
static void print_presets(FILE *f, int spectrum)
{
    int n;
    const watterson_preset_t *all = watterson_presets(&n);
    for (int q = 0; q < n; q++)
        if (all[q].spectrum == spectrum)
            fprintf(f, "  --%-14s %s\n", all[q].name, all[q].title);
}

static void helpmsg(FILE *f, const char *name)
{
    fprintf(f, USAGE_HEAD, name, name, name);
    print_presets(f, WATTERSON_SPECTRUM_GAUSSIAN);
    fputs(USAGE_LEGACY, f);
    print_presets(f, WATTERSON_SPECTRUM_LEGACY);
    fprintf(f, USAGE_TAIL, WATTERSON_MAX_PATHS, name, name, name);
}

/* getopt_long() accepts any unambiguous abbreviation of a long option
 * (--ccir-f for --ccir-flutter).  This tool does not: a bench passes
 * MERCURY_CH_FADING straight through as --NAME, and a misspelt name must fail,
 * not run a neighbour.  After a long option matched entry `name`, this finds
 * the argv word that carried it (argv[optind-1], or argv[optind-2] when
 * argv[optind-1] is the option's separately given argument) and checks that
 * the word is "--name" or "--name=...". */
static int long_option_is_exact(char **argv, const char *name)
{
    const char *word = argv[optind - 1];
    size_t      n    = strlen(name);
    if (optarg != NULL && optarg == word)
        word = argv[optind - 2];
    if (word[0] != '-' || word[1] != '-')
        return 1;                     /* a short option: nothing to check */
    word += 2;
    return strncmp(word, name, n) == 0 && (word[n] == '\0' || word[n] == '=');
}

static const char *spectrum_name(int spectrum)
{
    return spectrum == WATTERSON_SPECTRUM_LEGACY ? "legacy" : "gaussian";
}

/* The same, with what doppler_hz means under it, for the path report. */
static const char *spectrum_report(int spectrum)
{
    return spectrum == WATTERSON_SPECTRUM_LEGACY
               ? "legacy (doppler is the Butterworth cutoff)"
               : "gaussian (doppler is the 2-sigma spread)";
}

typedef struct
{
    float delay_ms, doppler_hz, freq_hz, gain;
} path_spec_t;

int main(int argc, char *argv[])
{
    FILE       *fin, *fout;
    watterson_t watterson;
    short       buf[BUF_N];
    float       htbuf[HT_N + BUF_N];
    COMP        ch_in[BUF_N];
    COMP        ch_fdm[BUF_N];
    COMP        ssbfiltbuf[SSBFILT_N + BUF_N];
    COMP        ssbfiltout[BUF_N];
    COMP        phase_ch;
    float       No;
    float       tx_pwr, tx_pwr_fade, noise_pwr;
    int         frames, i, j, k, Fs, nclipped, noutclipped, ssbfilt_en, complex_out;
    float       sam, peak, clip, papr, CNo, snr3k, gain, foff_hz;

    /* The channel description collected from the command line.  The model is
     * built ONCE, after every option is read, so --Fs, --legacy, a preset and
     * --path entries mean the same thing in any order. */
    const watterson_preset_t *preset      = NULL;
    const char               *preset_word = NULL;   /* how it was asked for */
    int                       legacy      = 0;
    path_spec_t               pspec[WATTERSON_MAX_PATHS];
    int                       npspec      = 0;
    int                       list_presets = 0, want_help = 0;

    /* ---- Defaults ---- */

    Fs          = 8000;
    foff_hz     = 0.0f;
    No          = -100.0f;
    clip        = 32767.0f;
    gain        = 1.0f;
    ssbfilt_en  = 1;
    complex_out = 0;

    /* ---- Command-line parsing (before any file is opened) ---- */

    {
        static const struct option fixed_opts[] =
        {
            {"complexout",   no_argument,       0, 'o'},
            {"clip",         required_argument, 0, 'c'},
            {"freq",         required_argument, 0, 'f'},
            {"Fs",           required_argument, 0, 'r'},
            {"gain",         required_argument, 0, 'g'},
            {"ssbfilt",      required_argument, 0, 's'},
            {"help",         no_argument,       0, 'h'},
            {"No",           required_argument, 0, 'n'},
            {"path",         required_argument, 0, 'P'},
            {"preset",       required_argument, 0, 'p'},
            {"legacy",       no_argument,       0, 'L'},
            {"list-presets", no_argument,       0, 'l'},
        };
        const int nfixed = (int)(sizeof(fixed_opts) / sizeof(fixed_opts[0]));
        int npre;
        const watterson_preset_t *pre = watterson_presets(&npre);
        /* The preset table is the one source of the preset names: each entry
         * becomes a long option here, so a preset added to the table is a
         * valid --NAME without a second list to keep in step. */
        struct option *long_opts = calloc((size_t)(nfixed + npre + 1), sizeof(*long_opts));
        if (long_opts == NULL)
        {
            fprintf(stderr, "%s: out of memory\n", argv[0]);
            return 1;
        }
        memcpy(long_opts, fixed_opts, sizeof(fixed_opts));
        for (i = 0; i < npre; i++)
        {
            long_opts[nfixed + i].name    = pre[i].name;
            long_opts[nfixed + i].has_arg = no_argument;
            long_opts[nfixed + i].flag    = 0;
            long_opts[nfixed + i].val     = PRESET_OPT_BASE + i;
        }

        for (;;)
        {
            int opt_idx = -1;
            int o = getopt_long(argc, argv, "c:f:g:hn:or:s:", long_opts, &opt_idx);
            if (o == -1)
                break;
            if (o == '?')
            {
                /* getopt_long() has already named the unknown or ambiguous
                 * option (or the missing argument) on stderr. */
                fprintf(stderr, "%s: see --help; --list-presets prints the preset names\n", argv[0]);
                free(long_opts);
                return 1;
            }
            if (opt_idx >= 0 && !long_option_is_exact(argv, long_opts[opt_idx].name))
            {
                fprintf(stderr,
                        "%s: '%s' is not a full option name (did you mean --%s?).\n"
                        "Option names must be typed in full; --list-presets prints the preset names.\n",
                        argv[0], argv[optind - 1], long_opts[opt_idx].name);
                free(long_opts);
                return 1;
            }
            if (o >= PRESET_OPT_BASE && o < PRESET_OPT_BASE + npre)
            {
                /* --NAME: the same as --preset NAME */
                optarg = (char *)pre[o - PRESET_OPT_BASE].name;
                o = 'p';
            }
            switch (o)
            {
            case 'c':
                clip = atof(optarg);
                break;
            case 'f':
                foff_hz = atof(optarg);
                break;
            case 'g':
                gain = atof(optarg);
                break;
            case 'n':
                No = atof(optarg);
                break;
            case 'o':
                complex_out = 1;
                break;
            case 'r':
                Fs = atoi(optarg);
                if (Fs <= 0)
                {
                    fprintf(stderr, "%s: --Fs needs a positive sample rate in Hz, not '%s'\n", argv[0], optarg);
                    free(long_opts);
                    return 1;
                }
                break;
            case 's':
                ssbfilt_en = atoi(optarg);
                break;
            case 'p':
                {
                    const watterson_preset_t *pr = watterson_preset_find(optarg);
                    if (!pr)
                    {
                        fprintf(stderr, "%s: unknown preset '%s'.  Known presets:\n", argv[0], optarg);
                        print_presets(stderr, WATTERSON_SPECTRUM_GAUSSIAN);
                        print_presets(stderr, WATTERSON_SPECTRUM_LEGACY);
                        free(long_opts);
                        return 1;
                    }
                    if (preset != NULL && preset != pr)
                    {
                        fprintf(stderr, "%s: two presets given ('%s' and '%s'); use one preset, plus --path for extra paths\n",
                                argv[0], preset->name, pr->name);
                        free(long_opts);
                        return 1;
                    }
                    preset      = pr;
                    preset_word = pr->name;
                }
                break;
            case 'L':
                legacy = 1;
                break;
            case 'P':
                {
                    path_spec_t ps;
                    if (sscanf(optarg, "%f,%f,%f,%f", &ps.delay_ms, &ps.doppler_hz, &ps.freq_hz, &ps.gain) != 4)
                    {
                        fprintf(stderr, "%s: invalid --path '%s'. Use: delay_ms,doppler_hz,freq_hz,gain\n", argv[0], optarg);
                        free(long_opts);
                        return 1;
                    }
                    if (npspec >= WATTERSON_MAX_PATHS)
                    {
                        fprintf(stderr, "%s: too many --path entries (max %d paths)\n", argv[0], WATTERSON_MAX_PATHS);
                        free(long_opts);
                        return 1;
                    }
                    pspec[npspec++] = ps;
                }
                break;
            case 'h':
                want_help = 1;
                break;
            case 'l':
                list_presets = 1;
                break;
            default:
                fprintf(stderr, "%s: unhandled option %d (internal error)\n", argv[0], o);
                free(long_opts);
                return 1;
            }
        }
        free(long_opts);
    }

    if (list_presets)
    {
        print_presets(stdout, WATTERSON_SPECTRUM_GAUSSIAN);
        print_presets(stdout, WATTERSON_SPECTRUM_LEGACY);
        return 0;
    }
    if (want_help)
    {
        helpmsg(stdout, argv[0]);
        return 0;
    }

    if (argc - optind != 2)
    {
        if (argc > 1)
            fprintf(stderr, "%s: expected InputFile and OutputFile, got %d file argument(s)\n\n",
                    argv[0], argc - optind);
        helpmsg(stderr, argv[0]);
        return 1;
    }

    /* ---- Build the channel, once, from everything collected above ---- */

    if (watterson_init(&watterson, Fs) < 0)
    {
        fprintf(stderr, "%s: watterson_init failed for Fs=%d\n", argv[0], Fs);
        return 1;
    }
    if (legacy)
        watterson_set_spectrum(&watterson, WATTERSON_SPECTRUM_LEGACY);
    if (preset != NULL)
    {
        if (legacy && preset->spectrum != WATTERSON_SPECTRUM_LEGACY)
        {
            fprintf(stderr,
                    "%s: --legacy cannot be combined with the standard preset '%s': its Doppler\n"
                    "spectrum is Gaussian by definition.  Use a legacy preset name, or --legacy\n"
                    "with --path entries, to build a Butterworth channel.\n",
                    argv[0], preset_word);
            return 1;
        }
        if (watterson_apply_preset(&watterson, preset) < 0)
        {
            fprintf(stderr, "%s: preset '%s' failed\n", argv[0], preset_word);
            return 1;
        }
    }
    for (i = 0; i < npspec; i++)
    {
        if (watterson_add_path(&watterson, pspec[i].delay_ms, pspec[i].doppler_hz,
                               pspec[i].freq_hz, pspec[i].gain) < 0)
        {
            if (watterson.num_paths >= WATTERSON_MAX_PATHS)
                fprintf(stderr, "%s: --path %g,%g,%g,%g: max paths (%d) exceeded\n", argv[0],
                        pspec[i].delay_ms, pspec[i].doppler_hz, pspec[i].freq_hz, pspec[i].gain,
                        WATTERSON_MAX_PATHS);
            else
                fprintf(stderr,
                        "%s: --path %g,%g,%g,%g rejected: delay_ms must be 0..10000, doppler_hz 0 or\n"
                        "above about Fs/2^36, gain 0..1, all values finite (see common/watterson.h)\n",
                        argv[0], pspec[i].delay_ms, pspec[i].doppler_hz, pspec[i].freq_hz, pspec[i].gain);
            watterson_dispose(&watterson);
            return 1;
        }
    }
    /* One spectrum per run.  A legacy preset plus a --path without --legacy
     * (or a Gaussian --path next to a legacy one) would fade the paths with
     * two different meanings of doppler_hz; refuse rather than report a
     * channel nobody asked for. */
    for (i = 1; i < watterson.num_paths; i++)
    {
        if (watterson.paths[i].spectrum != watterson.paths[0].spectrum)
        {
            fprintf(stderr,
                    "%s: the paths do not share one Doppler spectrum (path 0 is %s, path %d is %s).\n"
                    "Add --legacy so every --path uses the legacy taps, or use a standard preset.\n",
                    argv[0], spectrum_name(watterson.paths[0].spectrum), i,
                    spectrum_name(watterson.paths[i].spectrum));
            watterson_dispose(&watterson);
            return 1;
        }
    }

    /* ---- Open I/O files ---- */

    if (strcmp(argv[optind], "-") == 0)
        fin = stdin;
    else if ((fin = fopen(argv[optind], "rb")) == NULL)
    {
        fprintf(stderr, "Error opening input file '%s': %s\n", argv[optind], strerror(errno));
        watterson_dispose(&watterson);
        return 1;
    }

    if (strcmp(argv[optind + 1], "-") == 0)
        fout = stdout;
    else if ((fout = fopen(argv[optind + 1], "wb")) == NULL)
    {
        fprintf(stderr, "Error opening output file '%s': %s\n", argv[optind + 1], strerror(errno));
        watterson_dispose(&watterson);
        return 1;
    }

    /* Report the fading seed on stderr (stdout may be the sample stream).
     *
     * watterson_init() has already seeded the model -- from
     * MERCURY_WATTERSON_SEED if set, otherwise the clock and pid.  Printing it
     * is what makes an unpinned run reproducible after the fact: re-run with
     * MERCURY_WATTERSON_SEED set to this value and the fade replays exactly. */
    fprintf(stderr, "watterson: fading seed %u%s\n",
            watterson_get_seed(&watterson),
            getenv("MERCURY_WATTERSON_SEED") ? " (pinned)" : "");

    /* The AWGN level, like everything else, is applied after all options are
     * parsed, so --No takes effect regardless of its position. */
    watterson_set_noise(&watterson, No);

    /* ---- Initialisation ---- */

    phase_ch.real = 1.0f;
    phase_ch.imag = 0.0f;

    // noise variance for internal AWGN in watterson_process
    // (already set via watterson_set_noise)

    tx_pwr = tx_pwr_fade = noise_pwr = 0.0f;
    noutclipped = 0;
    nclipped = 0;
    peak = 0.0f;

    /* Clear Hilbert transform buffer */
    for (i = 0; i < HT_N; i++)
        htbuf[i] = 0.0f;

    /* Clear SSB filter buffer */
    for (i = 0; i < SSBFILT_N; i++)
    {
        ssbfiltbuf[i].real = 0.0f;
        ssbfiltbuf[i].imag = 0.0f;
    }

    /* SSB filter local oscillator (centre frequency = 1500 Hz) */
    COMP lo_phase = {1.0f, 0.0f};
    COMP lo_freq;
    lo_freq.real = cosf(2.0f * M_PI * SSBFILT_CENTRE / Fs);
    lo_freq.imag = sinf(2.0f * M_PI * SSBFILT_CENTRE / Fs);

    fprintf(stderr,
            "watterson_test: Fs=%d No=%4.2f dB/Hz foff=%4.2f Hz clip=%4.2f ssbfilt=%d complexout=%d\n",
            Fs, No, foff_hz, clip, ssbfilt_en, complex_out);

    if (watterson.num_paths > 0)
    {
        if (preset != NULL)
            fprintf(stderr, "  Preset %s: %s\n", preset->name, preset->title);
        for (i = 0; i < watterson.num_paths; i++)
        {
            const watterson_path_t *wp = &watterson.paths[i];
            /* delay_samples is the realised line length: ceil(delay * Fs / 1000)
             * for the Gaussian spectrum, one less for the legacy one. */
            fprintf(stderr,
                    "  Path %d: delay=%.3f ms (%d samples = %.4f ms) doppler=%.2f Hz spectrum=%s freq=%.1f Hz gain=%.3f\n",
                    i,
                    wp->delay_ms,
                    wp->delay_samples,
                    wp->delay_samples * 1000.0 / Fs,
                    wp->doppler_hz,
                    spectrum_report(wp->spectrum),
                    wp->freq_hz,
                    wp->gain);
        }
    }
    else
    {
        fprintf(stderr, "  AWGN only (no multipath)\n");
    }

    /* ---- Main processing loop ---- */

    frames = 0;
    while (fread(buf, sizeof(short), BUF_N, fin) == BUF_N)
    {
        frames++;

        /* Hilbert Transform: real int16 -> complex analytic signal */
        for (i = 0, j = HT_N; i < BUF_N; i++, j++)
        {
            htbuf[j] = (float)buf[i] * gain;

            ch_in[i].real = 0.0f;
            ch_in[i].imag = 0.0f;

            for (k = 0; k < HT_N; k++)
            {
                ch_in[i].real += htbuf[j - k] * ht_coeff[k].real;
                ch_in[i].imag += htbuf[j - k] * ht_coeff[k].imag;
            }
        }

        /* Update Hilbert transform overlap buffer */
        for (i = 0; i < HT_N; i++)
            htbuf[i] = htbuf[i + BUF_N];

        /* Clipping: limit complex signal magnitude */
        for (i = 0; i < BUF_N; i++)
        {
            float mag = sqrtf(ch_in[i].real * ch_in[i].real +
                              ch_in[i].imag * ch_in[i].imag);
            float angle = atan2f(ch_in[i].imag, ch_in[i].real);

            if (mag > clip)
            {
                mag = clip;
                nclipped++;
            }
            tx_pwr += mag * mag;

            if ((frames - 1) * BUF_N >= HT_N && mag > peak)
                peak = mag;

            ch_in[i].real = mag * cosf(angle);
            ch_in[i].imag = mag * sinf(angle);
        }

        /* Frequency offset (pre-Watterson shift) */
        {
            float tau = 2.0f * M_PI;
            float result = tau * foff_hz / Fs;
            COMP foff_rect;
            foff_rect.real = cosf(result);
            foff_rect.imag = sinf(result);

            for (i = 0; i < BUF_N; i++)
            {
                phase_ch = cmult(phase_ch, foff_rect);
                ch_fdm[i] = cmult(ch_in[i], phase_ch);
            }

            /* Normalise oscillator */
            float mag = cabsolute(phase_ch);
            phase_ch.real /= mag;
            phase_ch.imag /= mag;
        }

        /* ---- Apply Watterson model ---- */
        watterson_process(&watterson, ch_fdm, BUF_N);

        /* Measure power after fading */
        for (i = 0; i < BUF_N; i++)
        {
            tx_pwr_fade += ch_fdm[i].real * ch_fdm[i].real;
        }

        /* ---- SSB filter (output shaping) ---- */
        for (i = 0, j = SSBFILT_N; i < BUF_N; i++, j++)
        {
            if (ssbfilt_en)
            {
                int kk;
                ssbfiltbuf[j] = cmult(ch_fdm[i], cconj(lo_phase));
                ssbfiltout[i].real = 0.0f;
                ssbfiltout[i].imag = 0.0f;

                for (kk = 0; kk < SSBFILT_N; kk++)
                {
                    ssbfiltout[i].real += ssbfiltbuf[j - kk].real * ssbfilt_coeff[kk];
                    ssbfiltout[i].imag += ssbfiltbuf[j - kk].imag * ssbfilt_coeff[kk];
                }

                ssbfiltout[i] = cmult(ssbfiltout[i], lo_phase);
                lo_phase = cmult(lo_phase, lo_freq);
            }
            else
            {
                ssbfiltout[i] = ch_fdm[i];
            }
        }

        /* Update SSB filter memory */
        for (i = 0; i < SSBFILT_N; i++)
            ssbfiltbuf[i] = ssbfiltbuf[i + BUF_N];

        /* ---- Output ---- */
        {
            int nout = (complex_out + 1) * BUF_N;
            short bufout[nout];
            short *pout = bufout;

            for (i = 0; i < BUF_N; i++)
            {
                sam = ssbfiltout[i].real;
                if (sam > 32767.0f)  { noutclipped++; sam = 32767.0f; }
                if (sam < -32767.0f) { noutclipped++; sam = -32767.0f; }
                *pout++ = (short)sam;

                if (complex_out)
                {
                    sam = ssbfiltout[i].imag;
                    if (sam > 32767.0f)  { noutclipped++; sam = 32767.0f; }
                    if (sam < -32767.0f) { noutclipped++; sam = -32767.0f; }
                    *pout++ = (short)sam;
                }
            }

            fwrite(bufout, sizeof(short), nout, fout);
            if (fout == stdout) fflush(stdout);
        }
    }

    /* ---- Summary ---- */

    {
        int nsamples = frames * BUF_N;
        float outclipped_percent;
        papr = 10.0f * log10f(peak * peak / (tx_pwr / nsamples));
        /* SNR from the model's measured pre-noise signal power and actual
         * added-noise power (the previous tx_pwr_fade was sampled AFTER the
         * AWGN was added, so it reported signal+noise and pinned the SNR). */
        snr3k = watterson_measured_snr3k(&watterson);
        CNo   = snr3k + 10.0f * log10f(3000.0f);
        (void)tx_pwr_fade;
        outclipped_percent = noutclipped * 100.0f / nsamples;

        fprintf(stderr, "SNR3k(dB): %8.2f  C/No....: %8.2f\n", snr3k, CNo);
        fprintf(stderr, "peak.....: %8.2f  RMS.....: %8.2f   PAPR.....: %5.2f dB\n",
                peak, sqrtf(tx_pwr / nsamples), papr);
        fprintf(stderr, "Nsamples.: %8d  clipped.: %8.2f%%  OutClipped: %5.2f%%\n",
                nsamples, nclipped * 100.0f / nsamples, outclipped_percent);
        if (outclipped_percent > 0.1f)
            fprintf(stderr, "WARNING: output clipping\n");
    }

    fclose(fin);
    fclose(fout);
    watterson_dispose(&watterson);

    return 0;
}
