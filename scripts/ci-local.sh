#!/usr/bin/env bash
# Runs the checks of .github/workflows/ci.yml that can run on this machine,
# so that a push rarely meets a red CI. When a job in ci.yml changes, change
# its check here too; the layering pattern and the tool versions are read
# from ci.yml itself.
#
#   scripts/ci-local.sh                every check below except ubsan
#   scripts/ci-local.sh format tidy    only the checks named
#
# Checks:
#   layering        no GPU or window headers in the GPU-free layers (ADR 0003)
#   format          clang-format over every tracked .cpp and .hpp
#   build           the debug preset: configure if needed, build, run the tests
#   tidy            clang-tidy over every tracked engine and app .cpp
#   linux-warnings  macOS only: Homebrew's Clang over the same compile commands,
#                   which gives warnings Apple Clang does not and Linux Clang does
#   ubsan           macOS only, not run by default: the mac-ubsan preset's build
#                   and tests, the UndefinedBehaviorSanitizer half of CI's
#                   sanitizer job
#
# clang-format and clang-tidy must be the versions ci.yml pins. Put them on
# PATH, or name them in CLANG_FORMAT and CLANG_TIDY, for example after
#   python3 -m venv ~/.ci-tools
#   ~/.ci-tools/bin/pip install clang-format==<version> clang-tidy==<version>
# JOBS limits parallel compiles and clang-tidy runs (default 3, for 8 GB).
# LINUX_CLANG names the compiler for linux-warnings (default: Homebrew's).

set -uo pipefail

root=$(git rev-parse --show-toplevel) || exit 2
cd "$root" || exit 2
workflow=.github/workflows/ci.yml
jobs=${JOBS:-3}

case "$(uname -s)" in
Darwin) preset=mac-debug ;;
*) preset=linux-clang ;;
esac

say() { printf '\n== %s\n' "$*"; }
fail() { printf 'ci-local: %s\n' "$*" >&2; }

# The version ci.yml pins for a tool, such as CLANG_FORMAT_VERSION.
pinned() {
    sed -nE "s/^[[:space:]]*$1:[[:space:]]*\"?([0-9][0-9.]*)\"?[[:space:]]*$/\1/p" "$workflow" | head -n 1
}

# The tool, if it reports the version ci.yml pins.
pinned_tool() {
    local tool=$1 variable=$2 version
    version=$(pinned "$variable")
    if [ -z "$version" ]; then
        fail "no $variable in $workflow"
        return 1
    fi
    if ! "$tool" --version 2>/dev/null | grep -qE "version ${version//./\\.}([^0-9.]|$)"; then
        fail "$tool is not version $version, which ci.yml pins; see the top of this script"
        return 1
    fi
}

check_layering() {
    # CI's own pattern and folders, from its git grep line.
    local line pattern folders status=0
    line=$(grep -m 1 "git grep -nE" "$workflow")
    pattern=$(printf '%s\n' "$line" | sed -nE "s/.*git grep -nE '([^']*)'.*/\1/p")
    folders=$(printf '%s\n' "$line" | sed -nE "s/.* -- ([^|]*)\|\|.*/\1/p")
    if [ -z "$pattern" ] || [ -z "$folders" ]; then
        fail "could not read the layering pattern from $workflow, so nothing was checked"
        return 1
    fi
    # shellcheck disable=SC2086 # the folders are words
    git grep -nE "$pattern" -- $folders || status=$?
    if [ "$status" -eq 0 ]; then
        fail "GPU or window headers in $folders (ADR 0003)"
        return 1
    elif [ "$status" -ne 1 ]; then
        fail "git grep failed with status $status, so nothing was checked"
        return 1
    fi
}

check_format() {
    local tool=${CLANG_FORMAT:-clang-format}
    pinned_tool "$tool" CLANG_FORMAT_VERSION || return 1
    git ls-files '*.cpp' '*.hpp' | xargs "$tool" --dry-run --Werror
}

