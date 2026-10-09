classdef TestLlr < matlab.unittest.TestCase
%TESTLLR  Golden tests for stage R11 (soft demapping) and the LLR scale.
%   1. codec2's max-log demapper (llr_from_qam) vs MATLAB's approximate-LLR
%      demodulator on the same constellation and noise variance, bps 2..8.
%   2. The modem-level cost of the fixed-EsNo LLR scale at each mode's real
%      modulation: FER vs Es/N0 with LLRs scaled by the true Es/N0 versus the
%      mode constant (3 dB, or 10 dB for the 16200-bit code), decoded by the
%      C decoder (table sum-product, 50 iterations).
    properties (TestParameter)
        bps = {2, 4, 5, 6, 7, 8};
        modecode = {struct('code','H_256_768_22','bps',2,'fixed',3,  'ebno',-1:0.5:3), ...
                    struct('code','H_4096_8192_3d','bps',2,'fixed',3,'ebno',0:0.5:4), ...
                    struct('code','H_16200_9720','bps',4,'fixed',10, 'ebno',1:0.5:8)};
    end
    properties
        NFramesPerf = 300; MinErrors = 30;
    end
    methods (TestClassSetup)
        function build(tc) %#ok<MANU>
            c2.ensure_tools();
        end
    end
    methods (Test)
        function maxlog_matches_matlab(tc, bps)
            S = c2.qam_table(bps); M = numel(S);
            tc.verifyEqual(M, 2^bps);
            tc.verifyEqual(mean(abs(S).^2), 1, 'AbsTol', 1e-3);
            rng(bps);
            n = 4000; lab = randi([0 M-1], n, 1);
            esno_dB = 6; esno = 10^(esno_dB/10); sigma2 = 1/esno;      % complex noise variance, Es = 1
            y = S(lab+1) + sqrt(sigma2/2) * (randn(n,1) + 1j*randn(n,1));
            demod = comm.GeneralQAMDemodulator('Constellation', S, 'BitOutput', true, ...
                'DecisionMethod', 'Approximate log-likelihood ratio', ...
                'VarianceSource', 'Property', 'Variance', sigma2);
            ref = reshape(demod(y), bps, []);     % MATLAB: positive = bit 0, MSB first
            got = c2.llr(bps, esno_dB, y, 'maxlog');
            err = max(abs(got(:) - ref(:))) / max(abs(ref(:)));
            tc.log(1, sprintf('bps %d: max relative LLR error vs MATLAB approx-LLR = %.2e', bps, err));
            tc.verifyLessThan(err, 1e-4, sprintf('bps %d: max-log demapper deviates from MATLAB', bps));
        end

        function fixed_scale_cost_at_real_modulation(tc, modecode)
            code = modecode.code; bps = modecode.bps;
            H = c2.read_rows(code); [M, N] = size(H); K = N - M; R = K / N;
            S = c2.qam_table(bps); cfgE = ldpcEncoderConfig(H);
            nsym = N / bps; tc.assertEqual(nsym, round(nsym));
            esno_dB = modecode.ebno + 10*log10(R * bps);     % Es/N0 per symbol
            variants = {'true', 'fixed'}; fer = nan(2, numel(esno_dB)); rng(5);
            for ip = 1:numel(esno_dB)
                esno = 10^(esno_dB(ip)/10); sigma2 = 1/esno; nerr = [0 0]; ntot = 0;
                while ntot < tc.NFramesPerf && min(nerr) < tc.MinErrors
                    F = 10; info = randi([0 1], K, F); cw = ldpcEncode(info, cfgE);
                    lab = zeros(nsym, F);
                    for b = 1:bps, lab = lab * 2 + reshape(cw(b:bps:end, :), nsym, F); end   % MSB first, as psk_modulate_frame
                    y = S(lab + 1) + sqrt(sigma2/2) * (randn(nsym, F) + 1j*randn(nsym, F));
                    l_true  = c2.llr(bps, esno_dB(ip), y(:), 'maxlog');
                    l_fixed = c2.llr(bps, modecode.fixed, y(:), 'maxlog');
                    d1 = c2.ldpc_decode(code, reshape(l_true, N, F), 50, 'spt');
                    d2 = c2.ldpc_decode(code, reshape(l_fixed, N, F), 50, 'spt');
                    nerr(1) = nerr(1) + sum(any(logical(d1(1:K,:)) ~= logical(info), 1));
                    nerr(2) = nerr(2) + sum(any(logical(d2(1:K,:)) ~= logical(info), 1));
                    ntot = ntot + F;
                end
                fer(:, ip) = nerr / ntot;
            end
            x1 = arrayfun(@(v) golden.esno_at_fer(esno_dB, fer(v,:), 1e-1), 1:2);
            x2 = arrayfun(@(v) golden.esno_at_fer(esno_dB, fer(v,:), 1e-2), 1:2);
            p = c2.paths(); if ~isfolder(p.results), mkdir(p.results); end
            writetable(array2table([esno_dB; fer]', 'VariableNames', [{'EsNo_dB'}, variants]), fullfile(p.results, sprintf('llr_scale_%s.csv', code)));
            msg = sprintf('%s @ %d bits/sym: Es/N0 @FER1e-1 true=%.2f fixed(%g dB)=%.2f -> fixed-scale cost %.2f dB | @1e-2 %.2f dB', ...
                code, bps, x1(1), modecode.fixed, x1(2), x1(2)-x1(1), x2(2)-x2(1));
            tc.log(1, msg);
            fid = fopen(fullfile(p.results, 'llr_summary.txt'), 'a'); fprintf(fid, '%s\n', msg); fclose(fid);
            tc.verifyLessThan(x1(2) - x1(1), 0.3, sprintf('%s: fixed-EsNo scale costs %.2f dB at the real modulation', code, x1(2) - x1(1)));
        end
    end
end
