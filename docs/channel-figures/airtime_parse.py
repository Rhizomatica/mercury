import sys, re
chan, traffic, seed = sys.argv[1], sys.argv[2], sys.argv[3]
text = sys.stdin.read()
ctl = data = 0
for m in re.finditer(r"mode (\d+):\s+\d+ frames\s+\d+ ok\s+(\d+) s air", text):
    if int(m.group(1)) == 23: ctl += int(m.group(2))
    else: data += int(m.group(2))
ofdm = ctl + data
seedline = [l for l in text.splitlines() if l.startswith("seed=")][0]
d = dict(kv.split("=", 1) for kv in seedline.split() if "=" in kv)
air_a, air_b = float(d["air"].split("+")[0]), float(d["air"].split("+")[1])
total_air = air_a + air_b
floor = max(total_air - ofdm, 0.0)
done = int(d["done_ms"]) / 1000.0
print(f"{chan},{traffic},{seed},{d['integrity']},{done:.1f},{air_a},{air_b},{d['keys']},{ofdm},{ctl},{data},{floor:.1f}")
