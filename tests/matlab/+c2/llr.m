function llr = llr(bps, esno_dB, sym, path)
%LLR  Run codec2's soft demapper (utils/golden/llr_bench) on complex symbols.
%   sym: complex column (unit-power constellation, equalised); path: 'maxlog'
%   (llr_from_qam, default) or 'somap' (Demod2D+Somap, bps 2 and 4 only).
%   llr: bps x numel(sym), positive = bit 0, row 1 = label MSB.
    if nargin < 4 || isempty(path), path = 'maxlog'; end
    p = c2.paths(); tool = fullfile(p.golden, 'llr_bench');
    tin = [tempname '.f32']; tout = [tempname '.f32'];
    c = onCleanup(@() cellfun(@(f) delete(f), {tin, tout}, 'UniformOutput', false));
    iq = single([real(sym(:)) imag(sym(:))].');
    fid = fopen(tin, 'wb'); fwrite(fid, iq(:), 'single'); fclose(fid);
    [st, out] = system(sprintf('"%s" --bps %d --esno %.4f --path %s "%s" "%s"', tool, bps, esno_dB, path, tin, tout));
    assert(st == 0, 'llr_bench failed: %s', out);
    fid = fopen(tout, 'rb'); v = fread(fid, inf, 'single=>double'); fclose(fid);
    llr = reshape(v, bps, []);
end
