% run_all — run every golden test and print the summary.
here = fileparts(mfilename('fullpath')); addpath(here);
results = runtests(here, 'IncludeSubfolders', false);
disp(table(results));
