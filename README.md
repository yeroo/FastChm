# FastChm

A fast, zero-dependency CHM (Microsoft HTML Help) **compiler** for Windows — a modern
C++ replacement for `hhc.exe` (HTML Help Workshop), in the spirit of Free Pascal's
`chmcmd`, but with no runtime or build dependencies beyond MSVC and the C++17
standard library.

```
fastchm <project.hhp> [-o output.chm]
```

## Features

- **`.hhp` projects**: `[OPTIONS]`, `[FILES]`, `[WINDOWS]`, `[ALIAS]`, `[MAP]`
  (including `#include`d alias/map header files).
- **Auto-inclusion** of files referenced only by HTML `href`/`src` links or by
  TOC/index `Local` entries — `[FILES]` can list just your entry page.
- **From-scratch LZX encoder** (64K window, reset interval 64K, verbatim +
  aligned-offset blocks), **multi-threaded**: each 64K reset interval is
  compressed independently across all cores; output is identical to serial.
- Full ITSF/ITSP container (PMGL/PMGI directory chunks, quickref areas) and the
  standard metadata files: `#SYSTEM`, `#TOPICS`, `#URLSTR`, `#URLTBL`, `#STRINGS`,
  `#ITBITS`, `::DataSpace` control files.
- **Window definitions** (`#WINDOWS`, 20-argument HHW syntax) and
  **context IDs** (`#IVB`, works with `HtmlHelp()`/`hh.exe -mapid`).
- **Binary TOC** (`#TOCIDX`) and **binary index** (`$WWKeywordLinks` BTree +
  Data/Map/Property, KLinks) when `Binary TOC=Yes` / `Binary Index=Yes`.
- **Full-text search**: `$FIftiMain` word index (scale/root bit coding,
  prefix-compressed word tree) plus the `$OBJINST` word-breaker blob, when
  `Full-text search=Yes`. Phrase queries work (word positions are indexed).

Output opens in `hh.exe` with working TOC/Index/Search panes and survives
`hh.exe -decompile` with byte-identical files (verified with a 402-file / 1.8 MB
project — compiled, with full-text indexing, in under 60 ms).

## Building

```
build.bat
```

Locates Visual Studio via `vswhere` and builds `fastchm.exe` with `cl /O2 /W4`.

## Testing

```
fastchm test\sample\sample.hhp
hh.exe test\sample\sample.chm                     :: visual check
hh.exe -decompile test\decompiled test\sample\sample.chm
```

The sample project exercises auto-inclusion, window definitions, context IDs,
binary TOC/index and full-text search.

## Not yet implemented

- `[MERGE FILES]` (merged/collection help), `[SUBSETS]`, information types
- ALinks from `<object>` tags inside topic pages (`$WWAssociativeLinks` is
  emitted as an empty placeholder)
- CHM decompilation (use `hh.exe -decompile`)

## Format references

`refs/NOTES.md` condenses everything needed to maintain this: the ITSF container
layout, the LZX bitstream details, and the internal file formats. Derived from the
public unofficial specs (Russotto, chmspec) and cross-checked against Free Pascal's
chm package (used strictly as a format reference; all code here is original).
