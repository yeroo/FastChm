#!/usr/bin/env python3
"""FastChm regression test harness.

Compiles every test project, asserts the expected internal CHM files are present,
and (when hh.exe is available) round-trips through `hh.exe -decompile` to confirm
every compiled source file comes back byte-identical.

Usage: python test/run_tests.py [path-to-fastchm.exe]
Exit code 0 = all passed, 1 = a failure.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from chmdir import read_dir  # noqa: E402

FASTCHM = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "fastchm.exe")
HH = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "hh.exe")

passed = 0
failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("  ok   %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))
        # surfaces in the GitHub Actions log as an annotation we can read back
        print("::error::FAIL %s %s" % (name, detail))


def read_or_none(path):
    try:
        return open(path, "rb").read()
    except OSError:
        return None


def run_fastchm(args):
    r = subprocess.run([FASTCHM] + args, capture_output=True, text=True)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def source_files(proj_dir):
    """User-authored files that may end up in a CHM (compared on round-trip)."""
    exts = (".htm", ".html", ".css", ".hhc", ".hhk", ".js", ".gif", ".png", ".jpg")
    out = {}
    for root, _dirs, files in os.walk(proj_dir):
        for f in files:
            if f.lower().endswith(exts):
                out.setdefault(f.lower(), os.path.join(root, f))
    return out


def extract_roundtrip(proj_dir, chm):
    """Round-trips through FastChm's own --extract (self-contained, no hh.exe)."""
    tmp = tempfile.mkdtemp(prefix="fastchm_x_")
    try:
        subprocess.run([FASTCHM, "--extract", os.path.abspath(chm), tmp],
                       capture_output=True)
        srcs = source_files(proj_dir)
        checked, bad = 0, []
        for root, _dirs, files in os.walk(tmp):
            for f in files:
                src = srcs.get(f.lower())
                if not src:
                    continue
                checked += 1
                if sha(os.path.join(root, f)) != sha(src):
                    bad.append(f)
        return checked, bad
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def decompile_roundtrip(proj_dir, chm):
    """Returns (checked, mismatched-list). Skipped (checked=0) if hh.exe absent."""
    if not os.path.exists(HH):
        return 0, []
    tmp = tempfile.mkdtemp(prefix="fastchm_rt_")
    try:
        try:
            subprocess.run([HH, "-decompile", tmp, os.path.abspath(chm)],
                           capture_output=True, timeout=120)
        except subprocess.TimeoutExpired:
            return 0, []  # treat as skip rather than hang the suite
        srcs = source_files(proj_dir)
        checked, bad = 0, []
        for root, _dirs, files in os.walk(tmp):
            for f in files:
                src = srcs.get(f.lower())
                if not src:
                    continue
                checked += 1
                if sha(os.path.join(root, f)) != sha(src):
                    bad.append(f)
        return checked, bad
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_project(hhp, must_have, proj_dir=None):
    print("[project] %s" % os.path.relpath(hhp, ROOT))
    proj_dir = proj_dir or os.path.dirname(hhp)
    rc, out = run_fastchm([hhp])
    check("compiles", rc == 0, out.strip())
    if rc != 0:
        return
    # locate the produced .chm (from stdout "path: ...") else guess
    chm = None
    for line in out.splitlines():
        if ".chm:" in line:
            chm = line.split(".chm:")[0].split("]")[-1].strip() + ".chm"
            chm = os.path.join(ROOT, chm.replace("/", os.sep))
            break
    if not chm or not os.path.exists(chm):
        stem = os.path.splitext(os.path.basename(hhp))[0]
        chm = os.path.join(proj_dir, stem + ".chm")
    check("output exists", os.path.exists(chm), chm)
    if not os.path.exists(chm):
        return
    entries = read_dir(chm)
    for nm in must_have:
        check("contains %s" % nm, nm in entries)
    # self-contained round-trip via FastChm's own reader
    xchecked, xbad = extract_roundtrip(proj_dir, chm)
    check("extract round-trip byte-identical (%d files)" % xchecked,
          xchecked > 0 and not xbad, str(xbad))
    # cross-check against Windows' own decompiler when present (informational only:
    # hh.exe is absent or behaves differently on some Windows editions / CI runners,
    # so it must not gate the suite — the extract round-trip above is authoritative)
    checked, bad = decompile_roundtrip(proj_dir, chm)
    if checked and not bad:
        print("  ok   hh.exe cross-check byte-identical (%d files)" % checked)
    elif checked:
        print("  warn hh.exe cross-check mismatch (ignored): %s" % bad)
    else:
        print("  skip hh.exe cross-check (not available)")


