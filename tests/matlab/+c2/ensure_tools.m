function ensure_tools()
%ENSURE_TOOLS  Build utils/golden and export the LDPC fixtures if missing.
    p = c2.paths();
    if ~isfile(p.ldpc_bench) || ~isfile(fullfile(p.fixtures, 'ldpc', 'codes.txt'))
        [st, out] = system(sprintf('make -C "%s" all export 2>&1', p.golden));
        assert(st == 0, 'building utils/golden failed:\n%s', out);
    end
end
