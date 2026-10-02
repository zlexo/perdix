# Contributing

This engine is built slowly on purpose. Every system is designed before it is written, every change lands through a reviewed pull request, and every performance claim comes with a number.

## Who contributes

The code is source-available, not open source: anyone may read it, but nobody may use or copy it without permission (see [LICENSE](LICENSE)). Issues, bug reports and benchmark results are welcome. Pull requests from outside the project are not accepted for now; this guide describes how the project's own work moves.

## How a feature moves

1. **Design.** A design document (RFC) in [`docs/design/`](docs/design/) describes the whole feature: the problem, goals and non-goals, prior art, the options and their trade-offs, the chosen design (data layout, algorithms, API sketch), a performance budget, a test plan and the list of pull requests that will build it. It starts as `Draft` and becomes `Accepted` after review. **No feature code is written before its RFC is accepted.** Chores and small fixes are exempt.
2. **Issues.** Each pull request in the RFC's plan gets an issue on the right milestone.
3. **Pull requests.** One at a time, small, one concern each. See [Pull requests](#pull-requests).
4. **As built.** If the implementation had to differ from the design, the RFC gets an "As built" section that says how and why.
5. **Release.** A milestone ends with a tag (`v0.N.0`) and a changelog entry.

An RFC covers its feature from start to finish. Phasing belongs in its pull request plan, never in the definition of the feature.

Decisions that cut across the codebase and are expensive to reverse get an ADR (architecture decision record) in [`docs/adr/`](docs/adr/). Accepted ADRs are not edited; a later ADR supersedes them.

## Commits

Commits follow [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <summary in the imperative, lower case, no period>

<body: what changed and why, wrapped at 72 columns>
```

| Type | Use for |
|---|---|
| `feat` | new behavior |
| `fix` | a bug fix |
| `perf` | a change made for speed or memory, with numbers in the body |
| `refactor` | restructuring without behavior change |
| `style` | formatting only |
| `test` | tests only |
| `docs` | documentation, RFCs, ADRs |
| `build` | CMake, dependencies, toolchains |
| `ci` | workflows |
| `chore` | everything else |

Scopes follow the source layout (`core`, `world`, `mesh`, `render`, `app`), plus `rfc` and `adr` for design documents and `github` for templates and repository settings. A commit to an RFC or an ADR puts its number after the scope (`docs(adr): 0003 keep the world layer free of GPU code`). The commit that adds a new RFC is the one exception to the imperative: its summary is the RFC's title, as in `docs(rfc): 0002 chunk storage`.

- **Small and frequent.** One coherent step per commit. A change, its tests and its documentation are usually separate commits.
- **Every commit builds, and from the commit that adds a test on, the tests pass,** so history stays bisectable.
- **Say why.** The summary says what changed; the body says why, and what it costs.
- **Subjects are at most 72 characters,** type and scope included, like the body's lines.

## Pull requests

- Branch from `main` as `<type>/<topic>`, for example `feat/chunk-palette`. An RFC's branch is `rfc/NNNN-short-title`, for example `rfc/0002-chunk-storage`.
- Keep the diff reviewable in one sitting, roughly 400 changed lines at most.
- Fill in the template: what, why, how it was tested, and before and after numbers for anything on a hot path. Visual changes include a screenshot or a short clip.
- CI must be green: build, tests, formatting and, once rendering exists, zero Vulkan validation errors.
- Pull requests are merged with a merge commit, never squashed, so the small commits stay in history and `git log --first-parent` reads as a list of pull requests.

## Performance claims

Numbers come from the benchmark harness on the reference machine (an Apple M3 with 8 GB, see the vision), and the pull request says how they were measured. Shared CI runners are too noisy for timing, so CI never asserts a time budget; it only checks that benchmarks still run.

## Code style

Formatting is enforced by `clang-format` with the configuration in the repository root, at the version CI pins. Install the same release with `pipx install clang-format==22.1.8` and check that `clang-format --version` reports 22.1.8: other versions format the same file differently, and older ones, such as Homebrew's 14, reject the configuration. Static analysis runs `clang-tidy` with the repository's `.clang-tidy`, also pinned: `pipx install clang-tidy==22.1.8`, then `git ls-files 'engine/*.cpp' 'app/*.cpp' | xargs clang-tidy -p build/mac-debug` after configuring. It also checks naming. Naming, error handling and the rest of the conventions are set in RFC 0000.
