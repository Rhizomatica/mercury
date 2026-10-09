function H = read_rows(name)
%READ_ROWS  Load an exported codec2 parity-check matrix as sparse logical (M x N).
    p = c2.paths();
    f = fullfile(p.fixtures, 'ldpc', [name '.rows']);
    fid = fopen(f, 'r'); assert(fid > 0, 'missing %s (run make export)', f);
    c = onCleanup(@() fclose(fid));
    hdr = sscanf(fgetl(fid), '%d %d'); N = hdr(1); M = hdr(2);
    rows = cell(M, 1);
    for i = 1:M
        rows{i} = sscanf(fgetl(fid), '%d')';
    end
    ri = repelem((1:M)', cellfun(@numel, rows));
    ci = [rows{:}]';
    H = sparse(ri, ci, true, M, N);
    H = logical(H);
end
