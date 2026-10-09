function S = qam_table(bps)
%QAM_TABLE  The constellation codec2 uses for bps bits/symbol (label order).
    p = c2.paths(); tool = fullfile(p.golden, 'llr_bench');
    t = [tempname '.f32']; c = onCleanup(@() delete(t));
    [st, out] = system(sprintf('"%s" --bps %d --dump-table "%s"', tool, bps, t));
    assert(st == 0, 'llr_bench --dump-table failed: %s', out);
    fid = fopen(t, 'rb'); v = fread(fid, inf, 'single=>double'); fclose(fid);
    S = v(1:2:end) + 1j * v(2:2:end);
end
