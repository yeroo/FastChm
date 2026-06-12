# FastChm

A fast, zero-dependency CHM (Microsoft HTML Help) **compiler** for Windows — a modern
C++ replacement for `hhc.exe` (HTML Help Workshop), in the spirit of Free Pascal's
`chmcmd`, but with no runtime or build dependencies beyond MSVC and the C++17
standard library.

```
fastchm <project.hhp> [-o output.chm]
```

## What it does

- Parses the `.hhp` project (`[OPTIONS]`, `[FILES]`), auto-includes the `.hhc`/`.hhk`.
- Compresses all content with a from-scratch **LZX encoder** (64K window, reset
  interval 64K, verbatim + aligned-offset blocks, greedy hash-chain matching) —
  the same parameters `hhc.exe` uses.
- Writes the full ITSF/ITSP container (PMGL/PMGI directory chunks, quickref areas)
  and the internal metadata files: `#SYSTEM`, `#TOPICS`, `#URLSTR`, `#URLTBL`,
  `#STRINGS`, `#ITBITS`, and all `::DataSpace` control files (ControlData,
  SpanInfo, ResetTable, NameList, Transform/List).

Output opens in `hh.exe` with working TOC/Index panes and survives
`hh.exe -decompile` with byte-identical files (verified with a 402-file / 1.6 MB
project, compiled in ~40 ms).

## Building

```
build.bat
```

Locates Visual Studio via `vswhere` and builds `fastchm.exe` with `cl /O2 /W4`.
(A cosmetic `vswhere.exe is not recognized` line may appear; the build still works.)

## Testing

```
fastchm test\sample\sample.hhp
hh.exe test\sample\sample.chm                     :: visual check
hh.exe -decompile test\decompiled test\sample\sample.chm
```

## Not yet implemented

- `[WINDOWS]` definitions (`#WINDOWS` file), `[ALIAS]`/`[MAP]` context IDs (`#IVB`)
- Full-text search (`$FIftiMain`), binary TOC/index, KLink/ALink trees
- Auto-inclusion of files referenced only by links/TOC entries
- Multi-threaded compression (reset intervals are independent — easy win later)

## Format references

`refs/NOTES.md` condenses everything needed to maintain this: the ITSF container
layout, the LZX bitstream details, and the internal file formats. Derived from the
public unofficial specs (Russotto, chmspec) and cross-checked against Free Pascal's
chm package (used strictly as a format reference; all code here is original).