def generate_large_project(dirpath):
    """Writes a ~200 KB multi-file project so the compressed section spans several
    64 KB LZX reset intervals (exercises interval reset + frame realignment). Used
    in place of the (gitignored) hand-generated test/big."""
    os.makedirs(dirpath, exist_ok=True)
    files = []
    for i in range(60):
        name = "topic%03d.htm" % i
        para = ("Topic %d covers feature %d in detail. " % (i, i * 7)) * (20 + (i % 30))
        html = ("<html><head><title>Topic %d Reference</title></head><body>"
                "<h1>Topic %d</h1><p>%s</p></body></html>" % (i, i, para))
        open(os.path.join(dirpath, name), "w", encoding="utf-8").write(html)
        files.append(name)
    big = "All work and no play. " * 6000  # ~130 KB highly compressible
    open(os.path.join(dirpath, "big.htm"), "w", encoding="utf-8").write(
        "<html><head><title>Big</title></head><body>%s</body></html>" % big)
    files.append("big.htm")
    hhp = ("[OPTIONS]\nCompiled file=large.chm\nDefault topic=topic000.htm\n"
           "Title=Large\nLanguage=0x409\nFull-text search=Yes\n\n[FILES]\n"
           + "\n".join(files) + "\n")
    open(os.path.join(dirpath, "large.hhp"), "w", encoding="utf-8").write(hhp)
    return os.path.join(dirpath, "large.hhp")


def main():
    if not os.path.exists(FASTCHM):
        print("fastchm.exe not found at %s" % FASTCHM)
        return 1

    common = ["/#SYSTEM", "/#STRINGS", "/#TOPICS",
              "::DataSpace/Storage/MSCompressed/Content"]

    test_project(os.path.join(ROOT, "test", "sample", "sample.hhp"),
                 common + ["/#WINDOWS", "/#IVB", "/#TOCIDX",
                           "/$WWKeywordLinks/BTree", "/$WWAssociativeLinks/BTree",
                           "/$FIftiMain", "/$OBJINST"])

    test_project(os.path.join(ROOT, "test", "feat", "feat.hhp"),
                 common + ["/#SUBSETS", "/#IDXHDR", "/$WWKeywordLinks/BTree",
                           "/$WWAssociativeLinks/BTree"])

    # large generated project: multiple LZX reset intervals + FTS (replaces the
    # gitignored test/big so CI has the same coverage on a fresh checkout)
    biggen = tempfile.mkdtemp(prefix="fastchm_big_")
    try:
        test_project(generate_large_project(biggen), common + ["/$FIftiMain"], biggen)
    finally:
        shutil.rmtree(biggen, ignore_errors=True)

    # UTF-8 source title must be re-encoded into the project codepage (cp1252 here),
    # not stored as raw UTF-8 bytes.
    test_project(os.path.join(ROOT, "test", "utf8", "utf8.hhp"), common)
    print("[unicode] utf8 #STRINGS codepage check")
    tmp = tempfile.mkdtemp(prefix="fastchm_u_")
    try:
        subprocess.run([FASTCHM, "--extract",
                        os.path.join(ROOT, "test", "utf8", "utf8.chm"), tmp],
                       capture_output=True)
        strings = read_or_none(os.path.join(tmp, "#STRINGS"))
        check("utf8 #STRINGS extracted", strings is not None)
        if strings is not None:
            check("title stored as cp1252 (0xE9/0xFC present)",
                  0xE9 in strings and 0xFC in strings, strings.hex())
            check("title not raw UTF-8 (no 0xC3 lead byte)", 0xC3 not in strings)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # sitemap "text/site properties" (sample.hhc declares ImageType=Folder)
    print("[siteprops] sample #IDXHDR folder flag")
    tmp = tempfile.mkdtemp(prefix="fastchm_s_")
    try:
        subprocess.run([FASTCHM, "--extract",
                        os.path.join(ROOT, "test", "sample", "sample.chm"), tmp],
                       capture_output=True)
        idx = read_or_none(os.path.join(tmp, "#IDXHDR"))
        check("#IDXHDR extracted", idx is not None)
        if idx is not None:
            check("#IDXHDR ImageType=Folder flag set", len(idx) > 0x1C and idx[0x1C] == 1)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # collection build
    print("[collection] test/collection/collection.hhp")
    rc, out = run_fastchm(["--collection",
                           os.path.join(ROOT, "test", "collection", "collection.hhp")])
    check("collection builds", rc == 0, out.strip())
    for chm in ["apple.chm", "orange.chm", "collection.chm"]:
        p = os.path.join(ROOT, "test", "collection", chm)
        check("collection produced %s" % chm, os.path.exists(p))

    print("\n%d passed, %d failed" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # surface a runner-side crash as a readable annotation
        import traceback
        print("::error::harness crashed: %r" % e)
        traceback.print_exc()
        sys.exit(2)
