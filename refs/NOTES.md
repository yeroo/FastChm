# CHM format notes (condensed from refs/, for FastChm implementation)

Sources: russotto chmformat.html (container+LZX control), nongnu chmspec Internal.html
(system files), FPC chmwriter.pas / paslzxcomp.pas (working writer, LGPL — used as
format reference only; FastChm code is written from scratch).

All values little-endian unless noted.

## Container (ITSF v3)

```
0x00 'ITSF', DWORD ver=3, DWORD headerLen=0x60, DWORD 1, DWORD timestamp(BE), DWORD langID
0x18 GUID {7C01FD10-7BAA-11D0-9E0C-00A0C922E6EC}
0x28 GUID {7C01FD11-7BAA-11D0-9E0C-00A0C922E6EC}
0x38 QWORD off(hs0)=0x60, QWORD len(hs0)=0x18
0x48 QWORD off(hs1)=0x78, QWORD len(hs1)=0x54+0x1000*nChunks
0x58 QWORD contentOffset (= 0x78 + len(hs1))
```
Header section 0 (0x18 bytes): DWORD 0x01FE, DWORD 0, QWORD totalFileSize, DWORD 0, DWORD 0.
Header section 1: ITSP header then directory chunks.
```
'ITSP', ver=1, headerLen=0x54, 0x0A, chunkSize=0x1000, quickRefDensity=2,
indexTreeDepth (1 = no PMGI, 2 = one PMGI level...), rootIndexChunk (-1 if none),
firstPMGL, lastPMGL, -1, totalChunks, langID,
GUID {5D02926A-212E-11D0-9DF9-00A0C922E6EC}, 0x54, -1, -1, -1
```
PMGL chunk: 'PMGL', DWORD freeSpace(quickref+free bytes at end), DWORD 0, DWORD prevChunk(-1), DWORD nextChunk(-1); entries; quickref grows backward from end:
[end-2]=WORD numEntries, [end-4]=offset(entry 5 from entry 0), [end-6]=offset(entry 10)... (density 2 → n=5).
Entry: ENCINT nameLen, name(UTF-8), ENCINT section(0/1), ENCINT offset, ENCINT length.
Sorted by name, case-insensitive. Offsets are into the *uncompressed* section.
PMGI chunk: 'PMGI', DWORD freeSpace; entries {ENCINT nameLen, name, ENCINT chunkNum}; same quickref.
ENCINT: 7 bits per byte, MSB-first groups, high bit = continue.

## Section files (all in section 0, uncompressed)

- `::DataSpace/NameList`: WORD totalLenInWords, WORD count=2, then per name
  {WORD lenInChars, UTF-16 chars, WORD 0}: "Uncompressed", "MSCompressed".
- `::DataSpace/Storage/MSCompressed/ControlData` (0x20 bytes):
  DWORDs [6]['LZXC'][version=2][resetInterval=2][windowSize=2][cacheSize=1][0][0]
  (interval/window/cache in 0x8000 units when version=2 → 64K window, 64K reset).
- `::DataSpace/Storage/MSCompressed/SpanInfo`: QWORD uncompressed size (real, unpadded).
- `::DataSpace/Storage/MSCompressed/Transform/List`: 38 bytes = first 19 chars of
  "{7FC28940-9D31-11D0-9B27-00A0C91E9C7C}" as UTF-16 (MS bug, replicate exactly).
- `::DataSpace/Storage/MSCompressed/Transform/{7FC28940-9D31-11D0-9B27-00A0C91E9C7C}/InstanceData/ResetTable`:
  DWORD 2, DWORD nEntries, DWORD 8, DWORD 0x28, QWORD uncompLen(real),
  QWORD compLen, QWORD 0x8000, then QWORD compressed offset of each 0x8000-frame start
  (entry 0 = 0). nEntries = number of frames.
- `::DataSpace/Storage/MSCompressed/Content`: the LZX data; its directory entry is
  section 0, offset = end of other section-0 data; raw compressed bytes are written there.

## LZX (window 2^16, reset every 0x10000 input bytes)

Bitstream: 16-bit little-endian words; bits filled MSB-first within each word.
Frame = 0x8000 uncompressed bytes. At every frame boundary: pad bitstream to 16-bit
boundary, record compressed offset in reset table. Input is zero-padded to a frame
multiple; the pad bytes are compressed but excluded from SpanInfo/ResetTable uncomp size.
Matches must NOT cross frame boundaries. Full encoder state reset (Huffman prev lengths,
R0=R1=R2=1, LZ window) every reset interval (64K) → each 64K interval is independent.

