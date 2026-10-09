% plot_ldpc_figures — regenerate docs/ldpc-figures/*.svg from docs/ldpc-figures/data.
%
% Run from anywhere: it locates the repo from its own path.  Produces twelve
% static SVG figures (GitHub renders them in Markdown): seven per-code FER
% waterfalls, the decibel gain summary, the LLR-scale cost at the real
% modulations, the min-sum scaling sweep, decoder speed and working memory.
% Optional second output directory for PNG previews: plot_ldpc_figures('png_dir').
%
% Copyright (C) 2026 Joseph Freivald
% SPDX-License-Identifier: GPL-3.0-or-later
function plot_ldpc_figures(png_dir)
    if nargin < 1, png_dir = ''; end
    here = fileparts(mfilename('fullpath'));
    root = fileparts(fileparts(here));
    D = fullfile(root, 'docs', 'ldpc-figures', 'data');
    O = fullfile(root, 'docs', 'ldpc-figures');

    % ---- palette (validated categorical order, light surface) ----
    C.blue = '#2a78d6'; C.orange = '#eb6834'; C.aqua = '#1baf7a'; C.yellow = '#eda100';
    C.blue250 = '#86b6ef'; C.surface = '#fcfcfb'; C.ink = '#0b0b0b'; C.ink2 = '#52514e'; C.grid = '#e4e3df';
    % entity -> colour, held fixed across every figure
    E.new = C.blue; E.deployed = C.orange; E.golden = C.aqua; E.legacy50 = C.yellow;

    codes = {'HRA_56_56','DATAC14', 'nms16'; 'H_128_256_5','DATAC0', 'nms16'; 'H_256_512_4','DATAC13', 'nms16'; ...
             'H_256_768_22','DATAC15 / DATAC16', 'nms16'; 'H_1024_2048_4f','DATAC3 / DATAC4', 'spt'; ...
             'H_4096_8192_3d','DATAC1', 'nms16'; 'H_16200_9720','DATAC17 / QAM16C2', 'spt'};
    algname = containers.Map({'nms16','spt'}, {'int16 vector min-sum', 'table sum-product'});
    tex = @(t) strrep(t, '_', '\_');   % code names contain underscores; axis labels use TeX subscripts

    gain1 = nan(size(codes,1),1); gain2 = gain1; caponly = false(size(codes,1),1);
    % ---- 1-7: per-code waterfalls ----
    for i = 1:size(codes,1)
        code = codes{i,1}; T = readtable(fullfile(D, ['ldpc_fer_' code '.csv']));
        x = T.EsNo_dB;
        if strcmp(codes{i,3}, 'nms16'), newcol = 'C_nms16_50'; else, newcol = 'C_spt50'; end
        series = {T.C_leg10_fixedEsNo, 'Deployed today: 10 iterations, fixed-EsNo LLRs', E.deployed; ...
                  T.C_leg50,           'Legacy decoder, 50 iterations',                  E.legacy50; ...
                  T.M_bp50,            'MATLAB belief propagation, 50 (golden)',         E.golden; ...
                  T.(newcol),          ['New: ' algname(codes{i,3}) ', 50'],              E.new};
        f = newfig(C);
        ax = gca; hold(ax, 'on');
        yline(ax, 1e-1, '--', 'Color', C.ink2, 'LineWidth', 0.75, 'Alpha', 0.6);
        yline(ax, 1e-2, '--', 'Color', C.ink2, 'LineWidth', 0.75, 'Alpha', 0.6);
        h = gobjects(4,1);
        for s = 1:4
            y = series{s,1}; y(y <= 0) = NaN;
            h(s) = plot(ax, x, y, '-o', 'Color', series{s,3}, 'LineWidth', 1.6, 'MarkerSize', 5.5, ...
                        'MarkerFaceColor', series{s,3}, 'MarkerEdgeColor', C.surface, 'DisplayName', series{s,2});
        end
        set(ax, 'YScale', 'log'); ylim(ax, [5e-3 1.2]); yticks(ax, [1e-2 1e-1 1]); yticklabels(ax, {'1%','10%','100%'});
        xlim(ax, [min(x) max(x)]);
        gain1(i) = golden.esno_at_fer(x, series{1,1}, 1e-1) - golden.esno_at_fer(x, series{4,1}, 1e-1);
        gain2(i) = golden.esno_at_fer(x, series{1,1}, 1e-2) - golden.esno_at_fer(x, series{4,1}, 1e-2);
        caponly(i) = false;
        if isnan(gain1(i)) && isnan(gain2(i))
            % the deployed curve never decodes on this grid (the 16200-bit code's
            % 10 dB constant is ~10 dB off in a BPSK test): report the cap-only gain
            gain1(i) = golden.esno_at_fer(x, T.C_leg10, 1e-1) - golden.esno_at_fer(x, series{4,1}, 1e-1);
            gain2(i) = golden.esno_at_fer(x, T.C_leg10, 1e-2) - golden.esno_at_fer(x, series{4,1}, 1e-2);
            caponly(i) = true;
        end
        if isnan(gain2(i)), gtxt = sprintf('%.2f dB at FER 10%%', gain1(i)); else, gtxt = sprintf('%.2f dB at FER 1%%', gain2(i)); end
        if caponly(i), gtxt = [gtxt ' (cap only*)']; end
        title(ax, sprintf('%s  (%s): frame error rate, BPSK over AWGN', tex(code), codes{i,2}), 'Color', C.ink, 'FontWeight', 'normal');
        if caponly(i)
            subtitle(ax, {sprintf('deployed -> new decoder: %s recovered', gtxt), '* its 10 dB LLR constant never decodes in a BPSK test; gain vs legacy at 10 iterations, exact LLRs'}, 'Color', C.ink2);
        else
            subtitle(ax, sprintf('deployed -> new decoder: %s recovered', gtxt), 'Color', C.ink2);
        end
        xlabel(ax, 'E_s/N_0 (dB)'); ylabel(ax, 'frame error rate');
        legend(ax, h, 'Location', 'southwest', 'Box', 'off', 'TextColor', C.ink2);
        finish(ax, C); hold(ax, 'off');
        save(f, O, png_dir, ['fer_' code]);
    end

    % ---- 8: gain summary ----
    f = newfig(C, 760, 420); ax = gca; hold(ax, 'on');
    y = 1:size(codes,1);
    b = barh(ax, y, [gain1 gain2], 0.72, 'EdgeColor', 'none');
    b(1).FaceColor = C.blue250; b(1).DisplayName = 'at FER 10%';
    b(2).FaceColor = E.new;    b(2).DisplayName = 'at FER 1%';
    for i = y
        star = ''; if caponly(i), star = '*'; end
        if ~isnan(gain1(i)), text(ax, gain1(i) + 0.03, i - 0.18, sprintf('%.2f%s', gain1(i), star), 'Color', C.ink2, 'FontSize', 9, 'VerticalAlignment', 'middle'); end
        if ~isnan(gain2(i)), text(ax, gain2(i) + 0.03, i + 0.18, sprintf('%.2f%s', gain2(i), star), 'Color', C.ink, 'FontSize', 9, 'VerticalAlignment', 'middle'); end
    end
    yticks(ax, y); yticklabels(ax, cellfun(@(a,b) sprintf('%s  (%s)', tex(a), b), codes(:,1), codes(:,2), 'UniformOutput', false));
    set(ax, 'YDir', 'reverse'); xlim(ax, [0 2.2]);
    xlabel(ax, 'E_s/N_0 recovered, deployed decoder -> new decoder (dB)');
    title(ax, 'Coding gain returned per code', 'Color', C.ink, 'FontWeight', 'normal');
    subtitle(ax, {'iteration cap 10 -> 50 and calibrated LLR scale, BPSK over AWGN, 600 frames per point', '* cap-only gain: that mode''s 10 dB LLR constant never decodes in a BPSK test'}, 'Color', C.ink2);
    legend(ax, b, 'Location', 'northeast', 'Box', 'off', 'TextColor', C.ink2);
    finish(ax, C); hold(ax, 'off'); save(f, O, png_dir, 'gain_summary');

    % ---- 9: LLR scale cost at real modulation ----
    f = newfig(C, 980, 360); tl = tiledlayout(f, 1, 3, 'TileSpacing', 'compact', 'Padding', 'compact');
    sc = {'H_256_768_22','DATAC15/16, QPSK','3 dB'; 'H_4096_8192_3d','DATAC1, QPSK','3 dB'; 'H_16200_9720','QAM16C2, 16-QAM','10 dB'};
    for i = 1:3
        T = readtable(fullfile(D, ['llr_scale_' sc{i,1} '.csv'])); x = T.EsNo_dB;
        ax = nexttile(tl); hold(ax, 'on');
        yline(ax, 1e-1, '--', 'Color', C.ink2, 'LineWidth', 0.75, 'Alpha', 0.6);
        yline(ax, 1e-2, '--', 'Color', C.ink2, 'LineWidth', 0.75, 'Alpha', 0.6);
        y1 = T.true; y1(y1 <= 0) = NaN; y2 = T.fixed; y2(y2 <= 0) = NaN;
        h1 = plot(ax, x, y2, '-o', 'Color', E.deployed, 'LineWidth', 1.6, 'MarkerSize', 5.5, 'MarkerFaceColor', E.deployed, 'MarkerEdgeColor', C.surface, 'DisplayName', ['fixed constant (' sc{i,3} ')']);
        h2 = plot(ax, x, y1, '-o', 'Color', E.new, 'LineWidth', 1.6, 'MarkerSize', 5.5, 'MarkerFaceColor', E.new, 'MarkerEdgeColor', C.surface, 'DisplayName', 'calibrated (true E_s/N_0)');
        set(ax, 'YScale', 'log'); ylim(ax, [5e-3 1.2]); yticks(ax, [1e-2 1e-1 1]); yticklabels(ax, {'1%','10%','100%'}); xlim(ax, [min(x) max(x)]);
        g = golden.esno_at_fer(x, T.fixed, 1e-1) - golden.esno_at_fer(x, T.true, 1e-1);
        title(ax, sprintf('%s (%s)', tex(sc{i,1}), sc{i,2}), 'Color', C.ink, 'FontWeight', 'normal');
        subtitle(ax, sprintf('fixed scale costs %.2f dB at FER 10%%', g), 'Color', C.ink2);
        xlabel(ax, 'E_s/N_0 per symbol (dB)'); if i == 1, ylabel(ax, 'frame error rate'); end
        if i == 3, legend(ax, [h1 h2], 'Location', 'southwest', 'Box', 'off', 'TextColor', C.ink2); end
        finish(ax, C); hold(ax, 'off');
    end
    title(tl, 'LLR scale: mode constant versus calibrated E_s/N_0, table sum-product at 50 iterations', 'Color', C.ink);
    save(f, O, png_dir, 'llr_scale_cost');

    % ---- 10: min-sum scaling sweep ----
    txt = fileread(fullfile(D, 'nms_tuning.txt')); lines = strsplit(strtrim(txt), newline);
    f = newfig(C, 760, 400); ax = gca; hold(ax, 'on');
    yline(ax, 0, '-', 'Color', C.ink2, 'LineWidth', 0.75, 'Alpha', 0.6);
    cols = {E.new, E.deployed, E.golden, E.legacy50}; chosen = containers.Map({'H_256_768_22','H_1024_2048_4f','H_4096_8192_3d','H_16200_9720'}, {0.85, 0.70, 0.85, 0.80});
    hh = gobjects(numel(lines),1);
    for i = 1:numel(lines)
        code = regexp(lines{i}, '^(\S+):', 'tokens', 'once'); code = code{1};
        tok = regexp(lines{i}, 'nms(\d\.\d\d)=([+-]\d+\.\d+)/', 'tokens');
        a = cellfun(@(t) str2double(t{1}), tok); loss = cellfun(@(t) str2double(t{2}), tok);
        hh(i) = plot(ax, a, loss, '-o', 'Color', cols{i}, 'LineWidth', 1.6, 'MarkerSize', 5.5, 'MarkerFaceColor', cols{i}, 'MarkerEdgeColor', C.surface, 'DisplayName', tex(code));
        if isKey(chosen, code)
            k = find(abs(a - chosen(code)) < 1e-6, 1);
            if ~isempty(k), plot(ax, a(k), loss(k), 'o', 'MarkerSize', 11, 'Color', cols{i}, 'LineWidth', 1.5); end
        end
    end
    xlabel(ax, 'normalised min-sum scaling factor \alpha'); ylabel(ax, 'loss vs belief propagation at FER 10% (dB)');
    title(ax, 'Min-sum scaling sweep: the 0.75 default was the loss, not min-sum', 'Color', C.ink, 'FontWeight', 'normal');
    subtitle(ax, 'MATLAB norm-min-sum vs BP, 50 iterations, 400 frames per point; ring = value chosen per code', 'Color', C.ink2);
    legend(ax, hh, 'Location', 'northeast', 'Box', 'off', 'TextColor', C.ink2); xlim(ax, [0.68 0.92]);
    finish(ax, C); hold(ax, 'off'); save(f, O, png_dir, 'nms_scaling_sweep');

    % ---- 11: decoder speed ----
    T = readtable(fullfile(D, 'decoder_timing.csv'));
    f = newfig(C, 760, 360); ax = gca; hold(ax, 'on');
    y = 1:height(T);
    b = barh(ax, y, [T.legacy_us_per_iter T.spt_us_per_iter T.nms16_us_per_iter], 0.78, 'EdgeColor', 'none');
    b(1).FaceColor = E.deployed; b(1).DisplayName = 'legacy SumProduct';
    b(2).FaceColor = E.golden;   b(2).DisplayName = 'table sum-product (spt)';
    b(3).FaceColor = E.new;      b(3).DisplayName = 'int16 vector min-sum (nms16)';
    vals = [T.legacy_us_per_iter T.spt_us_per_iter T.nms16_us_per_iter]; off = [-0.26 0 0.26];
    for i = y, for s = 1:3, text(ax, vals(i,s) * 1.08, i + off(s), sprintf('%.0f', vals(i,s)), 'Color', C.ink2, 'FontSize', 8.5, 'VerticalAlignment', 'middle'); end, end
    set(ax, 'XScale', 'log', 'YDir', 'reverse'); xlim(ax, [2 2000]); yticks(ax, y); yticklabels(ax, strcat(strrep(T.code, '_', '\_'), {'  ('}, string(T.N), {' bits)'}));
    xlabel(ax, 'microseconds per iteration (log scale), Apple M5 Pro, single thread');
    title(ax, 'Decoder cost per iteration', 'Color', C.ink, 'FontWeight', 'normal');
    subtitle(ax, 'cliff fixtures, 100 frames; speedup 6-30x per iteration, 3-14x per frame at 50 iterations', 'Color', C.ink2);
    legend(ax, b, 'Location', 'northeast', 'Box', 'off', 'TextColor', C.ink2);
    finish(ax, C); hold(ax, 'off'); save(f, O, png_dir, 'decoder_speed');

    % ---- 12: working memory ----
    f = newfig(C, 760, 360); ax = gca; hold(ax, 'on');
    b = barh(ax, y, [T.old_transient_kb T.new_persistent_kb], 0.72, 'EdgeColor', 'none');
    b(1).FaceColor = E.deployed; b(1).DisplayName = 'before: allocated and freed on every decode';
    b(2).FaceColor = E.new;      b(2).DisplayName = 'after: persistent context, zero allocations per decode';
    vals = [T.old_transient_kb T.new_persistent_kb]; off = [-0.18 0.18];
    for i = y, for s = 1:2, text(ax, vals(i,s) * 1.08, i + off(s), sprintf('%d KB', vals(i,s)), 'Color', C.ink2, 'FontSize', 8.5, 'VerticalAlignment', 'middle'); end, end
    set(ax, 'XScale', 'log', 'YDir', 'reverse'); xlim(ax, [10 4000]); yticks(ax, y); yticklabels(ax, strcat(strrep(T.code, '_', '\_'), {'  ('}, string(T.N), {' bits)'}));
    xlabel(ax, 'decoder working memory per code (KB, log scale)');
    title(ax, 'Working memory per code', 'Color', C.ink, 'FontWeight', 'normal');
    subtitle(ax, 'old decoder: one heap allocation per graph node per call (22,700 for the 16200-bit code)', 'Color', C.ink2);
    legend(ax, b, 'Location', 'southoutside', 'Orientation', 'horizontal', 'Box', 'off', 'TextColor', C.ink2);
    finish(ax, C); hold(ax, 'off'); save(f, O, png_dir, 'working_memory');
    fprintf('figures written to %s\n', O);
end

function f = newfig(C, w, h)
    if nargin < 2, w = 760; end
    if nargin < 3, h = 420; end
    f = figure('Visible', 'off', 'Color', C.surface, 'Units', 'pixels', 'Position', [100 100 w h]);
end

function finish(ax, C)
    set(ax, 'Color', C.surface, 'XColor', C.ink2, 'YColor', C.ink2, 'GridColor', C.grid, 'GridAlpha', 1, ...
        'MinorGridLineStyle', 'none', 'Box', 'off', 'TickDir', 'out', 'TickLength', [0.006 0.006], ...
        'FontName', 'Helvetica', 'FontSize', 10, 'LineWidth', 0.6, 'Layer', 'top');
    grid(ax, 'on');
    ax.Title.FontSize = 12; ax.XLabel.Color = C.ink2; ax.YLabel.Color = C.ink2;
end

function save(f, O, png_dir, name)
    exportgraphics(f, fullfile(O, [name '.svg']), 'ContentType', 'vector', 'BackgroundColor', '#fcfcfb');
    if ~isempty(png_dir)
        if ~isfolder(png_dir), mkdir(png_dir); end
        exportgraphics(f, fullfile(png_dir, [name '.png']), 'Resolution', 110, 'BackgroundColor', '#fcfcfb');
    end
    close(f);
end