# The tests that need a GPU find the Vulkan loader through the SDK's
# environment on macOS.
vulkan_environment() {
    [ "$(uname -s)" = Darwin ] || return 0
    [ -n "${VULKAN_SDK:-}" ] && return 0
    local setup
    setup=$(ls -d "$HOME"/VulkanSDK/*/setup-env.sh 2>/dev/null | sort -V | tail -n 1)
    if [ -z "$setup" ]; then
        fail "no Vulkan SDK in ~/VulkanSDK, so the GPU tests have no loader"
        return 1
    fi
    # The SDK's script reads variables that may be unset.
    set +u
    # shellcheck disable=SC1090 # the SDK's own script
    source "$setup" >/dev/null
    set -u
}

build_and_test() {
    local name=$1
    if [ ! -f "build/$name/CMakeCache.txt" ]; then
        cmake --preset "$name" || return 1
    fi
    cmake --build --preset "$name" -j "$jobs" || return 1
    vulkan_environment || return 1
    ctest --preset "$name"
}

check_build() { build_and_test "$preset"; }

check_tidy() {
    local tool=${CLANG_TIDY:-clang-tidy}
    pinned_tool "$tool" CLANG_TIDY_VERSION || return 1
    # clang-tidy reads the compile commands and the generated headers, such
    # as the compiled shaders, so it needs a built tree, as in CI.
    if [ ! -f "build/$preset/compile_commands.json" ]; then
        fail "no build/$preset/compile_commands.json: run the build check first"
        return 1
    fi
    git ls-files 'engine/*.cpp' 'app/*.cpp' |
        xargs -P "$jobs" -n 4 "$tool" -p "build/$preset" --quiet
}

check_linux_warnings() {
    if [ "$(uname -s)" != Darwin ]; then
        echo "not macOS: the Linux jobs run the real compilers"
        return 0
    fi
    local clang=${LINUX_CLANG:-$(brew --prefix llvm 2>/dev/null)/bin/clang++}
    if [ ! -x "$clang" ]; then
        fail "no Homebrew Clang at $clang (brew install llvm, or set LINUX_CLANG)"
        return 1
    fi
    if [ ! -f "build/$preset/compile_commands.json" ]; then
        fail "no build/$preset/compile_commands.json: run the build check first"
        return 1
    fi
    "$clang" --version | head -n 1
    # Every compile command for our own sources, with the compiler swapped,
    # no -c or output file, and syntax only.
    python3 - "$clang" "build/$preset/compile_commands.json" "$root" "$jobs" <<'EOF'
import json, os, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

clang, database, root, jobs = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
ours = ("engine/", "app/", "tests/", "bench/")

def command(entry):
    words = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
    words = [clang] + words[1:]
    out = []
    skip = False
    for word in words:
        if skip:
            skip = False
            continue
        if word == "-o":
            skip = True
            continue
        if word == "-c":
            continue
        out.append(word)
    return out + ["-fsyntax-only"]

# The database may name the tree through a symbolic link (/tmp for
# /private/tmp on macOS), so paths are compared resolved.
root = os.path.realpath(root)
entries = []
for entry in json.load(open(database)):
    path = os.path.relpath(os.path.realpath(os.path.join(entry["directory"], entry["file"])), root)
    if path.startswith(ours):
        entries.append((path, entry))
if not entries:
    sys.exit("no compile commands for engine/, app/, tests/ or bench/, so nothing was checked")

def run(item):
    path, entry = item
    done = subprocess.run(command(entry), cwd=entry["directory"], capture_output=True, text=True)
    return path, done.returncode, done.stderr

failed = 0
with ThreadPoolExecutor(max_workers=jobs) as pool:
    for path, code, errors in pool.map(run, entries):
        if code != 0:
            failed += 1
            sys.stderr.write(errors)
print(f"{len(entries)} files, {failed} with errors")
sys.exit(1 if failed else 0)
EOF
}

check_ubsan() {
    if [ "$(uname -s)" != Darwin ]; then
        echo "not macOS: CI's sanitizer job runs both sanitizers"
        return 0
    fi
    build_and_test mac-ubsan
}

all=(layering format build tidy linux-warnings)
known=" layering format build tidy linux-warnings ubsan "
if [ "$#" -eq 0 ]; then
    set -- "${all[@]}"
fi
for name in "$@"; do
    if [[ "$known" != *" $name "* ]]; then
        fail "unknown check '$name'; known:$known"
        exit 2
    fi
done

passed=() failed=()
for name in "$@"; do
    say "$name"
    if "check_${name//-/_}"; then
        passed+=("$name")
    else
        failed+=("$name")
    fi
done

say "summary"
echo "passed: ${passed[*]:-none}"
echo "failed: ${failed[*]:-none}"
echo "not run here: Linux (gcc), Linux (lavapipe) and Linux (ASan + UBSan) need Linux;"
echo "              on macOS, 'ubsan' runs the UndefinedBehaviorSanitizer half."
[ "${#failed[@]}" -eq 0 ]
