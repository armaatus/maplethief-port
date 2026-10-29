"""The snippet suite of the recompiler (#39): one file per hand-written Thumb-2 snippet.

A snippet file (``tests/recomp/snippets/<group>/<name>.snippet``, format in
``tests/recomp/README.md``) holds the code as encoded bytes next to its assembly, the
initial CPU state and memory, and the expected final state. :func:`load` parses one;
:func:`run` lifts it with ``recomp.snippet.lift_snippet``, compiles and runs it with
``recomp_harness.run_snippet`` and returns every difference from the expected state.

The bytes are what the tests run, so CI needs no ARM assembler. Two checks keep them honest:
:func:`capstone_mismatches` disassembles them with Capstone (everywhere) and compares the
text with the assembly, and :func:`clang_encode` assembles the assembly with clang (where
clang has the ARM target) and compares the bytes. Run as a script to fill in or rewrite the
byte column from clang::

    uv run --group test python tests/recomp_snippets.py tests/recomp/snippets/flags/x.snippet
"""

from __future__ import annotations

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from functools import cache
from pathlib import Path

if __name__ == "__main__":  # run as a script: make tests/ and the repository importable
    _here = Path(__file__).resolve()
    sys.path[:0] = [str(_here.parent), str(_here.parents[1])]

from recomp_harness import DATA_BASE, SNIPPET_BASE, WINDOW, run_snippet
from repo_paths import REPO

SNIPPET_DIR = REPO / "tests" / "recomp" / "snippets"
SECTIONS = ("code", "in", "out", "c", "dispatch")
HEADERS = ("title", "ref")  # required
OPTIONAL_HEADERS = ("xfail", "mode")
MODES = ("thumb", "arm")
SP0 = WINDOW - 64  # sp when the snippet starts (tests/recomp/test_runtime.c)
RET = 0xFFFFFFF1  # lr when the snippet starts: MT_TEST_RETURN
NAMES = {"BASE": SNIPPET_BASE, "DATA": DATA_BASE, "SP0": SP0, "RET": RET}
EXTERNAL = re.compile(r"\b(ext|arm)_([0-9a-fA-F]+)\b")  # a function outside the snippet
DATA_SIZES = {".byte": 1, ".short": 2, ".long": 4}
MEM_SIZES = {"byte": 1, "half": 2, "word": 4, "dword": 8}
FLAG_LETTERS = "NZCV"
TRAP_KINDS = {
    "unsupported": 1, "bad_return": 2, "fell_off": 3, "unreachable": 4, "udf": 6, "import": 7,
    "no_function": 8, "sjlj": 9, "uncaught": 10, "terminate": 11, "memory": 12,
}  # fmt: skip


class SnippetError(ValueError):
    """A malformed snippet file; the message names the file and line."""


@dataclass(frozen=True)
class Line:
    """One line of ``[code]``: an instruction or a data directive at ``address``."""

    lineno: int
    address: int
    data: bytes | None  # None: still to be filled in by clang ("?" in the file)
    asm: str

    @property
    def directive(self) -> str | None:
        word = self.asm.split()[0]
        return word if word in DATA_SIZES else None


@dataclass
class Snippet:
    path: Path
    title: str
    ref: str
    lines: list[Line]
    labels: dict[str, int]  # label -> guest address (snippet labels and ext_/arm_ ones)
    label_lines: dict[str, int | None]  # snippet label -> number of the line it labels
    regs_in: dict[str, int] = field(default_factory=dict)  # r0.., s0.., d0.., fpscr
    flags_in: int = 0  # NZCV as bits 3..0
    mem_in: dict[int, bytes] = field(default_factory=dict)
    regs_out: dict[str, int | None] = field(default_factory=dict)  # None: any value
    flags_out: tuple[int | None, ...] | None = None  # per N, Z, C, V: 1, 0 or None (any)
    mem_out: list[tuple[int, int, int]] = field(default_factory=list)  # (addr, size, value)
    c: str = ""
    dispatch: dict[int, str] = field(default_factory=dict)
    xfail: str = ""  # why the lifter can't run it yet (the issue that will make it pass)
    mode: str = "thumb"  # "arm": the code is A32
    trap: tuple[int, int] | None = None  # (kind, pc) of the mt_trap the run must end in
    trap_detail: int | None = None  # and its detail, if [out] names it

    @property
    def arm(self) -> bool:
        return self.mode == "arm"

    @property
    def name(self) -> str:
        return self.path.relative_to(SNIPPET_DIR).with_suffix("").as_posix()

    @property
    def code(self) -> bytes:
        if any(line.data is None for line in self.lines):
            raise SnippetError(f"{self.path}: bytes still to be filled in ('?'); run the tool")
        return b"".join(line.data for line in self.lines if line.data is not None)


