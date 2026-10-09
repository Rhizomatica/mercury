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

static const char *USAGE =
    "Command line HF channel simulation tool using the Watterson model.\n"
    "\n"
    "usage: %s InputFile OutputFile [Options]\n"
    "\n"
    "  real int16 input -> Gain -> Hilbert Transform -> Clipper ->\n"
    "  Frequency Shift -> Watterson Model -> AWGN -> SSB Filter ->\n"
    "  real int16 output\n"
    "\n"
    "Options:\n"
    "  --Fs SampleRateHz      Sample rate (default 8000)\n"
    "  --gain G               Linear gain (default 1.0)\n"
    "  --freq FoffHz          Frequency offset (default 0 Hz)\n"
    "  --No dBHz              AWGN noise density in dB/Hz (default -100)\n"
    "  --clip int16           Hilbert clipper magnitude (default 32767)\n"
    "  --ssbfilt 0|1          SSB bandwidth filter (default 1 on)\n"
    "  --complexout           Output complex int16 IQ (default real int16)\n"
    "\n"
    "  Watterson channel presets:\n"
    "  --good                 ITU-R Good (0.1 Hz Doppler, 0.5 ms, 1 path)\n"
    "  --moderate             ITU-R Moderate (1.0 Hz Doppler, 1.0 ms, 2 paths)\n"
    "  --poor                 ITU-R Poor (1.0 Hz Doppler, 2.0 ms, 2 paths)\n"
    "  --flutter              ITU-R Flutter (10.0 Hz Doppler, 2.0 ms, 2 paths)\n"
    "\n"
    "  Custom path definition (can be repeated for multiple paths):\n"
    "  --path delay_ms,doppler_hz,freq_hz,gain\n"
    "                         e.g. --path 2.0,1.0,0.0,0.5\n"
    "                         doppler_hz is the ITU-R Doppler spread (2 sigma of the\n"
    "                         Gaussian Doppler spectrum); with --legacy before it, the\n"
    "                         cutoff of the pre-1.9.18 Butterworth tap filter instead\n"
    "\n"
    "  Standard presets (Gaussian Doppler spectrum, two equal-power paths):\n"
    "  --preset NAME | --NAME NAME = ccir-good | ccir-moderate | ccir-poor | ccir-flutter\n"
    "                         itur-lq | itur-lm | itur-ld | itur-mq | itur-mm | itur-md | itur-mdv\n"
    "                         itur-hq | itur-hm | itur-hd   (--preset ? lists them)\n"
    "  Legacy presets (Butterworth taps; the numbers in docs/MODES.md):\n"
    "  --good --moderate --poor --flutter --mpg --mpp --mpd\n"
    "\n"
    "  --help                 Show this message\n"
    "\n"
    "Examples:\n"
    "  Basic AWGN only:  %s in.raw out.raw --No -95\n"
    "  CCIR Poor:        %s in.raw out.raw --ccir-poor --No -95\n"
    "  Custom 3 paths:   %s in.raw out.raw --path 0,0.5,0,0.4 --path 2,0.5,0,0.3 --path 4,1.0,0,0.2 --No -95\n";

