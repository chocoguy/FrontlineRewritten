#!/usr/bin/env python3
"""Build the native game library and turn its errors into a work list.

Compiles frontline_game with the `ninja` preset, keeps going past failures,
collapses errors that repeat because a header is included by many units, sorts
them into buckets that match ROADMAP.md, and writes docs/port/build-errors.md.

    python3 tools/build_report.py              # configure, build, write the report
    python3 tools/build_report.py --log FILE   # report from an existing build log
    python3 tools/build_report.py --sizes      # list structs whose native layout differs

Rerun it after each portability fix; the diff of the report is the progress.
"""

import argparse
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DECOMP = ROOT / "FrontlineDecompiled"
BUILD = ROOT / "build" / "ninja"
REPORT = ROOT / "docs" / "port" / "build-errors.md"

DIAG = re.compile(r"^(/[^:]+):(\d+):(\d+): (?:fatal )?error: (.*)$")
FAILED = re.compile(r"^FAILED: .*?(\S+?)\.o\b")

# Buckets, in the order they're tried. Each is (key, title, roadmap ref, test),
# where test(message, source_line_is_in_asm) -> bool.
LIBC = r"\b(stricmp|strnicmp|_FILE|FILE|fopen|fclose|fread|fwrite|fseek|ftell|random|__sys_\w+|va_start|va_end)\b"
BUCKETS = [
    ("asm", "PowerPC assembly", "Phase 1.1: assembly → C",
     lambda m, asm: asm or re.search(r"\basm\b|nofralloc|__declspec", m)),
    ("layout", "Struct layout asserts", "Gotchas 1–2: ptr32<T>, u32",
     # SIZE_ASSERT/OFFSET_ASSERT from types.h, plus a few hand-rolled ones.
     lambda m, asm: "declared as an array with a negative size" in m),
    ("ptrcast", "Pointer truncated to 32 bits", "Gotcha 1: uptr / PTR_TO_ADDR / PTR32",
     lambda m, asm: "cast from pointer to smaller type" in m),
    ("intptr", "Pointer rebuilt from a 32-bit integer", "Gotcha 1: ADDR_TO_PTR / PTR32",
     lambda m, asm: "from smaller integer type" in m),
    ("libc", "MSL vs. Apple C library", "Phase 1.2: platform layer",
     lambda m, asm: re.search(LIBC, m)),
    ("strict", "Stricter C++ in clang", "Gotcha 7: compiler differences",
     lambda m, asm: re.search(r"should return a value|does not return a value|narrowed|cannot initialize a variable of type 'char \*'"
                              r"|explicit specialization .* after instantiation|no matching function", m)),
    ("other", "Other", "triage", lambda m, asm: True),
]


def in_asm_function(path: Path, line: int, cache: dict) -> bool:
    """True if `line` is inside a CodeWarrior `asm` function or block."""
    if path not in cache:
        try:
            cache[path] = path.read_text(errors="replace").splitlines()
        except OSError:
            cache[path] = []
    lines = cache[path]
    for text in reversed(lines[: line]):
        if re.search(r"\basm\b", text):
            return True
        if text.startswith("}"):
            return False
    return False


def build(log: Path) -> None:
    subprocess.run(["cmake", "--preset", "ninja", "-DFRONTLINE_BUILD_APP=OFF"], cwd=ROOT,
                   check=True, stdout=subprocess.DEVNULL)
    with log.open("w") as f:
        # Failing is the expected outcome; -k 0 keeps every unit going.
        subprocess.run(["cmake", "--build", str(BUILD), "--target", "frontline_game", "--", "-k", "0"],
                       cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)


def rel(path: str) -> str:
    p = Path(path).resolve()
    for base in (DECOMP, ROOT):
        try:
            return str(p.relative_to(base))
        except ValueError:
            pass
    return path


# Symbols the platform layer must provide, by GameCube SDK library. Anything
# else the game references (the C library, the C++ runtime) links as is.
SDK_LIBS = [
    ("gx", "GX: graphics (renderer)", r"^(GX|__GX)"),
    ("os", "OS: threads, heaps, alarms, messages, time", r"^(OS|__OS)"),
    ("dvd", "DVD: disc reads", r"^DVD"),
    ("vi", "VI: video timing", r"^VI"),
    ("pad", "PAD/SI: controllers", r"^(PAD|SI)"),
    ("card", "CARD: memory card saves", r"^CARD"),
    ("audio", "AI/AR/ARQ/AX/DSP: audio and audio RAM", r"^(AI|AR|ARQ|AX|DSP)[A-Z]"),
    ("cache", "DC/PPC: caches and CPU", r"^(DC|IC|PPC|LC)"),
    ("mtx", "MTX: matrix library", r"^(PSMTX|C_MTX|PSVEC|C_VEC|MTX)"),
    ("hooks", "Native hooks (native/address.h, native/hooks.h)", r"^(Native|g_native)"),
    ("excluded", "Units left out of the native build (gen_sources.py)", r"^(LoadElf::|DEBUG_)"),
]


def undefined_symbols(lib: Path) -> dict:
    """Map each SDK library key to the symbols the game library needs from it."""
    def nm(args):
        r = subprocess.run(["nm", "-g", *args, str(lib)], capture_output=True, text=True)
        return {line.split()[-1] for line in r.stdout.splitlines() if line.strip() and not line.endswith(":")}
    undefined = nm(["-u"]) - nm(["-U"])
    names = subprocess.run(["xcrun", "c++filt"], input="\n".join(sorted(undefined)), capture_output=True,
                           text=True).stdout.split("\n")
    out = defaultdict(list)
    for name in names:
        name = name.strip()
        if name.startswith("_") and not name.startswith("__Z"):
            name = name[1:]
        for key, _, pattern in SDK_LIBS:
            if re.match(pattern, name):
                out[key].append(name)
                break
    return out


