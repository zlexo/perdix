#!/usr/bin/env bash
# Fuzzes a reader of files from outside the project with libFuzzer, one of
# the targets in tests/fuzz:
#
#   archive              the archive reader (RFC 0009): bytes mutated from the
#                        test archives go through Archive::from_bytes, names
#                        and read
#   version_description  the launcher's version descriptions (RFC 0009): bytes
#                        mutated from the seed descriptions and release times
#                        read as a description and as a release time
#
# with UndefinedBehaviorSanitizer, the standard library's bounds checks, and a
# check of every heap read: on Linux AddressSanitizer; on macOS, where it does
# not run, Guard Malloc, which places each allocation against an unmapped
# page, so a read even one byte past it crashes.
#
#   scripts/fuzz.sh [seconds] [target]   180 seconds of archive by default
#
# It needs a configured debug build (the compile commands give each source's
# flags) and a Clang that ships libFuzzer, which Apple's does not: on macOS,
# brew install llvm. FUZZ_CXX names another; FUZZ_GUARD_MALLOC=0 leaves Guard
# Malloc out, which runs faster and checks less. Each target's corpus grows in
# build/fuzz/<target> from run to run; an input that stops it is kept there as
# crash-<hash>.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"
seconds=${1:-180}
case "$seconds" in
'' | *[!0-9]* | 0)
    echo "fuzz: give a number of seconds above 0, not '$seconds'" >&2
    exit 2
    ;;
esac
target=${2:-archive}
# What each target is built from, its include folders, and the files its
# corpus starts from. A source named by its file name alone, such as a
# fetched dependency's, is found wherever the build put it.
case "$target" in
archive)
    sources=(engine/core/src/archive.cpp engine/core/src/crc32.cpp engine/core/src/stb_image.cpp)
    includes=(-I engine/core/include)
    seeds=(tests/core/archives/*.zip)
    ;;
version_description)
    sources=(engine/minecraft/src/version_description.cpp engine/minecraft/src/json.cpp yyjson.c)
    includes=(-I engine/core/include -I engine/minecraft/include)
    seeds=(tests/minecraft/descriptions/*.json tests/minecraft/release_times/*.txt)
    ;;
*)
    echo "fuzz: no target '$target': archive or version_description" >&2
    exit 2
    ;;
esac
out=build/fuzz/$target
mkdir -p "$out/corpus"

guard_malloc=0
case "$(uname -s)" in
Darwin)
    preset=mac-debug
    llvm=$(brew --prefix llvm 2>/dev/null || true)
    cxx=${FUZZ_CXX:-${llvm:-/opt/homebrew/opt/llvm}/bin/clang++}
    install="brew install llvm"
    # Apple's linker by its path: another ld earlier on PATH may not take
    # LLVM's objects. libc++ checks bounds in its debug hardening mode.
    link=(--ld-path=/usr/bin/ld)
    checks=(-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG)
    sanitizers=undefined
    if [ "${FUZZ_GUARD_MALLOC:-1}" != 0 ]; then
        guard_malloc=1
        # Allocations end exactly at a page, so they are not aligned, and
        # UBSan's alignment check would stop on every one.
        checks+=(-fno-sanitize=alignment)
    fi
    ;;
*)
    preset=linux-clang
    cxx=${FUZZ_CXX:-$(command -v clang++-18 || command -v clang++ || echo clang++)}
    install="install Clang 18, or set FUZZ_CXX"
    link=()
    checks=(-D_GLIBCXX_ASSERTIONS)
    sanitizers=address,undefined
    ;;
esac
if [ ! -x "$(command -v "$cxx" || true)" ]; then
    echo "fuzz: no Clang with libFuzzer at $cxx ($install)" >&2
    exit 2
fi
database=build/$preset/compile_commands.json
if [ ! -f "$database" ]; then
    echo "fuzz: no $database: configure the $preset preset first" >&2
    exit 2
fi

# The sources the target needs, each compiled with its own flags from the
# compile commands, the compiler swapped (to LLVM's C compiler for C) and the
# fuzzer's coverage added.
sanitize=(-g -O1 "-fsanitize=fuzzer-no-link,$sanitizers" -fno-sanitize-recover=all)
python3 - "$cxx" "$database" "$root" "$out" "${#sources[@]}" "${sources[@]}" \
    "${sanitize[@]}" "${checks[@]}" <<'PY'
import json, os, shlex, subprocess, sys
cxx, database, root, out, count, *rest = sys.argv[1:]
wanted, extra = set(rest[:int(count)]), rest[int(count):]
cc = os.path.join(os.path.dirname(cxx), os.path.basename(cxx).replace("clang++", "clang"))
root = os.path.realpath(root)
done = set()
for entry in json.load(open(database)):
    path = os.path.relpath(os.path.realpath(os.path.join(entry["directory"], entry["file"])), root)
    if path not in wanted:
        path = os.path.basename(path)
    if path not in wanted or path in done:
        continue
    words = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
    command, skip = [cc if path.endswith(".c") else cxx], False
    for word in words[1:]:
        if skip:
            skip = False
        elif word == "-o":
            skip = True
        elif word != "-c":
            command.append(word)
    target = os.path.join(root, out, os.path.basename(path) + ".o")
    subprocess.run(command + extra + ["-c", "-o", target], cwd=entry["directory"], check=True)
    done.add(path)
if done != wanted:
    sys.exit(f"fuzz: no compile commands for {sorted(wanted - done)}")
PY
objects=()
for source in "${sources[@]}"; do
    objects+=("$out/$(basename "$source").o")
done
"$cxx" -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror \
    "${sanitize[@]}" "${checks[@]}" "-fsanitize=fuzzer,$sanitizers" "${includes[@]}" \
    "tests/fuzz/${target}_fuzz.cpp" "${objects[@]}" \
    ${link[@]+"${link[@]}"} -o "$out/${target}_fuzz"

# The seeds, and the inputs that once stopped this target, which the tests
# replay.
cp "${seeds[@]}" "$out/corpus/"
if [ -d "tests/fuzz/inputs/$target" ]; then
    cp -R "tests/fuzz/inputs/$target"/. "$out/corpus/"
fi
# The memory limit holds where AddressSanitizer counts allocations; the
# resident limit holds everywhere.
run=("$out/${target}_fuzz" -max_total_time="$seconds" -malloc_limit_mb=64 -rss_limit_mb=1024
    -print_final_stats=1 -artifact_prefix="$out/" "$out/corpus")
if [ "$guard_malloc" = 1 ]; then
    DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib MALLOC_STRICT_SIZE=1 "${run[@]}"
else
    "${run[@]}"
fi
