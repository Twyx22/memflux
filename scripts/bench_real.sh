#!/usr/bin/env bash
# bench_real.sh — benchmarks réels memflux, charges sécurisées (≤ 2 GB).
# Produit bench_results.csv consommé par scripts/gen_charts.py
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=build
OUT=/tmp/memflux/bench_results.csv
mkdir -p /tmp/memflux
echo "bench,phase,t_sec,rss_mb,swap_mb,psi_some,psi_full" > "$OUT"

rss() { awk '/VmRSS/{print $2/1024}' "$1" 2>/dev/null || echo 0; }
swp() { awk '/VmSwap/{print $2/1024}' "$1" 2>/dev/null || echo 0; }
psi() { head -1 /proc/pressure/memory | sed 's/.*avg10=\([0-9.]*\).*/\1/'; }
psifull() { awk '/^full/{print}' /proc/pressure/memory | sed 's/.*avg10=\([0-9.]*\).*/\1/'; }

sample() { # bench phase pid
  local r s p f
  r=$(rss /proc/$3/status); s=$(swp /proc/$3/status)
  p=$(psi); f=$(psifull)
  echo "$1,$2,$(date +%s.%N),$r,$s,$p,$f" >> "$OUT"
}

wait_psi_low() { while [ "$(psifull | cut -d. -f1)" -ge 5 ] 2>/dev/null; do sleep 2; done; }

echo "=== [1/4] pageout userspace : process 2GB dormeur → swap ==="
sudo pkill -x memfluxd 2>/dev/null || true; sleep 1
sudo stdbuf -oL $BUILD/memfluxd -f -v > /tmp/memflux/b1.log 2>&1 &
sleep 3
$BUILD/memfluxctl force > /dev/null   # réarmement
/tmp/memflux/child 2048 > /dev/null 2>&1 &
CP=$!
sleep 4
for t in 0 1 2 3; do sample pageout_pre before $CP; sleep 1; done
$BUILD/memfluxctl force
for t in 0 1 2 3 4 5 6 7; do sleep 1; sample pageout_post after $CP; done
echo "pageout: RSS $(rss /proc/$CP/status)MB, swap $(swp /proc/$CP/status)MB"
kill $CP 2>/dev/null || true
sudo pkill -x memfluxd 2>/dev/null || true
sleep 2; wait_psi_low() { while [ "$(psifull | cut -d. -f1)" -ge 3 ] 2>/dev/null; do sleep 2; done; }
wait_psi_low || true

echo "=== [2/4] damon_reclaim : 1.5GB dormeur, 60s ==="
sudo /tmp/memflux/venv/bin/damo reclaim enable > /dev/null 2>&1 || \
  sudo sh -c 'echo Y > /sys/module/damon_reclaim/parameters/enabled'
sudo sh -c 'echo 10000000 > /sys/module/damon_reclaim/parameters/min_age' 2>/dev/null || true
/tmp/memflux/child 1536 > /dev/null 2>&1 &
C1=$!
B0=$(sudo cat /sys/module/damon_reclaim/parameters/bytes_reclaimed_regions 2>/dev/null || echo 0)
sleep 3
for t in $(seq 0 12); do
  sample damon during $C1; sleep 4
done
B1=$(sudo cat /sys/module/damon_reclaim/parameters/bytes_reclaimed_regions 2>/dev/null || echo 0)
echo "damon_reclaim: $(( (B1-B0)/1048576 )) MB reclaimed en ~52s"
sudo /tmp/memflux/venv/bin/damo reclaim disable > /dev/null 2>&1 || true
kill $C1 2>/dev/null || true
sleep 3

echo "=== [3/4] débit allocateur : glibc vs memflux-preload ==="
for mode in glibc memflux; do
  if [ "$mode" = memflux ]; then
    R=$(MEMFLUX_STATS=0 LD_PRELOAD=$PWD/$BUILD/libmemflux-preload.so $BUILD/bench_alloc 4 150000 | tail -1)
  else
    R=$($BUILD/bench_alloc 4 150000 | tail -1)
  fi
  T=$(echo "$R" | grep -oE 'time=[0-9]+' | cut -d= -f2)
  PEAK=$(echo "$R" | grep -oE 'peak_rss=[0-9]+' | cut -d= -f2)
  FIN=$(echo "$R" | grep -oE 'final_rss=[0-9]+' | cut -d= -f2)
  echo "$mode: time=${T}ms peak=${PEAK}MB final=${FIN}MB"
  echo "allocator,$mode,0,$PEAK,$FIN,0,0" >> "$OUT"
done

echo "=== [4/4] working-set sampler : détection dormance ==="
/tmp/memflux/child 1024 > /dev/null 2>&1 &
C2=$!
sleep 4
sudo grep -oE 'ws_ratio=[0-9.]+ .score=[0-9.]+' /var/log/memflux.log 2>/dev/null | tail -1 || true
$BUILD/memfluxctl top 3 2>/dev/null | head -4 || true
kill $C2 2>/dev/null || true

echo "=== résultats: $OUT ==="
column -t -s, "$OUT" | head -30