# -- parsing -------------------------------------------------------------------------------


def _value(text: str, where: str) -> int:
    """An integer expression: terms (int literals, BASE/DATA/SP0/RET) joined by + and -, or
    ``f32:<float>`` / ``f64:<float>`` for the bits of a float."""
    text = text.strip()
    for prefix, fmt, out in (("f32:", "<f", "<I"), ("f64:", "<d", "<Q")):
        if text.startswith(prefix):
            try:
                return struct.unpack(out, struct.pack(fmt, float(text[len(prefix) :])))[0]
            except (ValueError, OverflowError) as exc:
                raise SnippetError(f"{where}: bad float {text!r}") from exc
    total = 0
    for sign, term in re.findall(r"([+-]?)\s*([^\s+-]+)", text):
        try:
            v = NAMES[term] if term in NAMES else int(term, 0)
        except ValueError as exc:
            raise SnippetError(f"{where}: bad value {term!r} in {text!r}") from exc
        total += -v if sign == "-" else v
    if not re.fullmatch(r"[+-]?\s*[^\s+-]+(\s*[+-]\s*[^\s+-]+)*", text):
        raise SnippetError(f"{where}: bad expression {text!r}")
    return total


def _reg_width(reg: str) -> int | None:
    """32 or 64 for a register name the state can set and check, else None."""
    m = re.fullmatch(r"([rsd])(\d+)", reg)
    if m:
        n = int(m.group(2))
        limit = {"r": 15, "s": 32, "d": 32}[m.group(1)]
        if n < limit:
            return 64 if m.group(1) == "d" else 32
    return 32 if reg == "fpscr" else None


def _flags(text: str, where: str, allow_any: bool) -> tuple[int | None, ...]:
    letters = text.strip()
    if len(letters) != 4:
        raise SnippetError(f"{where}: flags must be four letters like nZCv, got {text!r}")
    out: list[int | None] = []
    for want, got in zip(FLAG_LETTERS, letters, strict=True):
        if got == "?" and allow_any:
            out.append(None)
        elif got in (want, want.lower()):
            out.append(int(got == want))
        else:
            raise SnippetError(f"{where}: flags are N Z C V, lowercase when clear: {text!r}")
    return tuple(out)


def _mem_values(kind: str, text: str, where: str) -> list[int]:
    size = MEM_SIZES[kind]
    values = [_value(v, where) for v in text.split()]
    if not values:
        raise SnippetError(f"{where}: no values")
    for v in values:
        if not -(1 << (8 * size - 1)) <= v < 1 << (8 * size):
            raise SnippetError(f"{where}: {v:#x} does not fit a {kind}")
    return [v & ((1 << 8 * size) - 1) for v in values]


