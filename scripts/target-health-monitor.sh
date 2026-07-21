#!/bin/sh
# target-health-monitor.sh
#
# Runs ON THE DIRETTA TARGET (e.g. GentooPlayerRpi4), NOT on the s2d host.
# Samples the three suspected "degrades after ~1 hour" mechanisms once per
# interval and appends one CSV line each time, so the trend over an hour is
# directly plottable / greppable:
#
#   1. Thermal throttling   -> temp_c, throttled (vcgencmd)
#   2. USB-DAC IRQ load      -> xhci IRQ delta per second (isochronous churn)
#   3. Network IRQ spread    -> eth0 hard-IRQ delta + NET_RX softirq per-CPU
#
# Each numeric IRQ column is a PER-SECOND DELTA (count since last sample /
# elapsed seconds), so a rising column = that mechanism is getting worse.
#
# Usage:
#   ./target-health-monitor.sh                 # 60s interval -> ./target-health.csv
#   ./target-health-monitor.sh 120 /tmp/h.csv  # 120s interval, custom path
#
# Stop with Ctrl-C. Re-running appends (header written only for a new file).

INTERVAL="${1:-60}"
OUT="${2:-./target-health.csv}"

# --- locate the relevant IRQ numbers from /proc/interrupts ---------------
# xhci (USB host the DAC hangs off) and eth0. Match by the action label.
find_irq() {
    # $1 = grep pattern for the IRQ action column
    awk -v pat="$1" '
        $1 ~ /^[0-9]+:$/ && $0 ~ pat {
            n=$1; sub(":","",n); print n; exit
        }' /proc/interrupts
}

XHCI_IRQ="$(find_irq 'xhci')"
ETH_IRQ="$(find_irq 'eth0')"

[ -z "$XHCI_IRQ" ] && echo "WARN: no xhci IRQ found in /proc/interrupts" >&2
[ -z "$ETH_IRQ" ]  && echo "WARN: no eth0 IRQ found in /proc/interrupts" >&2

# Sum the counts across all CPUs for one IRQ line.
irq_total() {
    # $1 = irq number; empty -> 0
    [ -z "$1" ] && { echo 0; return; }
    awk -v irq="$1:" '$1==irq { s=0; for(i=2;i<=NF;i++){ if($i ~ /^[0-9]+$/) s+=$i } print s; exit }' /proc/interrupts
}

# NET_RX softirq counts, one column per CPU (space-separated).
netrx_percpu() {
    awk '/NET_RX:/ { out=""; for(i=2;i<=NF;i++) out=out (i>2?" ":"") $i; print out; exit }' /proc/softirqs
}
ncpu="$(grep -c '^processor' /proc/cpuinfo 2>/dev/null || echo 4)"

# --- header --------------------------------------------------------------
if [ ! -f "$OUT" ]; then
    hdr="ts,uptime_s,temp_c,throttled,xhci_irq_per_s,eth_irq_per_s"
    i=0; while [ "$i" -lt "$ncpu" ]; do hdr="$hdr,netrx_cpu${i}_per_s"; i=$((i+1)); done
    echo "$hdr" > "$OUT"
fi

echo "[monitor] xhci_irq=$XHCI_IRQ eth_irq=$ETH_IRQ interval=${INTERVAL}s -> $OUT" >&2
echo "[monitor] columns after throttled are PER-SECOND deltas. Ctrl-C to stop." >&2

# --- prime the deltas ----------------------------------------------------
prev_xhci="$(irq_total "$XHCI_IRQ")"
prev_eth="$(irq_total "$ETH_IRQ")"
prev_netrx="$(netrx_percpu)"
prev_t="$(awk '{print $1}' /proc/uptime)"

while :; do
    sleep "$INTERVAL"

    now_t="$(awk '{print $1}' /proc/uptime)"
    dt="$(awk -v a="$prev_t" -v b="$now_t" 'BEGIN{ d=b-a; if(d<=0)d=1; print d }')"

    cur_xhci="$(irq_total "$XHCI_IRQ")"
    cur_eth="$(irq_total "$ETH_IRQ")"
    cur_netrx="$(netrx_percpu)"

    d_xhci="$(awk -v a="$prev_xhci" -v b="$cur_xhci" -v dt="$dt" 'BEGIN{printf "%.1f",(b-a)/dt}')"
    d_eth="$(awk  -v a="$prev_eth"  -v b="$cur_eth"  -v dt="$dt" 'BEGIN{printf "%.1f",(b-a)/dt}')"

    # per-CPU NET_RX deltas
    netrx_cols="$(awk -v prev="$prev_netrx" -v cur="$cur_netrx" -v dt="$dt" 'BEGIN{
        np=split(prev,P," "); nc=split(cur,C," ");
        n=(nc<np?nc:np); out="";
        for(i=1;i<=n;i++){ d=(C[i]-P[i])/dt; out=out (i>1?",":"") sprintf("%.1f",d) }
        print out
    }')"

    # thermal
    if command -v vcgencmd >/dev/null 2>&1; then
        temp_c="$(vcgencmd measure_temp 2>/dev/null | sed -e "s/temp=//" -e "s/'C//")"
        throttled="$(vcgencmd get_throttled 2>/dev/null | sed 's/throttled=//')"
    else
        tz=/sys/class/thermal/thermal_zone0/temp
        [ -r "$tz" ] && temp_c="$(awk '{printf "%.1f",$1/1000}' "$tz")" || temp_c="NA"
        throttled="NA"
    fi

    ts="$(date '+%Y-%m-%d %H:%M:%S')"
    up="$(awk '{printf "%d",$1}' /proc/uptime)"
    echo "$ts,$up,$temp_c,$throttled,$d_xhci,$d_eth,$netrx_cols" >> "$OUT"

    prev_xhci="$cur_xhci"; prev_eth="$cur_eth"
    prev_netrx="$cur_netrx"; prev_t="$now_t"
done
