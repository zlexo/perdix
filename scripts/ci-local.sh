#!/usr/bin/env bash
# Runs the checks of .github/workflows/ci.yml that can run on this machine,
# so that a push rarely meets a red CI. When a job in ci.yml changes, change
# its check here too; the layering and Minecraft file patterns and the tool
# versions are read from ci.yml itself.
#
#   scripts/ci-local.sh                every check below except ubsan
#   scripts/ci-local.sh format tidy    only the checks named
#
# Checks:
#   layering        no GPU or window headers in the GPU-free layers (ADR 0003)
#   minecraft-files nothing of Minecraft's own committed: no assets/minecraft/
#                   folder and no jar (RFC 0009)
#   format          clang-format over every tracked .cpp and .hpp
#   build           the debug preset: configure if needed, build, run the tests
#   tidy            clang-tidy over every tracked engine and app .cpp
#   linux-warnings  macOS only: Homebrew's Clang over the same compile commands,
#                   against GCC's libstdc++ headers as on Linux, which gives the
#                   warnings and missing includes that Linux Clang has and Apple
#                   Clang does not (over libc++, doctest takes other overloads)
#   gcc-warnings    macOS only: Homebrew's GCC over the same compile commands,
#                   syntax only, which gives GCC's own warnings (sign
#                   conversion, dangling references, shadowing) that no Clang
#                   gives; GCC 15 here, a little stricter than CI's GCC 13
#   ubsan           macOS only, not run by default: the mac-ubsan preset's build
#                   and tests, the UndefinedBehaviorSanitizer half of CI's
#                   sanitizer job
#
# Not here: AddressSanitizer and ThreadSanitizer (CI's sanitizer and TSan
# jobs). On macOS 26 their builds hang or crash before main, with Apple
# Clang 17 and Homebrew Clang 20 alike.
#
# clang-format and clang-tidy must be the versions ci.yml pins. Put them on
# PATH, or name them in CLANG_FORMAT and CLANG_TIDY, for example after
#   python3 -m venv ~/.ci-tools
#   ~/.ci-tools/bin/pip install clang-format==<version> clang-tidy==<version>
# JOBS limits parallel compiles and clang-tidy runs (default 3, for 8 GB).
# LINUX_CLANG names the compiler for linux-warnings (default: Homebrew's).
# LINUX_STDLIB names the libstdc++ headers it uses (default: the newest of
# Homebrew's GCC, brew install gcc); LINUX_STDLIB=libc++ checks against the
# Mac's own instead, which cannot see those differences.
# LINUX_GCC names the g++ for gcc-warnings (default: Homebrew's newest).

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

check_minecraft_files() {
    # CI's own pattern, from its grep line, over the tracked paths written
    # NUL-separated, as CI writes them: git quotes a path with an unusual
    # character in a list of lines.
    local line pattern tracked found status=0
    line=$(grep -m 1 "grep -zinE" "$workflow")
    pattern=$(printf '%s\n' "$line" | sed -nE "s/.*grep -zinE '([^']*)'.*/\1/p")
    if [ -z "$pattern" ]; then
        fail "could not read the Minecraft files pattern from $workflow, so nothing was checked"
        return 1
    fi
    tracked=$(mktemp) || return 1
    found=$(mktemp) || return 1
    if ! git ls-files -z > "$tracked"; then
        fail "git ls-files failed, so nothing was checked"
        rm -f "$tracked" "$found"
        return 1
    fi
    grep -zinE "$pattern" "$tracked" > "$found" || status=$?
    tr '\0' '\n' < "$found"
    rm -f "$tracked" "$found"
    if [ "$status" -eq 0 ]; then
        fail "Minecraft's own files are never committed (RFC 0009): they are read from the player's install"
        return 1
    elif [ "$status" -ne 1 ]; then
        fail "grep failed with status $status, so nothing was checked"
        return 1
    fi
}

check_format() {
    local tool=${CLANG_FORMAT:-clang-format}
    pinned_tool "$tool" CLANG_FORMAT_VERSION || return 1
    git ls-files '*.cpp' '*.hpp' | xargs "$tool" --dry-run --Werror
}

