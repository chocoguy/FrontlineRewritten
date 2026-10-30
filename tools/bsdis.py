#!/usr/bin/env python3
"""Disassembles a compiled BS script class (<name>.sin and <name>.cbs) from a
level archive, read-only. The formats are those of the decomp's bsfile.cpp
(loading) and bsmachin.cpp (the opcodes).

    tools/bsdis.py <level.viv> <Class>                 states, handlers and code
    tools/bsdis.py <level.viv> <Class> --states        states and handlers only
    tools/bsdis.py <level.viv> <Class> --sites GetRandomInteger,RegisterTimerEvent
                                                       each call of those built-ins
                                                       with the code around it
    tools/bsdis.py <level.viv> <Class> --thread 0 --range 0x2e0:0x3a0
                                                       one stretch of a thread's code
    tools/bsdis.py <level.viv> <Class> --thread 0 --callers 0x2f9
                                                       where the function at a word is called
    tools/bsdis.py <level.viv> <Class> --thread 0 --summary
                                                       per state: debug strings, built-ins,
                                                       the states it moves to, rolls and timers
    tools/bsdis.py <level.viv> --check-sites game/native/ai/script_bias.cpp
                                                       checks each listed call site: the
                                                       built-in and its constant arguments
    tools/bsdis.py <level.viv> --classes               the script classes in it

Code addresses print as t<thread>:<segment>:<word>. Segment 0..n-1 of a
derived class are its base's code (n = inherited segments), segment n its
own; a call site's word offset within its own class's code is what
game/native/ai/script_bias.cpp keys on. Built-in names come from the decomp's
g_BIFCRCArray (bsbifunc.cpp), matched by the CRCs in the level's bsbifunc.dat.
"""

import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIF_SOURCE = ROOT / "FrontlineDecompiled/src/game/bsbifunc.cpp"

OPCODES = {
    0: "BNE", 1: "JMP", 2: "ADD", 3: "SUB", 4: "DIV", 5: "MULT", 6: "NEG", 7: "EQ", 8: "LT", 9: "GT",
    10: "LTE", 11: "GTE", 12: "AND", 13: "OR", 14: "NOT", 15: "LW", 16: "SW", 17: "PUSH", 18: "POP",
    19: "CAST", 20: "FNCALL", 21: "RETURN", 22: "BREAK", 23: "NEXTSTATE", 24: "LG", 25: "SG",
    26: "BIF", 27: "LM", 28: "SM", 29: "GP", 30: "NE", 31: "TRACE", 32: "LS",
}
TYPED = {"ADD", "SUB", "DIV", "MULT", "EQ", "NE", "LT", "GT", "LTE", "GTE"}
TYPES = {0x0202: "ii", 0x0203: "if", 0x0302: "fi", 0x0303: "ff"}  # left type in the low byte


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def s16(d, o):
    return struct.unpack_from("<h", d, o)[0]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def cstr(d, o):
    return d[o:d.index(b"\0", o)].decode("latin1")


def read_viv(path):
    """A C0FB archive: name -> bytes."""
    d = Path(path).read_bytes()
    if d[:2] != b"\xc0\xfb":
        sys.exit(f"{path}: not a C0FB archive")
    n = int.from_bytes(d[4:6], "big")
    i = 6
    files = {}
    for _ in range(n):
        off = int.from_bytes(d[i:i + 3], "big")
        size = int.from_bytes(d[i + 3:i + 6], "big")
        i += 6
        j = d.index(b"\0", i)
        files[d[i:j].decode("latin1").lower()] = d[off:off + size]
        i = j + 1
    return files


def bif_names(files):
    """Built-in index -> (name, argument count)."""
    src = BIF_SOURCE.read_text()
    by_crc = {int(c, 16): n for c, n in re.findall(r"\{0x([0-9A-Fa-f]+),\s*BIFunc_(\w+)\}", src)}
    d = files["bsbifunc.dat"]
    count = u32(d, 0xC)
    out = []
    for i in range(count):
        e = 0x14 + 0x10 * i
        crc = u32(d, e + 4)
        out.append((by_crc.get(crc, f"bif_{crc:08x}"), s16(d, e + 0xC)))
    return out


