# studio

This branch is `main` (a mirror of `upstream/main`) plus what is for this fork's owner and not for upstream. Nothing in it
changes a file upstream has, so merging `upstream/main` into it should not conflict.

- Branches: `main` mirrors `upstream/main` (fast-forward only, never committed to). `feature/host-clock` is the source of a
  pull request. `studio` is `main` plus `feature/host-clock` plus the files below. Merge, do not rebase: it is published.
- `measure/`: the measurement core `clkn` (a sound for every MIDI clock message it is handed, to read what a DAW gives
  a core and when, from the audio), its build script and its self test. See the head of `clkn_core.c`.
  `measure/build_clkn.sh selftest` runs on any host; `... install` (macOS) puts `clkn.fm1core` in the user cores
  folder. The measurements' notes and scripts are in the separate `fm1-remote` repository.
