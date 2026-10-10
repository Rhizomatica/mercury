classdef TestLdpc < matlab.unittest.TestCase
%TESTLDPC  Golden tests for stages T2 (LDPC encode) and R12 (LDPC decode),
%   plus the R11 LLR-scale hypothesis.  See docs/MODEM-ANALYSIS.md.
%
%   Golden: Communications Toolbox ldpcEncode / ldpcDecode on the parity-check
%   matrix exported from the C tables (utils/golden/ldpc_export).  The C side
%   runs through utils/golden/ldpc_bench (files, no MEX).
%
%   Results (FER curves, dB gaps) are written to tests/matlab/results/.

    properties (TestParameter)
        % data-mode codes, smallest first (DATAC14, DATAC0, DATAC13, DATAC15/16,
        % DATAC3/4, DATAC1, DATAC17/QAM16C2)
        code = {'HRA_56_56', 'H_128_256_5', 'H_256_512_4', 'H_256_768_22', ...
                'H_1024_2048_4f', 'H_4096_8192_3d', 'H_16200_9720'};
    end

    properties
        NFramesEquiv = 20;     % frames for equivalence checks
        NFramesPerf  = 600;    % frames per Es/N0 point (adaptive stop below)
        MinErrors    = 30;     % stop a point once this many frame errors are seen
        EbNoGrid_dB  = 0:0.5:4;
        GapTol_dB    = 0.3;    % decoder budget / LLR-scale loss allowed before FAIL
        ModemIters   = 10;     % freedv_700.c: f->ldpc->max_iter = 10 -- ONLY under #ifdef __EMBEDDED__
        TableIters   = 100;    % desktop builds: the code tables' *_MAX_ITER (what Mercury ships on a PC)
        GoldenIters  = 50;
    end

    methods (TestClassSetup)
        function build(tc) %#ok<MANU>
            c2.ensure_tools();
        end
    end

    methods (Test)
        function exported_H_matches_C_encoder(tc, code)
            % H from the exporter must annihilate codewords from the C encoder.
            H = c2.read_rows(code);
            [M, N] = size(H); K = N - M;
            rng(1);
            info = logical(randi([0 1], K, tc.NFramesEquiv));
            cw = c2.ldpc_encode(code, info);
            tc.verifyEqual(cw(1:K, :), info, 'C encoder is not systematic as assumed');
            synd = mod(double(H) * double(cw), 2);
            tc.verifyEqual(nnz(synd), 0, sprintf('%s: exported H does not annihilate C codewords (%d nonzero syndromes)', code, nnz(synd)));
        end

        function encoder_equivalence(tc, code)
            % codec2 encode() must equal ldpcEncode bit-for-bit.
            H = c2.read_rows(code);
            [M, N] = size(H); K = N - M;
            cfg = ldpcEncoderConfig(H);
            rng(2);
            info = logical(randi([0 1], K, tc.NFramesEquiv));
            cw_c = c2.ldpc_encode(code, info);
            cw_m = logical(ldpcEncode(double(info), cfg));
            tc.verifyEqual(cw_c, cw_m, sprintf('%s: C encoder differs from ldpcEncode', code));
        end

        function decoder_sign_convention(tc, code)
            % Hard LLRs of a codeword decode to that codeword: positive = bit 0.
            H = c2.read_rows(code); [M, N] = size(H); K = N - M;
            rng(3);
            info = logical(randi([0 1], K, 4));
            cw = c2.ldpc_encode(code, info);
            llr = (1 - 2 * double(cw)) * 20;
            [bits, ~, ok] = c2.ldpc_decode(code, llr, 5);
            tc.verifyEqual(bits, cw, sprintf('%s: sign convention', code));
            tc.verifyTrue(all(ok));
        end

        function decoder_budget_and_llr_scale(tc, code)
            % FER vs Es/N0 (BPSK, exact LLRs) for:
            %   C SumProduct @10 (embedded-build cap), C @50, MATLAB bp @50 (golden),
            %   MATLAB norm-min-sum @50, C @10 and @100 with the modem's fixed-EsNo
            %   LLR scale (EsNodB = 3, or 10 for the 16200 code) -- @100 fixed is
            %   what a desktop Mercury ships -- and the new decoder at its own
            %   cap of 100 (nms16 / spt, early exit on parity).
            H = c2.read_rows(code); [M, N] = size(H); K = N - M; R = K / N;
            cfgE = ldpcEncoderConfig(H);
            cfgBP = ldpcDecoderConfig(cfgE, 'bp'); cfgNMS = ldpcDecoderConfig(cfgE, 'norm-min-sum');
            esno_dB = tc.EbNoGrid_dB + 10*log10(R);           % BPSK: Es/N0 = Eb/N0 + 10log10(R)
            if strcmp(code, 'H_16200_9720'), fixedEsNo_dB = 10; else, fixedEsNo_dB = 3; end
            variants = {'C_leg10', 'C_leg50', 'C_sp50', 'C_nms50', 'M_bp50', 'M_nms50', 'C_leg10_fixedEsNo', 'C_nms50_fixedEsNo', 'C_nms16_50', 'C_spt50', ...
                        'C_leg100_fixedEsNo', 'C_nms16_100', 'C_spt100'};
            fer = nan(numel(variants), numel(esno_dB));
            rng(4);
            for ip = 1:numel(esno_dB)
                esno = 10^(esno_dB(ip)/10);
                sigma2 = 1 / (2 * esno);                      % per real dimension, Es = 1
                nerr = zeros(1, numel(variants)); ntot = 0;
                while ntot < tc.NFramesPerf && min(nerr) < tc.MinErrors
                    F = min(20, tc.NFramesPerf - ntot);
                    info = randi([0 1], K, F);
                    cw = ldpcEncode(info, cfgE);
                    y = (1 - 2*cw) + sqrt(sigma2) * randn(N, F);
                    llr = 2 * y / sigma2;                      % exact LLR, positive = 0
                    llr_fixed = 2 * y * 2 * 10^(fixedEsNo_dB/10);   % modem: 1/sigma2 replaced by 2*EsNo_fixed
                    d = cell(1, numel(variants));
                    d{1} = c2.ldpc_decode(code, llr, tc.ModemIters, 'legacy');
                    d{2} = c2.ldpc_decode(code, llr, tc.GoldenIters, 'legacy');
                    d{3} = c2.ldpc_decode(code, llr, tc.GoldenIters, 'sp');
                    d{4} = c2.ldpc_decode(code, llr, tc.GoldenIters, 'nms');
                    d{5} = ldpcDecode(llr, cfgBP, tc.GoldenIters, 'OutputFormat', 'whole', 'DecisionType', 'hard');
                    d{6} = ldpcDecode(llr, cfgNMS, tc.GoldenIters, 'OutputFormat', 'whole', 'DecisionType', 'hard');
                    d{7} = c2.ldpc_decode(code, llr_fixed, tc.ModemIters, 'legacy');
                    d{8} = c2.ldpc_decode(code, llr_fixed, tc.GoldenIters, 'nms');
                    d{9} = c2.ldpc_decode(code, llr, tc.GoldenIters, 'nms16');
                    d{10} = c2.ldpc_decode(code, llr, tc.GoldenIters, 'spt');
                    d{11} = c2.ldpc_decode(code, llr_fixed, tc.TableIters, 'legacy');   % deployed on a PC
                    d{12} = c2.ldpc_decode(code, llr, tc.TableIters, 'nms16');
                    d{13} = c2.ldpc_decode(code, llr, tc.TableIters, 'spt');
                    for v = 1:numel(variants)
                        nerr(v) = nerr(v) + sum(any(logical(d{v}(1:K, :)) ~= logical(info), 1));
                    end
                    ntot = ntot + F;
                end
                fer(:, ip) = nerr / ntot;
            end
            x1 = arrayfun(@(v) golden.esno_at_fer(esno_dB, fer(v, :), 1e-1), 1:numel(variants));
            x2 = arrayfun(@(v) golden.esno_at_fer(esno_dB, fer(v, :), 1e-2), 1:numel(variants));
            T = array2table([esno_dB; fer]', 'VariableNames', [{'EsNo_dB'}, variants]);
            p = c2.paths(); if ~isfolder(p.results), mkdir(p.results); end
            writetable(T, fullfile(p.results, sprintf('ldpc_fer_%s.csv', code)));
            fmt = @(x) strjoin(arrayfun(@(v) sprintf('%s=%.2f', variants{v}, x(v)), 1:numel(variants), 'UniformOutput', false), ' ');
            summary = sprintf('%s (N=%d K=%d R=%.2f) Es/N0[dB] @FER1e-1: %s | @FER1e-2: %s | legacy10 vs bp50 %.2f dB | deployed(leg100 fixed) vs new(nms16 100) %.2f dB | ctx sp50 vs legacy50 %.2f dB | ctx nms50 vs M nms50 %.2f dB | nms scale sensitivity %.2f dB | nms16 vs nms %.2f dB | spt vs sp %.2f dB', ...
                code, N, K, R, fmt(x1), fmt(x2), x1(1) - x1(5), x1(11) - x1(12), x1(3) - x1(2), x1(4) - x1(6), x1(8) - x1(4), x1(9) - x1(4), x1(10) - x1(3));
            tc.log(1, summary);
            fid = fopen(fullfile(p.results, 'ldpc_summary.txt'), 'a'); fprintf(fid, '%s\n', summary); fclose(fid);
            % Findings (the modem as shipped) are logged; the gates below are on the NEW decoder:
            %   ctx sum-product must reproduce legacy, ctx min-sum must track MATLAB norm-min-sum,
            %   and min-sum must be insensitive to the fixed-EsNo scale.
            tc.verifyLessThan(abs(x1(3) - x1(2)), 0.1, sprintf('%s: ctx sum-product deviates %.2f dB from legacy', code, x1(3) - x1(2)));
            tc.verifyLessThan(x1(4) - x1(6), 0.1, sprintf('%s: ctx min-sum is %.2f dB worse than MATLAB norm-min-sum', code, x1(4) - x1(6)));
            tc.verifyLessThan(abs(x1(8) - x1(4)), 0.15, sprintf('%s: min-sum moved %.2f dB under the fixed-EsNo scale', code, x1(8) - x1(4)));
            tc.verifyLessThan(x1(9) - x1(4), 0.1, sprintf('%s: int16 min-sum loses %.2f dB to float min-sum', code, x1(9) - x1(4)));
            tc.verifyLessThan(abs(x1(10) - x1(3)), 0.1, sprintf('%s: table sum-product deviates %.2f dB from exact', code, x1(10) - x1(3)));
        end
    end
end