class Script:
    """One class: its strings, and per thread the states and code."""

    def __init__(self, files, name):
        self.name = name
        sin = files[name.lower() + ".sin"]
        cbs = files[name.lower() + ".cbs"]
        cls = 0x64
        self.class_name = cstr(sin, cls + 8)
        num_threads = u16(sin, cls + 0x58)
        num_strings = u32(sin, cls + 0x5C)
        strings_at = u32(sin, cls + 0x60)
        self.strings = []
        offset = 0xC8
        if num_strings > 0:
            offset = num_strings * 4 + 0xC8
            for k in range(num_strings):
                s = cstr(sin, u32(sin, strings_at + 4 * k))
                self.strings.append(s)
                n = len(s) + 1
                offset += (n + 3) & ~3
        threads_at = offset
        offset += num_threads * 0x1C
        self.threads = []
        for t in range(num_threads):
            c = threads_at + 0x1C * t
            th = {
                "handlers": u16(sin, c + 0xC),
                "locals": u16(sin, c + 0x14),
                "initial": u16(sin, c + 0x16),
                "inherited": sin[c + 0x18],
                "states": {},
            }
            offset += (th["inherited"] + 1) * 4
            slots = [u32(sin, offset + 4 * h) for h in range(th["handlers"])]
            offset += th["handlers"] * 4
            real = []
            for h, v in enumerate(slots):
                if v in (0xFFFFFFFF, 0xFFFFFFFE):
                    th["states"][h] = "none" if v == 0xFFFFFFFF else "inherited"
                else:
                    real.append((h, offset))
                    offset += 0x14
            for h, at in real:
                th["states"][h] = {
                    "entry": u32(sin, at),
                    "parent": u16(sin, at + 0xC),
                    "level": sin[at + 0xE],
                    "num_messages": sin[at + 0xF],
                    "num_events": sin[at + 0x10],
                    "events": [],
                    "messages": [],
                }
            for h, at in real:
                st = th["states"][h]
                for _ in range(st["num_events"]):
                    st["events"].append({"address": u32(sin, offset), "event": u16(sin, offset + 4),
                                         "bucket": sin[offset + 6], "flags": sin[offset + 7]})
                    offset += 8
            for h, at in real:
                st = th["states"][h]
                for _ in range(st["num_messages"]):
                    st["messages"].append({"address": u32(sin, offset), "filter_arg": u32(sin, offset + 4),
                                           "message": u16(sin, offset + 8), "filter": sin[offset + 0xA],
                                           "flags": sin[offset + 0xB]})
                    offset += 0xC
            self.threads.append(th)
        self.base = cstr(cbs, 0x24)
        count = u32(cbs, 0x64)
        o = 0x68
        for t in range(count):
            n = u32(cbs, o)
            o += 4
            self.threads[t]["code"] = list(struct.unpack_from(f"<{n}I", cbs, o)) if n else []
            o += 4 * n

    def string(self, k):
        return self.strings[k] if 0 <= k < len(self.strings) else f"<string {k}>"


def decode(words, i):
    """(mnemonic, operand text, length in words) of the instruction at word i."""
    w = words[i]
    op = w & 0xFF
    name = OPCODES.get(op, f"op{op}")
    operand = struct.unpack("<h", struct.pack("<H", w >> 16))[0]
    if name == "PUSH":
        v = words[i + 1] if i + 1 < len(words) else 0
        f = struct.unpack("<f", struct.pack("<I", v))[0]
        text = f"{v}" if v < 0x10000 or v > 0xFFFF0000 else f"0x{v:x}"
        if 1e-4 < abs(f) < 1e6 and v >= 0x10000:
            text += f" ({f:g}f)"
        return name, text, 2
    if name == "FNCALL":
        v = words[i + 1] if i + 1 < len(words) else 0
        return name, f"{v >> 24}:{v & 0xFFFFFF:#x} (args {(w >> 16) & 0xFF})", 2
    if name in TYPED:
        return name + "." + TYPES.get(w >> 16, f"{w >> 16:#x}"), "", 1
    if name in ("JMP", "BNE", "BREAK"):
        return name, f"{i + operand:#x}", 1
    if name == "RETURN":
        return name, f"words {w >> 24} value {(w >> 16) & 0xFF}", 1
    if name == "GP":
        return name, f"property {(w >> 16) & 0xFF:#x}" + (" of frame[3]" if w >> 24 else ""), 1
    if name in ("NOT", "OR", "AND", "NEXTSTATE", "TRACE"):
        return name, "", 1
    return name, f"{operand}", 1


