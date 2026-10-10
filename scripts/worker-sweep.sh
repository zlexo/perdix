#!/usr/bin/env bash
# RFC 0005's worker sweep: the benchmark flythrough with 1 to 8 workers at
# each QoS mode (switching, utility, user-initiated), a few runs of each,
# then one table of the medians: how fast the world fills in, and what it
# costs the frames and the world thread.
#
#   scripts/worker-sweep.sh [runs] [workers...]   3 runs of 1 to 8 by default
#
# Each run is a full flythrough, about a minute and a half, in a window that
# takes the screen: run it plugged in, with nothing else open. The reports
# are kept in build/sweep/<date>/, one a run, and the table is printed and
# saved beside them as table.md. PERDIX names another executable than the
# release build's; QOS lists other modes; SUMMARIZE=<folder> prints the table
# of a sweep already run, running nothing.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"
runs=${1:-3}
shift || true
workers=("$@")
if [ ${#workers[@]} -eq 0 ]; then
    workers=(1 2 3 4 5 6 7 8)
fi
read -r -a modes <<<"${QOS:-switching utility user-initiated}"
# One row a mode and worker count: the median of its runs for each number.
# A report's phase row reads, split at "|": phase, time, frame p50 / p99 /
# max, longest step / pass, generated / uniform / meshed a second, holes
# within 256 / all, then memory.
summarize() {
    awk -v runs="$runs" '
        function median(values, count,    i, j, swap) {
            for (i = 2; i <= count; ++i) {
                for (j = i; j > 1 && values[j - 1] > values[j]; --j) {
                    swap = values[j]; values[j] = values[j - 1]; values[j - 1] = swap
                }
            }
            return count % 2 == 1 ? values[(count + 1) / 2] \
                                  : (values[count / 2] + values[count / 2 + 1]) / 2
        }
        function keep(name, value) {
            if (value != "-") {
                taken[name, ++counted[name]] = value + 0
            }
        }
        function take(name,    i, values) {
            for (i = 1; i <= counted[name]; ++i) {
                values[i] = taken[name, i]
            }
            return counted[name] > 0 ? median(values, counted[name]) : -1
        }
        FNR == 1 {
            # A new report: its configuration from its file name.
            n = split(FILENAME, parts, "/")
            split(parts[n], name, "-")
            mode = name[1] == "user" ? "user-initiated" : name[1]
            count = name[1] == "user" ? name[3] : name[2]
            config = mode " " count
            if (!(config in seen)) {
                seen[config] = 1
                order[++configs] = config
            }
            longest = 0
        }
        /^\| [a-z]+ \| [0-9]/ {
            split($0, cell, "|")
            phase = cell[2]; gsub(/ /, "", phase)
            split(cell[4], frame, "/")
            split(cell[5], world, "/")
            split(cell[6], rate, "/")
            split(cell[7], holes, "/")
            gsub(/[^0-9.]/, "", world[2])
            if (world[2] + 0 > longest) {
                longest = world[2] + 0
            }
            if (phase == "load") {
                time = cell[3]; gsub(/[^0-9.]/, "", time)
                keep(config SUBSEP "load", time)
                keep(config SUBSEP "generated", rate[1])
                keep(config SUBSEP "meshed", rate[3])
            }
            if (phase == "hover" || phase == "cruise" || phase == "sprint") {
                keep(config SUBSEP phase "50", frame[1])
                keep(config SUBSEP phase "99", frame[2])
            }
            if (phase == "sprint") {
                keep(config SUBSEP "holes", holes[1])
            }
            if (phase == "teleport") {
                keep(config SUBSEP "pass", longest)
            }
        }
        END {
            print "| QoS | Workers | Load | Generated / meshed a second | Hover p50 / p99 | Cruise p50 / p99 | Sprint p50 / p99 | Sprint holes p99 within 256 | Longest pass |"
            print "|---|---|---|---|---|---|---|---|---|"
            for (i = 1; i <= configs; ++i) {
                c = order[i]
                split(c, part, " ")
                printf "| %s | %s | %.2f s | %.0f / %.0f | %.1f / %.1f ms | %.1f / %.1f ms | %.1f / %.1f ms | %.0f | %.2f ms |\n",
                    part[1], part[2], take(c SUBSEP "load"), take(c SUBSEP "generated"),
                    take(c SUBSEP "meshed"), take(c SUBSEP "hover50"), take(c SUBSEP "hover99"),
                    take(c SUBSEP "cruise50"), take(c SUBSEP "cruise99"),
                    take(c SUBSEP "sprint50"), take(c SUBSEP "sprint99"),
                    take(c SUBSEP "holes"), take(c SUBSEP "pass")
            }
            printf "\nMedians of %d runs each.\n", runs
        }' "$@"
}

if [ -n "${SUMMARIZE:-}" ]; then
    summarize "$SUMMARIZE"/[a-z]*-[0-9]*-[0-9]*.md
    exit 0
fi
perdix=${PERDIX:-build/mac-release/app/perdix}
if [ ! -x "$perdix" ]; then
    echo "error: no $perdix: build the release preset first" >&2
    exit 1
fi
out=build/sweep/$(date +%Y-%m-%d-%H%M)
mkdir -p "$out"

# Run by run, each a pass over every configuration, so the machine's
# warming over the sweep (the reference machine has no fan) falls on every
# configuration alike instead of on the last ones.
for run in $(seq 1 "$runs"); do
    for count in "${workers[@]}"; do
        for mode in "${modes[@]}"; do
            report="$out/$mode-$count-$run.md"
            echo "run $run of $runs: $mode, $count workers" >&2
            if ! "$perdix" --flythrough --workers="$count" --worker-qos="$mode" >"$report"; then
                echo "error: the flythrough failed: $report" >&2
                exit 1
            fi
        done
    done
done

summarize "$out"/[a-z]*-[0-9]*-[0-9]*.md | tee "$out/table.md"
echo "The reports and the table: $out" >&2
