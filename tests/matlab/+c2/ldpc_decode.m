function [bits, iters, parity_ok] = ldpc_decode(name, llr, maxiter, alg)
%LDPC_DECODE  Decode with codec2's SumProduct via utils/golden/ldpc_bench.
%   llr: N x F single/double LLRs, positive = bit 0 (codec2 and MATLAB convention).
%   alg (optional): 'legacy' | 'sp' | 'nms' (default 'sp').
%   bits: N x F logical; iters, parity_ok: 1 x F.
    if nargin < 4 || isempty(alg), alg = 'sp'; end
    p = c2.paths();
    tin = [tempname '.f32']; tout = [tempname '.u8']; tst = [tempname '.txt'];
    c = onCleanup(@() cellfun(@(f) delete(f), {tin, tout, tst}, 'UniformOutput', false));
    fid = fopen(tin, 'wb'); fwrite(fid, single(llr(:)), 'single'); fclose(fid);
    [st, out] = system(sprintf('"%s" --code %s --dec "%s" "%s" --mi %d --alg %s --stats "%s"', ...
        p.ldpc_bench, name, tin, tout, maxiter, alg, tst));
    assert(st == 0, 'ldpc_bench --dec failed: %s', out);
    fid = fopen(tout, 'rb'); b = fread(fid, inf, 'uint8=>uint8'); fclose(fid);
    F = size(llr, 2);
    bits = logical(reshape(b, [], F));
    s = readmatrix(tst, 'FileType', 'text');
    iters = s(:, 1)'; parity_ok = logical(s(:, 2))';
end
