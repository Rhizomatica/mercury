function T = codes()
%CODES  Table of exported codec2 LDPC codes (from fixtures/ldpc/codes.txt).
    p = c2.paths();
    T = readtable(fullfile(p.fixtures, 'ldpc', 'codes.txt'), 'FileType', 'text', ...
        'CommentStyle', '#', 'ReadVariableNames', false, 'Delimiter', ' ');
    T.Properties.VariableNames = {'name','N','M','K','NumberRowsHcols','shift','H1', ...
        'max_row_w','max_col_w','dec_type','max_iter'};
end