def _state_line(snip: Snippet, text: str, where: str, out: bool) -> None:
    key, sep, value = text.partition("=")
    if not sep:
        raise SnippetError(f"{where}: expected 'name = value', got {text!r}")
    key = key.strip()
    words = key.split()
    if key == "trap" and out:
        kind, *rest = value.split()
        if kind not in TRAP_KINDS or len(rest) not in (1, 2):
            names = ", ".join(TRAP_KINDS)
            raise SnippetError(
                f"{where}: expected 'trap = <{names}> <pc> [<detail>]', got {value!r}"
            )
        snip.trap = (TRAP_KINDS[kind], _value(rest[0], where))
        snip.trap_detail = _value(rest[1], where) if len(rest) == 2 else None
        return
    if key == "flags":
        flags = _flags(value, where, allow_any=out)
        if out:
            snip.flags_out = flags
        else:
            snip.flags_in = sum(int(b or 0) << (3 - i) for i, b in enumerate(flags))
        return
    if len(words) == 2 and words[0] in MEM_SIZES:
        kind, addr = words[0], _value(words[1], where)
        size = MEM_SIZES[kind]
        values = _mem_values(kind, value, where)
        if addr < DATA_BASE or addr + size * len(values) > WINDOW:
            raise SnippetError(f"{where}: memory outside {DATA_BASE:#x}..{WINDOW:#x}")
        for i, v in enumerate(values):
            if out:
                for part in range(0, size, 4):  # the harness reads at most a word at a time
                    width = min(4, size - part)
                    part_value = (v >> (8 * part)) & ((1 << 8 * width) - 1)
                    snip.mem_out.append((addr + i * size + part, width, part_value))
            else:
                snip.mem_in[addr + i * size] = v.to_bytes(size, "little")
        return
    width = _reg_width(key)
    if width is None or (key == "r15"):
        raise SnippetError(f"{where}: unknown state name {key!r}")
    if out and value.strip() == "?":
        snip.regs_out[key] = None
        return
    v = _value(value, where)
    if not -(1 << (width - 1)) <= v < 1 << width:
        raise SnippetError(f"{where}: {v:#x} does not fit {key}")
    (snip.regs_out if out else snip.regs_in)[key] = v & ((1 << width) - 1)


def _code_line(text: str, where: str) -> tuple[bytes | None, str]:
    hexcode, _, asm = text.partition(" ")
    asm = asm.strip()
    if not asm:
        raise SnippetError(f"{where}: expected '<hex bytes> <assembly>', got {text!r}")
    if hexcode == "?":
        return None, asm
    try:
        data = bytes.fromhex(hexcode)
    except ValueError as exc:
        raise SnippetError(f"{where}: bad hex bytes {hexcode!r}") from exc
    if not data:
        raise SnippetError(f"{where}: no bytes")
    return data, asm


def _strip_comment(line: str, section: str | None) -> str:
    """Drop a comment: a line starting with ``#``, a trailing ``@ ...`` in [code] (``#``
    starts an immediate there) or a trailing ``# ...`` in [in], [out] and [dispatch]. The
    headers have no trailing comments (a ref or xfail may name ``#issue``)."""
    stripped = line.strip()
    if stripped.startswith("#"):
        return ""
    if section is None:
        return stripped
    marker = "@" if section == "code" else "#"
    return stripped.split(marker, 1)[0].strip()


def parse(text: str, path: Path) -> Snippet:
    """Parse the text of a snippet file (see tests/recomp/README.md)."""
    headers: dict[str, str] = {}
    section: str | None = None
    body: dict[str, list[tuple[int, str]]] = {s: [] for s in SECTIONS}
    seen: set[str] = set()
    for lineno, raw in enumerate(text.splitlines(), 1):
        where = f"{path}:{lineno}"
        m = re.fullmatch(r"\[(\w+)\]", raw.strip())
        if m:
            section = m.group(1)
            if section not in SECTIONS:
                raise SnippetError(f"{where}: unknown section [{section}]")
            if section in seen:
                raise SnippetError(f"{where}: section [{section}] twice")
            seen.add(section)
            continue
        if section == "c":  # C is kept verbatim
            body["c"].append((lineno, raw.rstrip()))
            continue
        line = _strip_comment(raw, section)
        if not line:
            continue
        if section is None:
            key, sep, value = line.partition(":")
            if not sep or key.strip() not in HEADERS + OPTIONAL_HEADERS:
                names = ", ".join(HEADERS + OPTIONAL_HEADERS)
                raise SnippetError(f"{where}: expected a header ({names})")
            headers[key.strip()] = value.strip()
            continue
        body[section].append((lineno, line))
    for h in HEADERS:
        if not headers.get(h):
            raise SnippetError(f"{path}: missing header '{h}:'")
    if not body["code"]:
        raise SnippetError(f"{path}: no [code]")
    if "out" not in seen:
        raise SnippetError(f"{path}: no [out]")

    lines: list[Line] = []
    labels: dict[str, int] = {}
    label_lines: dict[str, int | None] = {}  # label -> line number of the line it is on
    address = SNIPPET_BASE
    waiting: list[str] = []
    for lineno, text_ in body["code"]:
        where = f"{path}:{lineno}"
        m = re.fullmatch(r"([A-Za-z_]\w*):", text_)
        if m:
            name = m.group(1)
            if name in labels or EXTERNAL.fullmatch(name) or name.startswith("mt_"):
                raise SnippetError(f"{where}: label {name!r} defined twice or reserved")
            labels[name] = address
            waiting.append(name)
            continue
        data, asm = _code_line(text_, where)
        for name in waiting:
            label_lines[name] = lineno
        waiting.clear()
        size = DATA_SIZES.get(asm.split()[0])
        if data is not None and size is not None and len(data) % size:
            raise SnippetError(f"{where}: {asm.split()[0]} needs a multiple of {size} bytes")
        lines.append(Line(lineno, address, data, asm))
        address += 2 if data is None else len(data)  # "?": a guess until the bytes are in
    for name in waiting:
        label_lines[name] = None  # after the last line
    for line in lines:
        for kind, hexaddr in EXTERNAL.findall(line.asm):
            labels[f"{kind}_{hexaddr}"] = int(hexaddr, 16)

    snip = Snippet(path, headers["title"], headers["ref"], lines, labels, label_lines)
    snip.xfail = headers.get("xfail", "")
    snip.mode = headers.get("mode", "thumb")
    if snip.mode not in MODES:
        raise SnippetError(f"{path}: mode must be one of {', '.join(MODES)}, not {snip.mode!r}")
    for section, out in (("in", False), ("out", True)):
        for lineno, text_ in body[section]:
            _state_line(snip, text_, f"{path}:{lineno}", out)
    snip.c = "\n".join(raw for _, raw in body["c"]).strip("\n")
    for lineno, text_ in body["dispatch"]:
        key, sep, value = text_.partition("=")
        if not sep or not re.fullmatch(r"[A-Za-z_]\w*", value.strip()):
            raise SnippetError(f"{path}:{lineno}: expected '<guest address> = <C function>'")
        snip.dispatch[_value(key, f"{path}:{lineno}")] = value.strip()
    return snip


