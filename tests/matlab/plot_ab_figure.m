% plot_ab_figure — end-to-end A/B of the LDPC decoder through two mercury
% stations on the seeded -x sock bench (tests/integration/ab_sweep.sh).
%
% Reads docs/ldpc-figures/data/ab_runs.csv (one row per run, written by the
% sweep aggregator: set, arm, No_dBHz, payload_kb, seed, result, wall_s,
% snr_ab, snr_ba, vtime_s, keys_a, keys_b, iters) and writes
% docs/ldpc-figures/end_to_end_ab.svg: left, link time to deliver the 4 KB
% harness payload across the DATAC3/DATAC1 region; right, the 102-byte
% transfer at the ladder floor.  Arms: trunk = the pristine v1.9.17 binary on
% both stations; new = this change; cap10 = this tree forced to the embedded
% build's decoder (legacy, 10 iterations, fixed-EsNo LLRs).  The x axis is the
% bench's MEASURED in-burst SNR3k on the payload direction (A->B); the y axis
% is virtual time of the last key-up (link occupancy), which the lockstep
% bench reproduces to about one 20 ms block.  Markers are the mean over seeds,
% bars the min..max.
%
% Copyright (C) 2026 Joseph Freivald
% SPDX-License-Identifier: GPL-3.0-or-later
function plot_ab_figure(png_dir)
    if nargin < 1, png_dir = ''; end
    here = fileparts(mfilename('fullpath'));
    root = fileparts(fileparts(here));
    D = fullfile(root, 'docs', 'ldpc-figures', 'data');
    O = fullfile(root, 'docs', 'ldpc-figures');

    C.blue = '#2a78d6'; C.orange = '#eb6834'; C.aqua = '#1baf7a'; C.yellow = '#eda100';
    C.surface = '#fcfcfb'; C.ink = '#0b0b0b'; C.ink2 = '#52514e'; C.grid = '#e4e3df';
    arms = {'trunk', 'Deployed: trunk v1.9.17 binary (legacy, table cap 100, fixed-EsNo LLRs)', C.orange, 'o'; ...
            'emul',  'Embedded-build decoder forced: legacy, cap 10, fixed-EsNo LLRs',          C.yellow, 's'; ...
            'new',   'New decoder (this change)',                                              C.blue,   'o'};

    T = readtable(fullfile(D, 'ab_runs.csv'), 'TextType', 'string', 'Delimiter', ',');
    T = T(T.result == "pass" | T.result == "fail", :);

    f = figure('Visible', 'off', 'Color', C.surface, 'Units', 'pixels', 'Position', [100 100 1040 440]);
    tl = tiledlayout(f, 1, 2, 'TileSpacing', 'compact', 'Padding', 'compact');

    ax = nexttile(tl); hold(ax, 'on');
    h = panel(ax, T(T.payload_kb == 4, :), arms, C, 0.12);
    xlabel(ax, 'measured in-burst SNR3k, payload direction (dB)');
    ylabel(ax, 'link time to deliver, last key-up (s)');
    title(ax, {'4 KB harness payload (compressible): DATAC3 / DATAC1 region', 'ladder thresholds: DATAC3 -1 dB, DATAC1 +3 dB'}, 'Color', C.ink, 'FontWeight', 'normal');
    finish(ax, C);

    ax = nexttile(tl); hold(ax, 'on');
    panel(ax, T(T.payload_kb == 0, :), arms, C, 0.12);
    xlabel(ax, 'measured in-burst SNR3k, payload direction (dB)');
    ylabel(ax, 'link time to deliver, last key-up (s)');
    title(ax, {'102-byte payload at the ladder floor', 'below about -8 dB the connect rides the MFSK plane (untouched)'}, 'Color', C.ink, 'FontWeight', 'normal');
    finish(ax, C);

    lg = legend(h, 'Location', 'southoutside', 'Orientation', 'horizontal', 'NumColumns', 1);
    lg.Layout.Tile = 'south'; lg.Box = 'off'; lg.TextColor = C.ink2;

    exportgraphics(f, fullfile(O, 'end_to_end_ab.svg'), 'ContentType', 'vector', 'BackgroundColor', C.surface);
    if ~isempty(png_dir)
        if ~isfolder(png_dir), mkdir(png_dir); end
        exportgraphics(f, fullfile(png_dir, 'end_to_end_ab.png'), 'Resolution', 110, 'BackgroundColor', C.surface);
    end
    close(f);
    fprintf('wrote %s\n', fullfile(O, 'end_to_end_ab.svg'));
end

function h = panel(ax, S, arms, C, dx)
    h = gobjects(size(arms,1),1);
    for a = 1:size(arms,1)
        R = S(S.arm == arms{a,1}, :);
        nos = unique(R.No_dBHz);
        x = zeros(size(nos)); y = x; lo = x; hi = x; nfail = x;
        for i = 1:numel(nos)
            Q = R(R.No_dBHz == nos(i), :);
            x(i) = mean(Q.snr_ab, 'omitnan') + (a - 2) * dx;   % small horizontal offset per arm
            v = Q.vtime_s; y(i) = mean(v); lo(i) = min(v); hi(i) = max(v);
            nfail(i) = sum(Q.result ~= "pass");
        end
        [x, k] = sort(x); y = y(k); lo = lo(k); hi = hi(k); nfail = nfail(k);
        errorbar(ax, x, y, y - lo, hi - y, 'LineStyle', 'none', 'Color', arms{a,3}, 'LineWidth', 1.0, 'CapSize', 4);
        h(a) = plot(ax, x, y, ['-' arms{a,4}], 'Color', arms{a,3}, 'LineWidth', 1.6, 'MarkerSize', 6, ...
                    'MarkerFaceColor', arms{a,3}, 'MarkerEdgeColor', C.surface, 'DisplayName', arms{a,2});
        if any(nfail)
            plot(ax, x(nfail > 0), y(nfail > 0), 'x', 'Color', C.ink, 'LineWidth', 1.4, 'MarkerSize', 9, 'HandleVisibility', 'off');
        end
    end
end

function finish(ax, C)
    set(ax, 'Color', C.surface, 'XColor', C.ink2, 'YColor', C.ink2, 'GridColor', C.grid, 'GridAlpha', 1, ...
        'MinorGridLineStyle', 'none', 'Box', 'off', 'TickDir', 'out', 'TickLength', [0.006 0.006], ...
        'FontName', 'Helvetica', 'FontSize', 10, 'LineWidth', 0.6, 'Layer', 'top');
    grid(ax, 'on');
    ax.Title.FontSize = 12; ax.XLabel.Color = C.ink2; ax.YLabel.Color = C.ink2;
end