def unit_count() -> int:
    text = (BUILD / "game" / "GameSources.cmake").read_text()
    block = text.split("set(FRONTLINE_GAME_SOURCES", 1)[1].split(")", 1)[0]
    return sum(1 for line in block.splitlines() if line.strip())


AUDIT = ROOT / "build" / "audit"


def size_audit() -> None:
    """Build with FRONTLINE_AUDIT_SIZES, which turns the GameCube-only
    SIZE_ASSERT/OFFSET_ASSERT checks on natively, and list the structs that fail:
    their native layout differs, so each needs checking for file data, fixed
    allocation sizes (GC_SIZE) and raw offsets."""
    flags = "-DFRONTLINE_AUDIT_SIZES"
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(AUDIT), "-G", "Ninja", "-DFRONTLINE_BUILD_APP=OFF",
                    f"-DCMAKE_C_FLAGS={flags}", f"-DCMAKE_CXX_FLAGS={flags}"], check=True, stdout=subprocess.DEVNULL)
    r = subprocess.run(["cmake", "--build", str(AUDIT), "--target", "frontline_game", "--", "-k", "0"],
                       capture_output=True, text=True)
    structs = defaultdict(set)
    for raw in (r.stdout + r.stderr).splitlines():
        m = DIAG.match(raw)
        if not m or "negative size" not in m.group(4):
            continue
        line = Path(m.group(1)).read_text(errors="replace").splitlines()[int(m.group(2)) - 1]
        name = re.search(r"(?:SIZE|OFFSET)_ASSERT\(\s*([\w:]+)", line)
        structs[name.group(1) if name else line.strip()].add(rel(m.group(1)))
    for name in sorted(structs):
        print(f"{name:40} {', '.join(sorted(structs[name]))}")
    print(f"{len(structs)} structs differ natively")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--log", type=Path, help="use this build log instead of building")
    parser.add_argument("--sizes", action="store_true", help="list the structs whose native layout differs")
    args = parser.parse_args()
    if args.sizes:
        size_audit()
        return

    log = args.log or BUILD / "build_report.log"
    if not args.log:
        build(log)

    errors = {}  # (file, line, col, message) -> bucket key
    failed = set()
    cache = {}
    for raw in log.read_text(errors="replace").splitlines():
        if m := FAILED.match(raw):
            failed.add(m.group(1))
        m = DIAG.match(raw)
        if not m:
            continue
        path, line, col, msg = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
        key = (rel(path), line, col, msg)
        if key in errors:
            continue
        asm = in_asm_function(Path(path), line, cache)
        errors[key] = next(k for k, _, _, test in BUCKETS if test(msg, asm))

    by_bucket = defaultdict(list)
    for key, bucket in errors.items():
        by_bucket[bucket].append(key)

    total = unit_count()
    out = [
        "# Native build errors",
        "",
        "Generated by `tools/build_report.py`; don't edit by hand. Rerun it after a fix and",
        "commit the result, so the diff shows the progress.",
        "",
        f"- **Units:** {total - len(failed)} of {total} compile ({len(failed)} fail)",
        f"- **Errors:** {len(errors)} unique (an error in a header counts once, however many"
        " units include it)",
        "",
        "Compile errors only, so this undercounts the work. Link errors (the ~200 GameCube",
        "system-library functions the platform layer has to provide) come after the units",
        "compile, and some 64-bit problems compile cleanly and only fail at run time (see",
        "`docs/architecture/`).",
        "",
        "| Bucket | Errors | Files | Where the fix is planned |",
        "|---|---:|---:|---|",
    ]
    for key, title, ref, _ in BUCKETS:
        items = by_bucket.get(key, [])
        if items:
            files = len({i[0] for i in items})
            out.append(f"| [{title}](#{key}) | {len(items)} | {files} | {ref} |")
    out.append("")

    for key, title, ref, _ in BUCKETS:
        items = sorted(by_bucket.get(key, []))
        if not items:
            continue
        out += [f'<a id="{key}"></a>', "", f"## {title}", "", f"{ref}.", ""]
        per_file = defaultdict(list)
        for f, line, col, msg in items:
            per_file[f].append((line, msg))
        out += ["| File | Errors | First |", "|---|---:|---|"]
        for f, hits in sorted(per_file.items(), key=lambda kv: (-len(kv[1]), kv[0])):
            line, msg = hits[0]
            msg = msg.replace("|", "\\|")
            if len(msg) > 90:
                msg = msg[:87] + "..."
            out.append(f"| `{f}:{line}` | {len(hits)} | {msg} |")
        out.append("")

    lib = BUILD / "game" / "libfrontline_game.a"
    if not failed and lib.exists():
        needed = undefined_symbols(lib)
        out += ["## Link work list", "",
                "Every unit compiles, so the next errors are at link time: the GameCube system",
                "functions the game calls, which the platform layer and renderer provide.", "",
                "| Library | Functions |", "|---|---:|"]
        for key, title, _ in SDK_LIBS:
            if needed.get(key):
                out.append(f"| [{title}](#link-{key}) | {len(needed[key])} |")
        out.append("")
        for key, title, _ in SDK_LIBS:
            if needed.get(key):
                out += [f'<a id="link-{key}"></a>', "", f"### {title}", "",
                        ", ".join(f"`{n}`" for n in sorted(needed[key])), ""]

    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text("\n".join(out))
    print(f"{total - len(failed)}/{total} units compile, {len(errors)} unique errors -> "
          f"{REPORT.relative_to(ROOT)}")
    for key, title, _, _ in BUCKETS:
        if by_bucket.get(key):
            print(f"  {len(by_bucket[key]):5}  {title}")


if __name__ == "__main__":
    sys.exit(main())