def load(path: Path) -> Snippet:
    return parse(path.read_text(encoding="utf-8"), path)


def all_files() -> list[Path]:
    return sorted(SNIPPET_DIR.rglob("*.snippet"))


# -- running -------------------------------------------------------------------------------


def _expected(snip: Snippet) -> dict[str, int | None]:
    """Every value the run is checked against: the explicit [out] plus "unchanged" for each
    of r0-r14 and the 32-bit halves of d0-d31 that [out] doesn't name."""
    regs_in: dict[str, int] = {"r13": SP0, "r14": RET}
    halves_in: dict[int, int] = {}
    for key, v in snip.regs_in.items():
        if key[0] == "r":
            regs_in[key] = v
        elif key[0] == "s":
            halves_in[int(key[1:])] = v
        elif key[0] == "d":
            n = int(key[1:])
            halves_in[2 * n], halves_in[2 * n + 1] = v & 0xFFFFFFFF, v >> 32
    want: dict[str, int | None] = {f"r{n}": regs_in.get(f"r{n}", 0) for n in range(15)}
    want |= {f"h{n}": halves_in.get(n, 0) for n in range(64)}  # h<n>: half n of d0-d31
    for key, v in snip.regs_out.items():
        if key[0] == "r" or key == "fpscr":
            want[key] = v
        elif key[0] == "s":
            want[f"h{key[1:]}"] = v
        else:
            n = int(key[1:])
            want[f"h{2 * n}"] = None if v is None else v & 0xFFFFFFFF
            want[f"h{2 * n + 1}"] = None if v is None else v >> 32
    flags = snip.flags_out or tuple((snip.flags_in >> (3 - i)) & 1 for i in range(4))
    for letter, bit in zip(FLAG_LETTERS, flags, strict=True):
        want[letter] = bit
    for i, (_, _, value) in enumerate(snip.mem_out):
        want[f"m{i}"] = value
    return want


def _describe(key: str, snip: Snippet) -> str:
    if key.startswith("h"):
        n = int(key[1:])
        return f"s{n}" if n < 32 else f"d{n // 2}.{'hi' if n % 2 else 'lo'}"
    if key.startswith("m"):
        addr, size, _ = snip.mem_out[int(key[1:])]
        return f"{({1: 'byte', 2: 'half', 4: 'word'})[size]} {addr:#x}"
    return {"N": "flag N", "Z": "flag Z", "C": "flag C", "V": "flag V"}.get(key, key)


