function p = paths()
%PATHS  Repository and tool locations for the golden tests.
    here = fileparts(fileparts(mfilename('fullpath')));   % tests/matlab
    p.root     = fileparts(fileparts(here));             % repo root
    p.fixtures = fullfile(here, 'fixtures');
    p.results  = fullfile(here, 'results');
    p.golden   = fullfile(p.root, 'utils', 'golden');
    p.ldpc_bench  = fullfile(p.golden, 'ldpc_bench');
    p.ldpc_export = fullfile(p.golden, 'ldpc_export');
    p.freedv   = fullfile(p.root, 'modem', 'freedv');
end
