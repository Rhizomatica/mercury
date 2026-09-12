% Vectorised equivalent of ch_fading.m (same interleaved layout, same seed).
function gen_fading(raw_file_name, Fs, dopplerSpreadHz, len_samples)
  randn('seed',1);
  spread     = doppler_spread(dopplerSpreadHz, Fs, len_samples);
  spread_2ms = doppler_spread(dopplerSpreadHz, Fs, len_samples);
  hf_gain = 1.0/sqrt(var(spread)+var(spread_2ms));
  printf("hf_gain: %f\n", hf_gain);
  inter = zeros(1, len_samples*4 + 4);
  inter(1:4) = hf_gain;
  inter(5:4:end) = real(spread);
  inter(6:4:end) = imag(spread);
  inter(7:4:end) = real(spread_2ms);
  inter(8:4:end) = imag(spread_2ms);
  f = fopen(raw_file_name,"wb");
  fwrite(f, inter, "float32");
  fclose(f);
  printf("wrote %s: %d samples\n", raw_file_name, len_samples);
end