static void helpmsg(const char *name)
{
    fprintf(stderr, USAGE, name, name, name, name);
}

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
    int         watterson_configured;

    if (argc < 3)
    {
        helpmsg(argv[0]);
        return 1;
    }

    /* ---- Open I/O files ---- */

    if (strcmp(argv[1], "-") == 0)
        fin = stdin;
    else if ((fin = fopen(argv[1], "rb")) == NULL)
    {
        fprintf(stderr, "Error opening input file '%s': %s\n", argv[1], strerror(errno));
        return 1;
    }

    if (strcmp(argv[2], "-") == 0)
        fout = stdout;
    else if ((fout = fopen(argv[2], "wb")) == NULL)
    {
        fprintf(stderr, "Error opening output file '%s': %s\n", argv[2], strerror(errno));
        return 1;
    }

    /* ---- Defaults ---- */

    Fs          = 8000;
    foff_hz     = 0.0f;
    No          = -100.0f;
    clip        = 32767.0f;
    gain        = 1.0f;
    ssbfilt_en  = 1;
    complex_out = 0;
    watterson_configured = 0;

    /* ---- Command-line parsing ---- */

    {
        int opt_idx = 0;
        int o;
        static struct option long_opts[] =
        {
            {"complexout",  no_argument,       0, 'o'},
            {"clip",        required_argument, 0, 'c'},
            {"freq",        required_argument, 0, 'f'},
            {"Fs",          required_argument, 0, 'r'},
            {"gain",        required_argument, 0, 'g'},
            {"ssbfilt",     required_argument, 0, 's'},
            {"help",        no_argument,       0, 'h'},
            {"No",          required_argument, 0, 'n'},
            /* the pre-1.9.18 presets, now entries of the preset table with the
             * legacy (Butterworth) spectrum so their results reproduce */
            {"good",        no_argument,       0, 'p'},
            {"moderate",    no_argument,       0, 'p'},
            {"poor",        no_argument,       0, 'p'},
            {"flutter",     no_argument,       0, 'p'},
            {"path",        required_argument, 0, 'P'},
            {"preset",      required_argument, 0, 'p'},
            {"legacy",      no_argument,       0, 'L'},
            /* every named preset is also a long option, so a bench can pass
             * MERCURY_CH_FADING=ccir-poor as --ccir-poor */
            {"ccir-good", no_argument, 0, 'p'}, {"ccir-moderate", no_argument, 0, 'p'},
            {"ccir-poor", no_argument, 0, 'p'}, {"ccir-flutter",  no_argument, 0, 'p'},
            {"itur-lq", no_argument, 0, 'p'}, {"itur-lm", no_argument, 0, 'p'}, {"itur-ld", no_argument, 0, 'p'},
            {"itur-mq", no_argument, 0, 'p'}, {"itur-mm", no_argument, 0, 'p'}, {"itur-md", no_argument, 0, 'p'},
            {"itur-mdv", no_argument, 0, 'p'},
            {"itur-hq", no_argument, 0, 'p'}, {"itur-hm", no_argument, 0, 'p'}, {"itur-hd", no_argument, 0, 'p'},
            {"mpg", no_argument, 0, 'p'}, {"mpp", no_argument, 0, 'p'}, {"mpd", no_argument, 0, 'p'},
            {0, 0, 0, 0}
        };

        while ((o = getopt_long(argc, argv, "c:f:g:hn:or:s:", long_opts, &opt_idx)) != -1)
        {
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
                break;
            case 's':
                ssbfilt_en = atoi(optarg);
                break;
            case 'p':
                {
                    const char *pname = optarg ? optarg : long_opts[opt_idx].name;
                    const watterson_preset_t *pr = watterson_preset_find(pname);
                    if (!pr)
                    {
                        fprintf(stderr, "Unknown preset '%s'. Known presets:\n", pname);
                        int n; const watterson_preset_t *all = watterson_presets(&n);
                        for (int q = 0; q < n; q++) fprintf(stderr, "  %-14s %s\n", all[q].name, all[q].title);
                        return 1;
                    }
                    watterson_init(&watterson, Fs);
                    if (watterson_apply_preset(&watterson, pr) < 0)
                    {
                        fprintf(stderr, "preset '%s' failed\n", pname);
                        return 1;
                    }
                    watterson_configured = 1;
                }
                break;
            case 'L':
                if (!watterson_configured)
                {
                    watterson_init(&watterson, Fs);
                    watterson_configured = 1;
                }
                watterson_set_spectrum(&watterson, WATTERSON_SPECTRUM_LEGACY);
                break;
            case 'P':
                {
                    float p_delay, p_doppler, p_freq, p_gain;
                    if (sscanf(optarg, "%f,%f,%f,%f", &p_delay, &p_doppler, &p_freq, &p_gain) != 4)
                    {
                        fprintf(stderr, "Invalid --path format. Use: delay_ms,doppler_hz,freq_hz,gain\n");
                        return 1;
                    }
                    if (!watterson_configured)
                    {
                        watterson_init(&watterson, Fs);
                        watterson_configured = 1;
                    }
                    if (watterson_add_path(&watterson, p_delay, p_doppler, p_freq, p_gain) < 0)
                    {
                        fprintf(stderr, "Max paths (%d) exceeded.\n", WATTERSON_MAX_PATHS);
                        return 1;
                    }
                }
                break;
            case 'h':
            case '?':
                helpmsg(argv[0]);
                return 0;
            }
        }
    }

    /* If no Watterson preset was selected, init with no paths (AWGN only). */
    if (!watterson_configured)
        watterson_init(&watterson, Fs);

    /* Report the fading seed on stderr (stdout may be the sample stream).
     *
     * watterson_init() has already seeded the model -- from
     * MERCURY_WATTERSON_SEED if set, otherwise the clock and pid.  Printing it
     * is what makes an unpinned run reproducible after the fact: re-run with
     * MERCURY_WATTERSON_SEED set to this value and the fade replays exactly. */
    fprintf(stderr, "watterson: fading seed %u%s\n",
            watterson_get_seed(&watterson),
            getenv("MERCURY_WATTERSON_SEED") ? " (pinned)" : "");

    /* Apply the AWGN level ONCE, after all options are parsed — so --No takes
     * effect regardless of its position relative to a preset/--path (getopt
     * may permute argv, and presets must not bake in a stale No). */
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
        for (i = 0; i < watterson.num_paths; i++)
        {
            fprintf(stderr,
                    "  Path %d: delay=%.1f ms doppler=%.1f Hz freq=%.1f Hz gain=%.3f\n",
                    i,
                    watterson.paths[i].delay_ms,
                    watterson.paths[i].doppler_hz,
                    watterson.paths[i].freq_hz,
                    watterson.paths[i].gain);
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