Per reset: write 1 bit E8-translation flag (we write 0).
Block: 3 bits type (1=verbatim, 2=aligned, 3=uncompressed), 24 bits uncompressed size.
Aligned block: 8 x 3-bit aligned-tree code lengths follow the header.
Then: main tree part 1 (256 lengths), part 2 (8*nSlots lengths), length tree (249),
each pretree-encoded; then token stream.

Tree encoding: 20 x 4-bit pretree lengths, then pretree-coded values:
0..16 = (prevLen[i] - newLen + 17) % 17 delta; 17 = zero-run 4..19 (+4-bit excess);
18 = zero-run 20..51 (+5-bit excess); 19 = repeat-run 4..5 (+1-bit excess) followed by
one pretree-coded delta value. prevLen = lengths of same tree in previous block of this
interval (zeros after reset).

Tokens: literal = main symbol < 256. Match: lenHeader=min(len-2,7);
mainSym = 256 + (slot<<3 | lenHeader); if lenHeader==7, lengthTree sym = len-9 (0..248).
formattedOffset = dist+2; R0/R1/R2 LRU → formatted 0/1/2 (R0: no change; R1: swap R0,R1;
R2: swap R0,R2; new: R2=R1,R1=R0,R0=dist). slot: largest s with posBase[s] <= formatted.
extraBits[s] = min(17, max(0,(s-2)>>1)); posBase[0]=0, posBase[s+1]=posBase[s]+(1<<extraBits[s]).
Footer = formatted - posBase[slot], written as extraBits raw bits (verbatim block) or
(extraBits-3) raw + aligned-tree(footer&7) when aligned block && extraBits>=3.
64K window → 32 position slots, main tree 256+256=512 symbols. Max match 257, min 2.
Match dist <= 0xFFFD (offsets wsize-2..wsize illegal). Code length limits: main/length 16,
pretree 15 (4-bit field), aligned 7.
Canonical codes: increasing code values, ordered by (codeLen asc, symbol asc).
Empty tree (all lengths 0) is legal. Single-symbol tree: give it and the lowest other
symbol length 1 (codes by symbol order).
Match heuristic (ref encoder): reject len<3; len<4 if fmtOff>=64; len<5 if fmtOff>=2048;
len<6 if fmtOff>=65536.

## System files

- `/#SYSTEM`: DWORD ver=3, then {WORD code, WORD len, bytes}. Write order
  10,9,4,2,3,16,(6),0,1,(5),(7),(11),(13).
  10=time_t DWORD; 9=compiler name string (NT); 4=36 bytes {DWORD lcid, DWORD dbcs,
  DWORD ftsOn, DWORD hasKLinks, DWORD hasALinks, QWORD FILETIME, DWORD 0, DWORD 0};
  2=default topic; 3=title; 16=default font; 0=hhc name; 1=hhk name; 5=default window;
  7/11=DWORD (binary index/toc on); 13=#IDXHDR copy. All strings NUL-terminated.
- `/#ITBITS`: empty file (size 0).
- `/#STRINGS`: byte 0 first; NUL-terminated strings; must not cross 0x1000 block
  boundaries (zero-pad block and restart string in next block). Offset = string id.
- `/#TOPICS`: 16 bytes/entry: DWORD tocOffset(0), DWORD #STRINGS offset of title (-1 none),
  DWORD #URLTBL offset, WORD inContents (6=in contents w/ title, 2=not, 0 if '#' in url),
  WORD 0. Entries for each .htm file in order added; then hhc, then hhk (code 2, no title).
- `/#URLSTR`: 0x4000-byte blocks, first byte of each block = 0; entries
  {DWORD 0, DWORD 0, url string NT} not crossing block boundary (zero-pad).
  URL = path without leading '/'.
- `/#URLTBL`: 12-byte entries {DWORD 0, DWORD topicIndex, DWORD urlstrOffset};
  at block offset 0xFFC write a pad DWORD 0 (341 entries per 0x1000 block).
- `/#IVB` (context ids, [MAP]+[ALIAS]): DWORD totalSize, then {DWORD id, DWORD stringsOffsetOfFile}.
- `/#WINDOWS` (optional): DWORD count, DWORD entrySize=196, then 196-byte entries
  (see chmwriter.pas WriteWindows for field layout; string fields are #STRINGS offsets).

Compressed-section layout (in order): user content files (incl. hhc/hhk), #TOPICS,
#URLSTR, #URLTBL, (#WINDOWS), #STRINGS. Section 0: #ITBITS, #SYSTEM, ::DataSpace files.
hh.exe reads the *text* .hhc/.hhk named by #SYSTEM codes 0/1 when no binary TOC/index.
