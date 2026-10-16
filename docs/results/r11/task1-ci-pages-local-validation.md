# Task 1 -- Pages + CI: local validation (the achievable half)

The green badge lives on RUNNERS (GitHub Actions on the mirror, Forgejo on
Codeberg) and needs a push to trigger them plus the maintainer enabling Pages
in repo settings -- not reachable from an EC2 box. What IS reachable, and done
here, is running each workflow's actual work on kiwi (intel-lo@r11, Debian 13,
ruby 3.3.8 + bundler + doxygen + clang + cross-gcc) to catch failures before a
real runner sees them.

## Pages workflow (.github/workflows/pages.yml, .forgejo mirror)
- `gen_api_reference.sh`: rc 0.
- `bundle install` (just-the-docs 0.12.0, jekyll 4.4.1): rc 0 (one transient
  `listen` gem fetch failed; a --retry=3 + vendor/bundle path succeeded -- the
  real runner's ruby/setup-ruby@v1 bundler-cache avoids it).
- `JEKYLL_ENV=production bundle exec jekyll build`: **rc 0, 55 files, index.html
  present and contains "libumem".** Only SCSS deprecation warnings from the
  just-the-docs theme (cosmetic, not errors).
=> the Pages site BUILDS GREEN. First real deploy needs: push + GitHub Pages
   "Source: GitHub Actions" enabled + Codeberg Pages token. Those are repo-
   settings/maintainer steps, recorded as owed.

## CI workflow (.github/workflows/ci.yml) steps validated locally
- fuzz harness test/fuzz/fuzz_malloc.c: standalone replay compiles clean
  (`gcc -Wall -Wextra` rc 0). libFuzzer-entry build needs clang+libFuzzer (the
  CI job provides it); the harness is sound.
- clang-format 19.1.7 present; the format gate is changed-files-only.
- QEMU cross-build legs: validated separately in gate C (riscv64/arm32/aarch64
  compile+link+run) and the task-3 getpcstack work.
- The build-test matrix is autotools make check, already green on x86/arm
  (exit_criteria_gate gate_failures=0, v4.2.0 qualification).

## Honest status
Local validation of the workflow WORK: PASS (Pages site builds, fuzz compiles,
format tool present). The green CI/Pages BADGE is owed-on-push: it requires
triggering the GitHub/Codeberg runners and the one-time Pages repo setting,
which only the maintainer/push can do. No badge is claimed green that was not
observed green.
