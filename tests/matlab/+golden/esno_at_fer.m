function x = esno_at_fer(esno, fer, target)
%ESNO_AT_FER  Es/N0 (dB) at which a FER curve crosses `target`, log-linear interp.
%   Returns NaN if the curve never crosses.  Points with zero errors are
%   clamped to a floor so the log is finite.
    fer = max(fer, 1e-6);
    y = log10(fer(:)); x0 = esno(:);
    k = find(y(1:end-1) >= log10(target) & y(2:end) < log10(target), 1);
    if isempty(k), x = NaN; return; end
    x = interp1(y(k:k+1), x0(k:k+1), log10(target));
end