def labels_of(script, t):
    """Own-code word -> labels (state entries, event handlers, call targets)."""
    th = script.threads[t]
    own = th["inherited"]
    labels = {}

    def add(addr, text):
        if addr >> 24 == own:
            labels.setdefault(addr & 0xFFFFFF, []).append(text)

    for h, st in th["states"].items():
        if isinstance(st, dict):
            add(st["entry"], f"state {h} entry")
            for e in st["events"]:
                add(e["address"], f"state {h} on event {e['event']:#x}")
            for m in st["messages"]:
                add(m["address"], f"state {h} on message {m['message']:#x}")
    words = th["code"]
    i = 0
    while i < len(words):
        name, text, n = decode(words, i)
        if name == "FNCALL":
            add(words[i + 1], "function")
        i += n
    return labels


def listing(script, t, bifs):
    """(word, text) lines of a thread's own code."""
    words = script.threads[t]["code"]
    out = []
    i = 0
    while i < len(words):
        name, text, n = decode(words, i)
        if name == "BIF":
            idx = words[i] >> 16
            bif, args = bifs[idx] if idx < len(bifs) else (f"bif{idx}", "?")
            text = f"{bif} ({args} args)"
        elif name == "LS":
            text = repr(script.string(words[i] >> 16))
        elif name == "NEXTSTATE":
            text = "(state on top)"
        out.append((i, f"{name:<10} {text}".rstrip()))
        i += n
    return out


def print_states(script, t):
    th = script.threads[t]
    print(f"thread {t}: {th['handlers']} states, initial {th['initial']}, {th['locals']} locals, "
          f"{th['inherited']} inherited segment(s), {len(th['code'])} own code words")
    for h in sorted(th["states"]):
        st = th["states"][h]
        if not isinstance(st, dict):
            if st != "none":
                print(f"  state {h}: {st}")
            continue
        parent = "-" if st["parent"] == 0xFFFF else st["parent"]
        e = st["entry"]
        print(f"  state {h}: level {st['level']} parent {parent} entry {e >> 24}:{e & 0xFFFFFF:#x}")
        for ev in st["events"]:
            a = ev["address"]
            print(f"    event {ev['event']:#06x} bucket {ev['bucket']:2} flags {ev['flags']} -> {a >> 24}:{a & 0xFFFFFF:#x}")
        for m in st["messages"]:
            a = m["address"]
            print(f"    message {m['message']:#06x} filter {m['filter']}:{m['filter_arg']:#x} flags {m['flags']} "
                  f"-> {a >> 24}:{a & 0xFFFFFF:#x}")


def summarize(script, t, lines, labels):
    """Code grouped by the state whose entry or handler it follows (functions
    by themselves): strings, built-ins, NEXTSTATE targets, rolls and timers."""
    groups = {}
    order = []
    current = "start"
    prev = None
    for k, (i, text) in enumerate(lines):
        if i in labels:
            lab = labels[i][0]
            current = lab.split(" on ")[0].replace(" entry", "") if lab.startswith("state") else f"function {i:#x}"
        g = groups.get(current)
        if g is None:
            g = groups[current] = {"strings": [], "bifs": {}, "next": set(), "sites": [], "words": 0}
            order.append(current)
        g["words"] += 1
        op = text.split()[0]
        if op == "LS":
            g["strings"].append(text[11:])
        elif op == "BIF":
            name = text.split()[1]
            g["bifs"][name] = g["bifs"].get(name, 0) + 1
            if name in ("GetRandomInteger", "GetRandomFloat", "RegisterTimerEvent"):
                args = [lines[j][1].split()[1] for j in range(max(0, k - 3), k) if lines[j][1].startswith("PUSH")]
                g["sites"].append(f"{i:#06x} {name}({', '.join(args)})")
        elif op == "NEXTSTATE" and prev is not None and prev[1].startswith("PUSH"):
            g["next"].add(int(prev[1].split()[1]))
        prev = (i, text)
    for name in order:
        g = groups[name]
        print(f"\n{name}: {g['words']} instructions" + (f"; to states {sorted(g['next'])}" if g["next"] else ""))
        for st in g["strings"]:
            print(f"    says {st}")
        top = sorted(g["bifs"].items(), key=lambda x: -x[1])[:10]
        if top:
            print("    calls " + ", ".join(f"{n} x{c}" for n, c in top))
        for site in g["sites"]:
            print(f"    site {site}")


