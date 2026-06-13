# Changelog

All notable changes to FastChm. Versions follow the `#SYSTEM` compiler-id string
embedded in produced CHMs.

## 0.5

- Test harness (`test/run_tests.py`) that compiles every sample project, asserts the
  expected internal CHM files exist, and round-trips through both FastChm's own reader
  and `hh.exe -decompile` (50 checks).
- CMake build (`CMakeLists.txt`) with a `ctest` target; `build.bat` still works.
- GitHub Actions CI (Windows): configure, build, test on push/PR.
- `--version` / `-v` flag; version centralized in `src/version.h`.
- Removed the one MSVC-specific call (`fopen_s`) for portable C++17.
- `--list` / `--extract`: built-in LZX decoder + ITSF reader (read side / decompiler).
- Encoding correctness: codepage/LCID derived from `Language` (or `Charset`); UTF-8 /
  UTF-16 / codepage source files decoded and metadata re-encoded into the project
  codepage; DBCS flag set. Replaces the previously hardcoded 1252/1033.

## 0.4

- `--collection` mode: build a master CHM plus every child referenced in its
  `[MERGE FILES]`, into the master's folder, in one command.

## 0.3

- KLinks and ALinks harvested from `<object>` controls in topic pages
  (`$WWKeywordLinks` / `$WWAssociativeLinks` BTrees).
- `[MERGE FILES]` recorded in `#IDXHDR`; `[SUBSETS]` → `#SUBSETS`;
  `[INFOTYPES]` → `#SYSTEM` info-type count.

## 0.2

- Multi-threaded LZX compression (independent 64K reset intervals).
- Auto-inclusion of files referenced by HTML links and sitemap `Local` entries.
- `[ALIAS]`/`[MAP]` context IDs (`#IVB`); `[WINDOWS]` definitions (`#WINDOWS`).
- Binary TOC (`#TOCIDX`) and binary index (`$WWKeywordLinks`).
- Full-text search (`$FIftiMain` + `$OBJINST`).

## 0.1

- Initial release: ITSF/ITSP container writer, from-scratch LZX encoder, and the
  core internal files (`#SYSTEM`, `#TOPICS`, `#URLSTR`, `#URLTBL`, `#STRINGS`,
  `::DataSpace/*`). Compiles `.hhp` projects to `.chm`.
