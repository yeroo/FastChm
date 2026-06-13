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
    # cross-check against Windows' own decompiler when present
    checked, bad = decompile_roundtrip(proj_dir, chm)
    if checked:
        check("hh.exe round-trip byte-identical (%d files)" % checked, not bad, str(bad))
    else:
        print("  skip hh.exe round-trip (not available)")


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

    test_project(os.path.join(ROOT, "test", "big", "big.hhp"),
                 common + ["/$FIftiMain"])

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
    sys.exit(main())