def run(snip: Snippet, tmp_path: Path) -> list[str]:
    """Lift, compile and run ``snip``; one message per value that differs from [out]."""
    state = dict(snip.regs_in)
    state["cpsr"] = snip.flags_in << 28
    read = {f"m{i}": (addr, size) for i, (addr, size, _) in enumerate(snip.mem_out)}
    # The code is also mapped as data, like __TEXT at run time: an add pc table is read then.
    memory = {SNIPPET_BASE: snip.code, **snip.mem_in}
    out = run_snippet(
        tmp_path,
        snip.code,
        state,
        memory,
        read,
        extra_c=snip.c,
        functions=snip.dispatch,
        arm=snip.arm,
        expect_trap=snip.trap is not None,
    )
    got: dict[str, int] = {f"r{n}": out[f"r{n}"] for n in range(15)}
    for n in range(32):
        got[f"h{2 * n}"], got[f"h{2 * n + 1}"] = out[f"d{n}"] & 0xFFFFFFFF, out[f"d{n}"] >> 32
    for i, letter in enumerate(FLAG_LETTERS):
        got[letter] = (out["cpsr"] >> (31 - i)) & 1
    got["fpscr"] = out["fpscr"]
    got |= {k: out[k] for k in read}
    diffs = []
    want = _expected(snip)
    if snip.trap is not None:
        want |= {"trap_kind": snip.trap[0], "trap_pc": snip.trap[1]}
        want["trap_detail"] = snip.trap_detail
        got |= {k: out[k] for k in ("trap_kind", "trap_pc", "trap_detail")}
    for key, value in want.items():
        if value is not None and got[key] != value:
            diffs.append(f"{_describe(key, snip)}: got {got[key]:#x}, expected {value:#x}")
    return diffs


def forms(snip: Snippet) -> set[str]:
    """The census forms (:mod:`recomp.census`) of the snippet's instructions: what it tests."""
    from recomp.census import function_forms
    from recomp.snippet import snippet_function

    return set(function_forms(snippet_function(snip.code, SNIPPET_BASE, snip.arm)))


# -- checking the bytes --------------------------------------------------------------------

_REG_ALIAS = {"r9": "sb", "r10": "sl", "r11": "fp", "r12": "ip", "r13": "sp", "r14": "lr",
              "r15": "pc"}  # fmt: skip
_NUMBER = re.compile(r"#\s*(-?)(0x[0-9a-f]+|\d+\.\d*(?:e[+-]?\d+)?|\d+e[+-]?\d+|\d+)")


def normalize(text: str, labels: dict[str, int] | None = None) -> str:
    """Assembly text in a form that Capstone's and hand-written syntax share: lower case,
    no ``.w``/``.n`` width suffix, register aliases, labels and immediates as one spelling."""
    text = text.lower().strip()
    for name, addr in sorted((labels or {}).items(), key=lambda kv: -len(kv[0])):
        text = re.sub(rf"(?<![\w.]){re.escape(name.lower())}(?!\w)", f"#{addr:#x}", text)
    mnemonic, _, operands = text.partition(" ")
    mnemonic = re.sub(r"\.[wn]$", "", mnemonic)
    operands = re.sub(r"\b(r9|r1[0-5])\b", lambda m: _REG_ALIAS[m.group(1)], operands)

    def number(m: re.Match[str]) -> str:
        sign, digits = m.groups()
        if digits.startswith("0x"):
            return f"#{(-int(digits, 16) if sign else int(digits, 16)) & 0xFFFFFFFF:#x}"
        if any(c in digits for c in ".e"):
            return f"#{float(sign + digits)!r}"
        return f"#{(-int(digits) if sign else int(digits)) & 0xFFFFFFFF:#x}"

    operands = _NUMBER.sub(number, operands)
    operands = re.sub(r"\s*,\s*", ", ", operands)
    operands = re.sub(r"\[\s*", "[", re.sub(r"\s*\]", "]", operands))
    return f"{mnemonic} {operands}".strip()


