# FastChm

A fast, zero-dependency CHM (Microsoft HTML Help) **compiler** for Windows — a modern
C++ replacement for `hhc.exe` (HTML Help Workshop), in the spirit of Free Pascal's
`chmcmd`, but with no runtime or build dependencies beyond MSVC and the C++17
standard library.

```
fastchm <project.hhp> [-o output.chm]
fastchm --collection <master.hhp>      # build a master + its [MERGE FILES] children
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
- **KLinks and ALinks from `<object>` controls** embedded in topic pages
  (`param name="Keyword"` / `"ALink Name"`) — real `$WWKeywordLinks` and
  `$WWAssociativeLinks` BTrees; object keywords merge into the binary index.
- **Collections**: `[MERGE FILES]` (recorded in `#IDXHDR`), `[SUBSETS]`
  (`#SUBSETS`), and `[INFOTYPES]` (`#SYSTEM` info-type count). `--collection`
  compiles a master plus every child CHM it references, in one command.
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
binary TOC/index, full-text search and ALink/KLink object controls.
`test\feat\feat.hhp` additionally covers `[MERGE FILES]`, `[SUBSETS]` and
`[INFOTYPES]`, and `test\collection\` is a two-child merged collection built with
`fastchm --collection test\collection\collection.hhp`.

## Collections (merged help)

A collection is a *master* CHM that merges the tables of contents, indexes and
full-text search of several *child* CHMs at runtime. To build one:

1. Author each child as a normal project (its own `.hhp`/`.hhc`/`.hhk`).
2. In the master `.hhp`, list the children under `[MERGE FILES]`, and in the
   master `.hhc` reference each child's contents with a `Merge` param, ideally
   wrapped in a named book entry:

   ```html
   <LI> <OBJECT type="text/sitemap"><param name="Name" value="Apples"></OBJECT>
   <UL>
     <LI> <OBJECT type="text/sitemap">
          <param name="Name" value="Apples">
          <param name="Merge" value="apple.chm::/apple.hhc">
          </OBJECT>
   </UL>
   ```
3. Build everything in one step:

   ```
   fastchm --collection master.hhp
   ```

   This compiles the master and every `child.chm` named in `[MERGE FILES]`
   (from `child.hhp` or `child/child.hhp` next to the master), writing all CHMs
   into the master's folder. A child that has no `.hhp` is left as a prebuilt
   `.chm`.

Ship all the CHMs in the same directory. `hh.exe` performs the merge when the
master is opened and caches the combined index/search in a `<master>.chw` file
(so the folder must be writable at first run) — the compiler does not produce the
`.chw`. See `test/collection/` for a worked two-child example.

> Note: `hh.exe`'s runtime merge labels the *first* merged sub-tree "untitled"
> regardless of the authored name — a long-standing HTML Help quirk, not a
> property of the compiled files (each child shows its real name standalone and
> in any non-first position). Wrapping merges in named book entries, as above,
> keeps the section headings correct.

## Not yet implemented

- `#SUBSETS`/`#INFOTYPES` are emitted structurally; the HH Workshop info-type
  authoring semantics (per-topic type membership) are not modelled
- CHM decompilation (use `hh.exe -decompile`)

## Format references

`refs/NOTES.md` condenses everything needed to maintain this: the ITSF container
layout, the LZX bitstream details, and the internal file formats. Derived from the
public unofficial specs (Russotto, chmspec) and cross-checked against Free Pascal's
chm package (used strictly as a format reference; all code here is original).
