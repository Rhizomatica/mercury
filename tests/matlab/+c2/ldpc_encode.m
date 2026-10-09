function cw = ldpc_encode(name, info)
%LDPC_ENCODE  Encode with codec2's encoder via utils/golden/ldpc_bench.
%   info: K x F logical/0-1 matrix (one frame per column); cw: N x F.
    p = c2.paths();
    tin = [tempname '.u8']; tout = [tempname '.u8'];
    c = onCleanup(@() cellfun(@(f) delete(f), {tin, tout}, 'UniformOutput', false));
    fid = fopen(tin, 'wb'); fwrite(fid, uint8(info(:)), 'uint8'); fclose(fid);
    [st, out] = system(sprintf('"%s" --code %s --enc "%s" "%s"', p.ldpc_bench, name, tin, tout));
    assert(st == 0, 'ldpc_bench --enc failed: %s', out);
    fid = fopen(tout, 'rb'); b = fread(fid, inf, 'uint8=>uint8'); fclose(fid);
    F = size(info, 2);
    cw = logical(reshape(b, [], F));
end