def capstone_mismatches(snip: Snippet) -> list[str]:
    """Disassemble each instruction line with Capstone (in sequence, so IT blocks give the
    conditions) and compare with its assembly; data directives are compared by value."""
    import capstone

    mode = capstone.CS_MODE_ARM if snip.arm else capstone.CS_MODE_THUMB
    cs = capstone.Cs(capstone.CS_ARCH_ARM, mode)
    problems = []
    run_lines: list[Line] = []

    def flush() -> None:
        if not run_lines:
            return
        blob = b"".join(line.data or b"" for line in run_lines)
        decoded = {i.address: i for i in cs.disasm(blob, run_lines[0].address)}
        for line in run_lines:
            insn = decoded.get(line.address)
            where = f"line {line.lineno} ({line.asm!r})"
            if insn is None or insn.size != len(line.data or b""):
                size = len(line.data or b"")
                problems.append(f"{where}: Capstone decodes no {size}-byte instruction here")
                continue
            got = normalize(f"{insn.mnemonic} {insn.op_str}")
            want = normalize(line.asm, snip.labels)
            if got != want:
                problems.append(f"{where}: Capstone reads {got!r}, the file says {want!r}")
        run_lines.clear()

    for line in snip.lines:
        if line.data is None:
            problems.append(f"line {line.lineno}: bytes not filled in")
            continue
        if line.directive:
            flush()
            size = DATA_SIZES[line.directive]
            values = [
                _value(v, f"line {line.lineno}") for v in line.asm.split(None, 1)[1].split(",")
            ]
            want = b"".join((v & ((1 << 8 * size) - 1)).to_bytes(size, "little") for v in values)
            if want != line.data:
                problems.append(
                    f"line {line.lineno}: {line.asm} is {want.hex()}, not {line.data.hex()}"
                )
            continue
        run_lines.append(line)
    flush()
    return problems


CLANG_FLAGS = ("--target=thumbv7-none-eabi", "-march=armv7-a", "-mfpu=neon", "-c")


def _clang_candidates() -> list[str]:
    """``$MAPLETHIEF_CLANG``, then ``clang``, then versioned ``clang-NN`` on PATH (newest
    first; Linux distributions often install only those)."""
    found = [os.environ.get("MAPLETHIEF_CLANG", ""), shutil.which("clang") or ""]
    versioned = []
    for d in os.environ.get("PATH", "").split(os.pathsep):
        for exe in Path(d).glob("clang-[0-9]*") if d and Path(d).is_dir() else ():
            m = re.fullmatch(r"clang-(\d+)(\.exe)?", exe.name)
            if m:
                versioned.append((int(m.group(1)), str(exe)))
    found += [path for _, path in sorted(versioned, reverse=True)]
    return [path for path in found if path]


@cache
def clang() -> str | None:
    """A clang that can assemble Thumb-2 with VFPv3 (Apple's and LLVM's can), else None."""
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "probe.s"
        probe = ".syntax unified\n.thumb\nvadd.f64 d16, d17, d18\nbx lr\n"
        src.write_text(probe, encoding="utf-8")
        for path in _clang_candidates():
            cmd = [path, *CLANG_FLAGS, str(src), "-o", str(Path(tmp) / "probe.o")]
            try:
                done = subprocess.run(cmd, capture_output=True, text=True)
            except OSError:
                continue
            if done.returncode == 0:
                return path
    return None


