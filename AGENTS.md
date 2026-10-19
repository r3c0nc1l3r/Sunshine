On Windows we use msys2 and ucrt64 to compile.
You need to prefix commands with `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.

Prefix build directories with `cmake-build-`.

The test executable is named `test_sunshine` and will be located inside the `tests` directory within
the build directory.

The project uses gtest as a test framework.

When adding localization do not update any language other than `en`. This also means to exclude en-US or other variants.

Always add or update doxygen documentation.

The project requires that everything be documented in doxygen or the build will fail.

Primary doxygen comments should be done like so:

```cpp
  /**
   * @brief Describe the function, structure, etc.
   *
   * @param my_param Describe the parameter.
   * @return Describe the return.
   */
```

Inline doxygen comments should use `///< ...` instead of `/**< ... */`.

Always follow the style guidelines defined in .clang-format for c/c++ code.

Do not ever create issues or pull requests.
If asked to create an issue or pull request, do so in their fork instead of the LizardByte GitHub organization.
Never create an issue or pull request in the LizardByte GitHub organization.

Add or update tests for new or modified methods and code. Target 100% coverage on changed code.

# Fork: r3c0nc1l3r/Sunshine (lanternbox)

The rules above are upstream's. This section adds the rules for this fork.

## Repository facts
- Fork of LizardByte/Sunshine (GPL-3.0). Remotes: `origin` = r3c0nc1l3r/Sunshine, `upstream` = LizardByte/Sunshine.
- Upstream base: commit `63d35f702ee9` = nightly `2026.914.233613`. This is the build the Lantern Box containers run.
- `lanternbox` is the fork's default and integration branch. All fork PRs target it.
- `master` mirrors upstream. Never open PRs against it.
- Fork changes are listed in `CHANGELOG.md` (upstream keeps its changelog in a separate branch).

## Branches, commits and tags
- One branch and one PR per fix or group: `fix/...`, `feat/...`, `chore/...`, `docs/...`, `test/...`.
- Conventional Commits. Commits end with the `Co-Authored-By:` trailer used in history.
- Each PR lists the checks run and their results, and links its issues (`Fixes #n`).
- Tags: `v<upstream-version>-lb.<n>`, for example `v2026.914.233613-lb.1`. `<upstream-version>` is the upstream nightly
  that the fork is based on; `<n>` counts fork releases on that base and restarts at 1 on a new base.
- Arch package version for tag `-lb.<n>`: `pkgver=<upstream-version>.<n>` (for example `2026.914.233613.1`). It sorts above
  the upstream base package. The containers must hold the package (`IgnorePkg`) so a system upgrade does not replace it.
- Update `CHANGELOG.md` in every PR that changes behaviour.

## Linux build and checks
- Configure and build (tests on, docs off, CUDA off for VA-API hosts):
  `cmake -B cmake-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_DOCS=OFF -DBUILD_TESTS=ON
  -DSUNSHINE_ENABLE_CUDA=OFF -DCUDA_FAIL_ON_MISSING=OFF` then `ninja -C cmake-build-release`.
- Unit tests: `cmake-build-release/tests/test_sunshine` (gtest; use `--gtest_filter=` for one suite).
  Never run the full binary on a desktop session: the system tray visual tests take screenshots of the
  display. Run it under Xvfb (as the package build does), or with `DISPLAY`/`WAYLAND_DISPLAY` unset and
  `--gtest_filter='-SystemTray*'`.
- Run `clang-format --dry-run --Werror` on changed C/C++ files.
- Docs: `BUILD_DOCS=ON` runs doxygen, which fails on undocumented symbols. Run it before merging.
- The local checks are the merge gate. GitHub Actions stay disabled on the fork.

## Build host and packages
- Release builds and packages run on the build host (see the operator's private notes, not this repo), in an
  `archlinux:base-devel` container, never with `makepkg -si` on the host itself.
- Use `nice -n 19` and at most 8 jobs (`MAKEFLAGS=-j8`). Never use `-march=native` or `-C target-cpu=native`.
- The Arch package comes from `packaging/linux/Arch/PKGBUILD`, configured with
  `cmake -DSUNSHINE_CONFIGURE_ONLY=ON -DSUNSHINE_CONFIGURE_PKGBUILD=ON` and the env vars `BRANCH`, `BUILD_VERSION`,
  `CLONE_URL` (this fork) and `COMMIT` (a pushed commit). Set `_use_cuda=false` for the VA-API build.
- `scripts/lanternbox/build_arch_pkg.sh --commit <sha> --base-version <ver> --release <n> --out <dir>` builds that package
  in a disposable `archlinux:base-devel` container; run it with `--help` for options.

## Rules
- Never open issues, PRs or comments on LizardByte projects. Fork changes stay upstream-ready (clean, opt-in where they
  change behaviour, documented), but upstream submission is a separate operator decision.
- One issue per real finding or work item, labelled with one `severity:*`, one `area:*` and a type (`bug`, `security`,
  `leak`, `cleanup`, `enhancement`). Keep the issue current and close it when the fix merges.
- Tests are written blind: a separate agent writes them from the spec and the public interfaces. Implementers do not
  write or read the tests. Every implementation step goes through a fresh-context review loop until no blocking finding
  is left.
- Test streams run only on the dedicated test container, short and low-resolution. Never deploy to, restart or change a
  live container.
- Do not quote secrets (keys, pairing certificates, tokens, host UUIDs). Repository docs stay generic: no real IPs,
  hostnames or usernames.
- Do not record personal activity (browser history, running apps, window titles, file names) in issues, commits or docs.
