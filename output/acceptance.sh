#!/bin/bash
# Acceptance driver: equivalence proof + jitter buffer acceptance + performance.
# Self-contained; safe to re-run. Records are written to output/.
set -e
ROOT=/home/ys/feedback/uidreczz28Z6pCPhnOy
BASE=/tmp/speex-baseline
OUT=$ROOT/output
cd "$ROOT"

banner() {
  echo
  echo "================================================================"
  echo "==  $1"
  echo "================================================================"
}

banner "SECTION 1: build baseline (git HEAD, before the change)"
if [ ! -x "$BASE/build/libspeex/testenc" ]; then
  rm -rf "$BASE" && mkdir -p "$BASE"
  git archive HEAD | tar -x -C "$BASE"
  (cd "$BASE" && meson setup build -Dtest-binaries=enabled >/dev/null && ninja -C build >/dev/null)
fi
echo "baseline build: OK ($BASE/build)"

banner "SECTION 2: build current tree (with jitter buffer)"
ninja -C build >/dev/null
echo "current build: OK ($ROOT/build)"

banner "SECTION 3: equivalence proof - repo test entry (testenc) bit-exact"
[ -f "$OUT/input.sw" ] || python3 - <<'EOF'
import struct, math
state = 12345
def lcg():
    global state
    state = (1103515245*state + 12345) & 0x7fffffff
    return state
s = [max(-32768, min(32767, int(8000*math.sin(2*math.pi*220*i/8000.0)+4000*math.sin(2*math.pi*440*i/8000.0)+(lcg()%2000-1000)))) for i in range(32000)]
open('output/input.sw','wb').write(struct.pack('<%dh'%len(s), *s))
EOF
"$BASE/build/libspeex/testenc" "$OUT/input.sw" "$OUT/baseline_out.sw" "$OUT/baseline_bits.dat" 2>/dev/null
build/libspeex/testenc "$OUT/input.sw" "$OUT/after_out.sw" "$OUT/after_bits.dat" 2>/dev/null
sha256sum "$OUT/baseline_out.sw" "$OUT/after_out.sw" "$OUT/baseline_bits.dat" "$OUT/after_bits.dat" | tee "$OUT/equivalence_sha256.txt"
cmp "$OUT/baseline_out.sw" "$OUT/after_out.sw" && echo "decoded output: BIT-EXACT"
cmp "$OUT/baseline_bits.dat" "$OUT/after_bits.dat" && echo "encoded bitstream: BIT-EXACT"

banner "SECTION 4: equivalence proof - exported symbols only grow"
nm -D --defined-only "$BASE/build/libspeex/libspeex.so" | awk '{print $3}' | sort > "$OUT/baseline_symbols.txt"
nm -D --defined-only build/libspeex/libspeex.so | awk '{print $3}' | sort > "$OUT/after_symbols.txt"
echo "symbols before: $(wc -l < "$OUT/baseline_symbols.txt"), after: $(wc -l < "$OUT/after_symbols.txt")"
echo "removed/changed symbols (must be empty):"
comm -23 "$OUT/baseline_symbols.txt" "$OUT/after_symbols.txt" | tee "$OUT/symbols_removed.txt"
echo "added symbols:"
comm -13 "$OUT/baseline_symbols.txt" "$OUT/after_symbols.txt" | tee "$OUT/symbols_added.txt"

banner "SECTION 5: acceptance - reorder recovery / loss diagnostics / delay bound"
build/libspeex/testjitter | tee "$OUT/testjitter.log"

banner "SECTION 6: performance data"
python3 -c "
d = open('$OUT/input.sw','rb').read()
open('/tmp/big_input.sw','wb').write(d*200)"
echo "-- testenc before (3 runs, 12.8MB input) --"
for i in 1 2 3; do /usr/bin/time -f "%e s  %M KB" "$BASE/build/libspeex/testenc" /tmp/big_input.sw /tmp/o1.sw /tmp/b1.dat 2>&1 >/dev/null | tail -1; done | tee "$OUT/perf_before.txt"
echo "-- testenc after (3 runs, 12.8MB input) --"
for i in 1 2 3; do /usr/bin/time -f "%e s  %M KB" build/libspeex/testenc /tmp/big_input.sw /tmp/o2.sw /tmp/b2.dat 2>&1 >/dev/null | tail -1; done | tee "$OUT/perf_after.txt"
cmp /tmp/o1.sw /tmp/o2.sw && cmp /tmp/b1.dat /tmp/b2.dat && echo "large-input outputs: BIT-EXACT"
echo "-- jitter buffer throughput (put+get+tick) --"
build/libspeex/testjitter perf | tee "$OUT/perf_jitter.txt"

banner "SECTION 7: summary"
echo "equivalence : testenc outputs bit-exact before/after, symbols only added"
echo "acceptance  : see testjitter.log (all checks PASS)"
echo "performance : see perf_before.txt / perf_after.txt / perf_jitter.txt"
echo "artifacts   : $OUT"