def _elf_text(obj: bytes) -> tuple[bytes, dict[str, int], set[int]]:
    """``.text``, its symbols (name -> value) and its relocated offsets from an ELF32 object."""
    (shoff,) = struct.unpack_from("<I", obj, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", obj, 0x2E)
    secs = [struct.unpack_from("<10I", obj, shoff + i * shentsize) for i in range(shnum)]

    def cstr(table: int, off: int) -> str:
        start = secs[table][4] + off
        return obj[start : obj.index(b"\0", start)].decode()

    names = [cstr(shstrndx, s[0]) for s in secs]
    text_index = names.index(".text")
    text = obj[secs[text_index][4] : secs[text_index][4] + secs[text_index][5]]
    symbols: dict[str, int] = {}
    relocs: set[int] = set()
    for s in secs:
        if s[1] == 2:  # SHT_SYMTAB; sh_link is its string table
            for off in range(s[4], s[4] + s[5], 16):
                st_name, st_value, _, _, _, st_shndx = struct.unpack_from("<IIIBBH", obj, off)
                if st_shndx == text_index and st_name:
                    symbols[cstr(s[6], st_name)] = st_value
        elif s[1] in (9, 4) and s[7] == text_index:  # SHT_REL / SHT_RELA for .text
            step = 8 if s[1] == 9 else 12
            offsets = range(s[4], s[4] + s[5], step)
            relocs |= {struct.unpack_from("<I", obj, o)[0] for o in offsets}
    return text, symbols, relocs


def clang_source(snip: Snippet) -> str:
    """The snippet as an assembly file for clang: the code at its guest address, a marker
    symbol before each line, and a one-instruction function at each ext_/arm_ address."""
    out = [".syntax unified", ".arm" if snip.arm else ".thumb", ".text"]
    out.append(f".space {SNIPPET_BASE:#x}")
    before: dict[int | None, list[str]] = {}
    for name, lineno in snip.label_lines.items():
        before.setdefault(lineno, []).append(name)
    for line in snip.lines:
        out += [f"{name}:" for name in before.get(line.lineno, [])]
        out += [f"mt_line_{line.lineno}:", f"    {line.asm}"]
    out += [f"{name}:" for name in before.get(None, [])]
    out.append("mt_line_end:")
    for name, addr in sorted(snip.labels.items(), key=lambda kv: kv[1]):
        m = EXTERNAL.fullmatch(name)
        if m:
            arm = m.group(1) == "arm"
            out += [f".org {addr:#x}", ".arm" if arm else ".thumb"]
            out += [f"{name}:"] if arm else [".thumb_func", f"{name}:"]
            out.append("    bx lr")
    return "\n".join(out) + "\n"


def clang_encode(snip: Snippet) -> dict[int, bytes | None]:
    """The bytes clang assembles for each line (by line number); None where clang leaves a
    relocation for the linker (``blx`` to an ARM function), which only Capstone checks."""
    path = clang()
    if path is None:
        raise RuntimeError("no clang with the ARM target")
    with tempfile.TemporaryDirectory() as tmp:
        src, obj = Path(tmp) / "snippet.s", Path(tmp) / "snippet.o"
        src.write_text(clang_source(snip), encoding="utf-8")
        done = subprocess.run(
            [path, *CLANG_FLAGS, str(src), "-o", str(obj)], capture_output=True, text=True
        )
        if done.returncode != 0:
            raise SnippetError(f"{snip.path}: clang failed:\n{done.stderr}")
        text, symbols, relocs = _elf_text(obj.read_bytes())
    marks = sorted((v, int(k[8:])) for k, v in symbols.items() if re.fullmatch(r"mt_line_\d+", k))
    ends = [v for v, _ in marks[1:]] + [symbols["mt_line_end"]]
    result: dict[int, bytes | None] = {}
    for (start, lineno), end in zip(marks, ends, strict=True):
        result[lineno] = None if any(start <= r < end for r in relocs) else text[start:end]
    return result


def fill_in(path: Path) -> list[str]:
    """Rewrite the byte column of ``path`` from clang; returns the lines it changed."""
    text = path.read_text(encoding="utf-8")
    changed: list[str] = []
    for _ in range(8):  # placeholders change later addresses, so repeat until stable
        snip = parse(text, path)
        encoded = clang_encode(snip)
        rows = text.split("\n")
        for line in snip.lines:
            data = encoded[line.lineno]
            if data is None:
                if line.data is None:
                    where = f"{path}:{line.lineno}"
                    raise SnippetError(f"{where}: clang leaves a relocation; encode it by hand")
                continue
            if data != line.data:
                row = rows[line.lineno - 1]
                indent = row[: len(row) - len(row.lstrip())]
                rest = row.strip().split(" ", 1)[1].lstrip()
                rows[line.lineno - 1] = f"{indent}{data.hex():<12}{rest}"
                changed.append(f"{line.lineno}: {data.hex()} {line.asm}")
        text_new = "\n".join(rows)
        if text_new == text:
            return changed
        text = text_new
        path.write_text(text, encoding="utf-8")
    raise SnippetError(f"{path}: the bytes did not settle")


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 2
    if clang() is None:
        print("no clang with the ARM target on PATH", file=sys.stderr)
        return 1
    for arg in argv:
        for line in fill_in(Path(arg)):
            print(f"{arg}:{line}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