def check_sites(files, path):
    """Each kSites entry of script_bias.cpp must be a call of the built-in its
    kind uses, with the listed arguments pushed as constants. Returns the
    number of mismatches."""
    entry = re.compile(r'\{"(\w+)",\s*(\d+),\s*0x([0-9A-Fa-f]+),\s*(k\w+),\s*(-?\d+),\s*(-?\d+),')
    bifs = bif_names(files)
    scripts = {}
    bad = 0
    for cls, t, word, kind, a1, a2 in entry.findall(Path(path).read_text()):
        t, word, a1, a2 = int(t), int(word, 16), int(a1), int(a2)
        key = cls.lower()
        if key + ".cbs" not in files:
            print(f"  {cls}: not in this level")
            continue
        sc = scripts.setdefault(key, Script(files, cls))
        words = sc.threads[t]["code"]
        want = "RegisterTimerEvent" if kind == "kTimer" else "GetRandomInteger"
        w = words[word] if word < len(words) else 0
        name = bifs[w >> 16][0] if (w & 0xFF) == 26 and (w >> 16) < len(bifs) else None
        # the pushes just before the call, last argument nearest
        pushed = []
        i = word - 2
        while i >= 0 and (words[i] & 0xFF) == 17 and len(pushed) < 3:
            pushed.insert(0, words[i + 1])
            i -= 2
        expect = [a1, a2] if kind != "kTimer" else [a1, a2, 0]
        ok = name == want and pushed[-len(expect):] == expect
        print(f"  {'ok ' if ok else 'BAD'} {cls} t{t}:{word:#06x} {kind} {name} pushes {pushed}")
        bad += not ok
    print("all sites match" if bad == 0 else f"{bad} mismatched")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("viv")
    ap.add_argument("cls", nargs="?")
    ap.add_argument("--classes", action="store_true")
    ap.add_argument("--states", action="store_true")
    ap.add_argument("--thread", type=int)
    ap.add_argument("--sites", help="comma-separated built-in names")
    ap.add_argument("--range", help="from:to words of the own code (with --thread)")
    ap.add_argument("--callers", help="word of a function in the own code (with --thread)")
    ap.add_argument("--summary", action="store_true", help="per state (with --thread)")
    ap.add_argument("--check-sites", help="script_bias.cpp, whose kSites to check against this level")
    ap.add_argument("--before", type=int, default=8)
    ap.add_argument("--after", type=int, default=10)
    a = ap.parse_args()
    files = read_viv(a.viv)
    if a.check_sites:
        sys.exit(check_sites(files, a.check_sites))
    if a.classes or a.cls is None:
        for n in sorted(f[:-4] for f in files if f.endswith(".cbs")):
            print(n)
        return
    bifs = bif_names(files)
    script = Script(files, a.cls)
    print(f"class {script.class_name}" + (f" : {script.base}" if script.base else "") +
          f", {len(script.threads)} threads, {len(script.strings)} strings")
    threads = [a.thread] if a.thread is not None else range(len(script.threads))
    want = set(a.sites.split(",")) if a.sites else None
    for t in threads:
        if a.states:
            print_states(script, t)
            continue
        labels = labels_of(script, t)
        lines = listing(script, t, bifs)
        seg = script.threads[t]["inherited"]
        if a.range:
            lo, hi = (int(x, 0) for x in a.range.split(":"))
            for i, text in lines:
                if lo <= i <= hi:
                    for lab in labels.get(i, []):
                        print(f"        ; {lab}")
                    print(f"  t{t}:{seg}:{i:#06x}  {text}")
            continue
        if a.callers:
            target = (seg << 24) | int(a.callers, 0)
            words = script.threads[t]["code"]
            current = None
            for i, text in lines:
                if i in labels:
                    current = labels[i][0]
                if text.startswith("FNCALL") and words[i + 1] == target:
                    print(f"  t{t}:{seg}:{i:#06x}  called  [{current}]")
            continue
        if a.summary:
            summarize(script, t, lines, labels)
            continue
        if want is None:
            print_states(script, t)
            for i, text in lines:
                for lab in labels.get(i, []):
                    print(f"        ; {lab}")
                print(f"  t{t}:{script.threads[t]['inherited']}:{i:#06x}  {text}")
            continue
        # Call sites with context, and the label they fall under.
        current = None
        owner = {}
        for i, _ in lines:
            if i in labels:
                current = labels[i][0]
            owner[i] = current
        for k, (i, text) in enumerate(lines):
            if not text.startswith("BIF") or text.split()[1] not in want:
                continue
            print(f"\n== t{t}:{script.threads[t]['inherited']}:{i:#06x} {text.split()[1]}  [{owner[i]}]")
            for j in range(max(0, k - a.before), min(len(lines), k + a.after + 1)):
                w, tx = lines[j]
                mark = ">>" if j == k else "  "
                lab = "".join(f"   ; {x}" for x in labels.get(w, []))
                print(f"  {mark} {w:#06x}  {tx}{lab}")


if __name__ == "__main__":
    main()
