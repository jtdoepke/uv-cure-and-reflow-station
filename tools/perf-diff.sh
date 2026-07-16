#!/usr/bin/env bash
# Diff a perf run against the committed baseline: tools/perf-diff.sh BASELINE CURRENT
#
# Two classes of number, treated differently on purpose:
#
#   EXACT    task counts, chunk counts, byte counts. Deterministic and machine-independent — the
#            same code emits the same tasks everywhere. A change here is always signal.
#   NOISY    the microsecond timings. Real, but hostage to the machine. Anything inside +/-NOISE_PCT
#            prints as `~` rather than a number, because a diff tool that reports noise as signal
#            trains you to ignore it.
#
# Exit 0 always: this reports, it does not gate. CI gates the exact metrics separately.
set -euo pipefail

BASE=${1:?usage: perf-diff.sh BASELINE CURRENT}
CUR=${2:?usage: perf-diff.sh BASELINE CURRENT}
NOISE_PCT=${NOISE_PCT:-2}

if [[ ! -f $BASE ]]; then
  echo "no baseline at $BASE — run: make perf-baseline" >&2
  exit 0
fi

echo "baseline: $(grep -m1 '^# git=' "$BASE" || echo '(no provenance)')"
echo "current:  $(grep -m1 '^# git=' "$CUR" || echo '(no provenance)')"
echo

# Join on "scenario<TAB>metric". Both files are emitted in a stable scenario order, but sort
# anyway so a reordered scenario list can't silently mis-pair rows.
join -t'	' -j1 \
  <(grep -v '^#' "$BASE" | awk -F'\t' 'NF>=4{printf "%s|%s\t%s\t%s\n",$1,$2,$3,$4}' | sort) \
  <(grep -v '^#' "$CUR" | awk -F'\t' 'NF>=4{printf "%s|%s\t%s\n",$1,$2,$3}' | sort) \
  | awk -F'\t' -v noise="$NOISE_PCT" '
    BEGIN { printf "%-34s %12s %12s %10s\n", "scenario|metric", "baseline", "current", "delta" }
    {
      key=$1; base=$2; unit=$3; cur=$4;
      # us is wall-noisy by nature; kb (RSS) is quantized by the allocator and jitters just as
      # much. Everything else — task/chunk counts, LVGL heap bytes — is deterministic.
      noisy = (unit == "us" || unit == "kb");
      if (!noisy) {
        # An exact metric that did not move is not a finding. Report the absolute delta, not a
        # percentage: "+800 tasks" is the sentence you want, "+80.9%" is not.
        if (cur+0 == base+0) { d = "="; }
        else { d = sprintf("%+d", cur - base); changed++; }
      }
      else if (base+0 == 0) { d = (cur+0 == 0) ? "~" : "NEW"; if (d == "NEW") changed++; }
      else {
        pct = (cur - base) * 100.0 / base;
        if (pct < noise && pct > -noise) d = "~";
        else { d = sprintf("%+.1f%%", pct); changed++; }
      }
      printf "%-34s %12s %12s %10s\n", key, base, cur, d;
    }
    END {
      printf "\n%d metric(s) moved (exact metrics: any change; timings: outside +/-%s%%)\n",
             changed+0, noise
    }
  '
