#!/bin/bash
#
# bench.sh — Resource usage benchmark for nv-monitor vs top vs htop
#
# Run on the target device (from anywhere):
#   ./tests/bench.sh
#
# Outputs a summary to stdout. No files sent, just paste the results.

set -e
cd "$(dirname "$0")/.." || exit 1   # run against repo root regardless of caller CWD

NV_MONITOR="./nv-monitor"
DURATION=10
CLK=$(getconf CLK_TCK)

echo "========================================"
echo " nv-monitor resource benchmark"
echo "========================================"
echo ""
echo "--- System ---"
uname -m
cat /sys/firmware/devicetree/base/model 2>/dev/null || grep "model name" /proc/cpuinfo | head -1 | cut -d: -f2
echo ""
echo "CPUs: $(nproc)"
free -h | head -2
echo ""
echo "Binary sizes:"
[ -x "$NV_MONITOR" ] && ls -lh "$NV_MONITOR" | awk '{print "  nv-monitor:", $5}'
command -v top &>/dev/null && ls -lh "$(which top)" | awk '{print "  top:       ", $5}'
command -v htop &>/dev/null && ls -lh "$(which htop)" | awk '{print "  htop:      ", $5}' || echo "  htop:       (not installed)"
echo ""

measure() {
    local NAME="$1"
    shift
    "$@" &>/dev/null &
    local PID=$!
    sleep 2

    # Memory
    local RSS=$(grep VmRSS /proc/$PID/status 2>/dev/null | awk '{print $2}')
    local PRIVATE=$(grep Private_Dirty /proc/$PID/smaps_rollup 2>/dev/null | awk '{print $2}')
    local SHARED=$(grep Shared_Clean /proc/$PID/smaps_rollup 2>/dev/null | awk '{print $2}')
    local THREADS=$(grep Threads /proc/$PID/status 2>/dev/null | awk '{print $2}')

    # CPU over measurement period
    local CPU1=$(cat /proc/$PID/stat 2>/dev/null | awk '{print $14+$15}')
    sleep $DURATION
    local CPU2=$(cat /proc/$PID/stat 2>/dev/null | awk '{print $14+$15}')
    local CPU_PCT=$(echo "scale=2; ($CPU2-$CPU1)*100/$CLK/$DURATION" | bc 2>/dev/null)

    kill $PID 2>/dev/null; wait $PID 2>/dev/null

    printf "%-35s RSS: %6s KB  Private: %6s KB  Shared: %6s KB  Threads: %s  CPU: %s%%\n" \
        "$NAME" "${RSS:-?}" "${PRIVATE:-?}" "${SHARED:-?}" "${THREADS:-?}" "${CPU_PCT:-?}"
}

echo "--- Measurements (${DURATION}s sample) ---"
echo ""

if [ -x "$NV_MONITOR" ]; then
    measure "nv-monitor (prometheus exporter)" "$NV_MONITOR" -p 9198
fi

if command -v top &>/dev/null; then
    measure "top (batch mode)" top -b -d 1
fi

if command -v htop &>/dev/null; then
    echo ""
    echo "Note: htop requires a real terminal for CPU measurement."
    echo "      Binary size shown above. Run 'htop' in a terminal and check 'ps' for live stats."
fi

echo ""
echo "--- NVML memory breakdown ---"
if [ -x "$NV_MONITOR" ]; then
    "$NV_MONITOR" -p 9198 &>/dev/null &
    NV_PID=$!
    sleep 2
    echo "NVML mapped regions:"
    grep nvidia /proc/$NV_PID/maps 2>/dev/null | while read line; do
        RANGE=$(echo "$line" | awk '{print $1}')
        START=$(echo "$RANGE" | cut -d- -f1)
        END=$(echo "$RANGE" | cut -d- -f2)
        SIZE=$(python3 -c "print(f'{(int(\"$END\",16)-int(\"$START\",16))/1024:.0f} KB')" 2>/dev/null || echo "? KB")
        PERMS=$(echo "$line" | awk '{print $2}')
        FILE=$(echo "$line" | awk '{print $NF}')
        echo "  $PERMS  $SIZE  $FILE"
    done
    if ! grep -q nvidia /proc/$NV_PID/maps 2>/dev/null; then
        echo "  (no NVML loaded)"
    fi
    kill $NV_PID 2>/dev/null; wait $NV_PID 2>/dev/null
fi

echo ""
echo "--- Notes ---"
echo "RSS includes shared library pages mapped by other GPU processes."
echo "Private_Dirty is the unique memory cost of each process."
echo "CPU% measured over ${DURATION}s (scrape-driven collection)."
echo ""
echo "Done."