# The tests that need a GPU find the Vulkan loader through the SDK's
# environment on macOS, set up or put back by vulkan_environment.
# shellcheck source=scripts/vulkan-environment.sh
source "$root/scripts/vulkan-environment.sh"

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
    local stdlib=${LINUX_STDLIB:-} target=
    if [ -z "$stdlib" ]; then
        stdlib=$(ls -d "$(brew --prefix gcc 2>/dev/null)"/include/c++/[0-9]* 2>/dev/null | sort -V | tail -n 1)
    fi
    if [ -z "$stdlib" ]; then
        fail "no libstdc++ headers (brew install gcc, or set LINUX_STDLIB; LINUX_STDLIB=libc++ uses the Mac's)"
        return 1
    fi
    "$clang" --version | head -n 1
    if [ "$stdlib" = libc++ ]; then
        echo "standard library: the Mac's libc++, so Linux's doctest overloads and missing includes go unchecked"
    else
        # The target's own headers, such as bits/c++config.h, sit in a
        # folder named for the target.
        target=$(ls -d "$stdlib"/*/bits/c++config.h 2>/dev/null | head -n 1)
        target=${target%/bits/c++config.h}
        if [ -z "$target" ]; then
            fail "$stdlib has no <target>/bits/c++config.h, so it is not libstdc++'s headers"
            return 1
        fi
        echo "standard library: libstdc++ $(basename "$stdlib") from $stdlib"
    fi
    # Every compile command for our own sources, with the compiler swapped,
    # the standard library's headers replaced, no -c or output file, and
    # syntax only.
    python3 - "$clang" "build/$preset/compile_commands.json" "$root" "$jobs" "$stdlib" "$target" <<'EOF'
import json, os, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

clang, database, root, jobs = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
stdlib, target = sys.argv[5], sys.argv[6]
headers = [] if stdlib == "libc++" else ["-nostdinc++", "-isystem", stdlib, "-isystem", target]
ours = ("engine/", "app/", "tests/", "bench/")

def command(entry):
    words = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
    words = [clang] + headers + words[1:]
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

check_gcc_warnings() {
    if [ "$(uname -s)" != Darwin ]; then
        echo "not macOS: CI's Linux (gcc) job runs GCC itself"
        return 0
    fi
    local gxx=${LINUX_GCC:-}
    if [ -z "$gxx" ]; then
        gxx=$(ls "$(brew --prefix gcc 2>/dev/null)"/bin/g++-[0-9]* 2>/dev/null | sort -V | tail -n 1)
    fi
    if [ -z "$gxx" ] || [ ! -x "$gxx" ]; then
        fail "no Homebrew GCC (brew install gcc, or set LINUX_GCC)"
        return 1
    fi
    if [ ! -f "build/$preset/compile_commands.json" ]; then
        fail "no build/$preset/compile_commands.json: run the build check first"
        return 1
    fi
    "$gxx" --version | head -n 1
    # Homebrew's GCC keeps copies of the SDK's headers fixed for the SDK it
    # was built on, which break on a newer one ("'FILE' does not name a
    # type"). GCC's own fixincludes makes them again for this machine's SDK
    # into a folder of ours, which the compiler then reads in place of its
    # copy. GCC's install is not touched.
    local sdk tools data machine fixed status=0
    sdk=$(xcrun --show-sdk-path) || return 1
    tools=$(dirname "$("$gxx" -print-prog-name=cc1plus)")/install-tools
    data=$(dirname "$("$gxx" -print-libgcc-file-name)")/install-tools
    machine=$("$gxx" -dumpmachine)
    if [ ! -x "$tools/fixinc.sh" ] || [ ! -f "$data/macro_list" ]; then
        fail "$gxx has no fixincludes tools in $tools"
        return 1
    fi
    fixed=$(mktemp -d) || return 1
    mkdir -p "$fixed/tools" "$fixed/include-fixed"
    # fixinc.sh runs fixincl from its working folder: copies of both.
    cp "$tools/fixinc.sh" "$tools/fixincl" "$tools/mkinstalldirs" "$fixed/tools/"
    if ! (cd "$fixed/tools" &&
        TARGET_MACHINE=$machine target_canonical=$machine MACRO_LIST="$data/macro_list" \
            sh ./fixinc.sh "$fixed/include-fixed" "$sdk/usr/include" >"$fixed/fixinc.log" 2>&1); then
        fail "fixincludes failed; its log is $fixed/fixinc.log"
        return 1
    fi
    # As GCC's install does: the SDK's limits.h becomes syslimits.h, behind
    # GCC's own.
    rm -f "$fixed/include-fixed/syslimits.h"
    if [ -f "$fixed/include-fixed/limits.h" ]; then
        mv "$fixed/include-fixed/limits.h" "$fixed/include-fixed/syslimits.h"
    else
        cp "$data/gsyslimits.h" "$fixed/include-fixed/syslimits.h"
    fi
    cp "$data/include/limits.h" "$fixed/include-fixed/limits.h"
    # Every compile command for our own sources with GCC: its own search
    # list, include-fixed swapped for ours, no -arch, -c or output file, and
    # syntax only.
    python3 - "$gxx" "build/$preset/compile_commands.json" "$root" "$jobs" "$sdk" \
        "$fixed/include-fixed" <<'EOF' || status=$?
import json, os, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

gxx, database, root, jobs, sdk, fixed = sys.argv[1:7]
jobs = int(jobs)
ours = ("engine/", "app/", "tests/", "bench/")

shown = subprocess.run([gxx, "-isysroot", sdk, "-E", "-x", "c++", "-", "-v"],
                       stdin=subprocess.DEVNULL, capture_output=True, text=True).stderr
start, end = "#include <...> search starts here:", "End of search list."
if start not in shown or end not in shown:
    sys.exit(f"{gxx} printed no include search list, so nothing was checked")
search = ["-nostdinc"]
for line in shown.split(start, 1)[1].split(end, 1)[0].splitlines():
    folder = line.strip().removesuffix(" (framework directory)")
    if not folder:
        continue
    if os.path.basename(folder) == "include-fixed":
        folder = fixed
    search += ["-iframework" if folder.endswith("/Frameworks") else "-isystem", folder]

def command(entry):
    words = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
    out = [gxx] + search
    skip = False
    for word in words[1:]:
        if skip:
            skip = False
            continue
        if word in ("-o", "-arch"):
            skip = True
            continue
        if word == "-c":
            continue
        out.append(word)
    return out + ["-fsyntax-only"]

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
    rm -rf "$fixed"
    return "$status"
}

check_ubsan() {
    if [ "$(uname -s)" != Darwin ]; then
        echo "not macOS: CI's sanitizer job runs both sanitizers"
        return 0
    fi
    build_and_test mac-ubsan
}

all=(layering minecraft-files format build tidy linux-warnings gcc-warnings)
known=" layering minecraft-files format build tidy linux-warnings gcc-warnings ubsan "
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
echo "not run here: Linux (gcc)'s build and tests, Linux (lavapipe) and Linux (ASan + UBSan)"
echo "              need Linux; on macOS, 'ubsan' runs the UndefinedBehaviorSanitizer half."
[ "${#failed[@]}" -eq 0 ]
