"""The coverage report (#40) on synthetic inputs: fake functions with a problem at every stage
give the expected coverage.md and coverage.json, every function that is not clean has a
reason, and compile errors in a generated program are charged to the function they are in
(compiled with the host compiler: MSVC cl on the Windows runner). No game code."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from recomp import coverage
from recomp.coverage import CompileResult, Failure, collect
from recomp.discover import Discovery, Function
from recomp.exceptions import Context, Report
from recomp.imports import ImportTable
from recomp.lift.core import Lifted, c_name
from recomp.output import write_program

SOURCE = {"path": "work/slices/Fake.armv7", "arch": "armv7", "sha256": "ab" * 32}
CLEAN, BAD_TABLE, FLAGS, NEON, SJLJ, BAD_C, NOT_LIFTED, UNTAGGED, ALONE = (
    0x1001, 0x1011, 0x1021, 0x1031, 0x1041, 0x1051, 0x1061, 0x1071, 0x2001,
)  # fmt: skip


def _function(ptr: int, *problems: str) -> Function:
    return Function(ptr, ptr & ~1, (ptr & ~1) + 0x10, problems=list(problems))


def _lifted(ptr: int, problems=(), unsupported=()) -> Lifted:
    body = f"void {c_name(ptr)}(mt_cpu *MT_RESTRICT cpu)\n{{\n    (void)cpu;\n}}\n"
    return Lifted(ptr, c_name(ptr), body, list(unsupported), list(problems))


def fake_run(compile_result: CompileResult) -> coverage.Coverage:
    """Nine functions: one clean, one untagged and clean, and one with a problem at each
    stage, plus an import table problem and a dispatch.c that does not compile."""
    discovered = [CLEAN, BAD_TABLE, FLAGS, NEON, SJLJ, BAD_C, NOT_LIFTED, UNTAGGED]
    problems = {BAD_TABLE: ["jump table at 0x1014: no bound found"]}
    functions = {p: _function(p, *problems.get(p, ())) for p in discovered}
    found = Discovery(functions, {}, {}, tuple(discovered))
    lifted = [
        _lifted(CLEAN),
        _lifted(BAD_TABLE),
        _lifted(FLAGS, problems=["flag c may be undefined at 0x1024 (read by bhi)"]),
        _lifted(NEON, unsupported=[(0x1034, "vshr.u32 d0, d0, #1")]),
        _lifted(SJLJ),
        _lifted(BAD_C),
        _lifted(UNTAGGED),
        _lifted(ALONE),
    ]
    sjlj = Report([Context(SJLJ, problems=["no jbuf[1] store before the Register call"])],
                  {}, {}, {})  # fmt: skip
    imports = ImportTable((), {}, (), ("bind at 0x3000: _gone is not an import",))
    tags = {p: ("engine", f"Fn{p:X}") for p in discovered if p != UNTAGGED}
    tags[ALONE] = ("lib:crt1", "start")
    tags[CLEAN] = ("app-objc", "-[Fake clean|pipe]")
    signals = {
        "forms": {"forms": 3, "listed": 2, "missing": ["neon vshr.u32 d,d,#"]},
        "difftest": {"tested": 2, "passed": 1, "with_mismatches": 1, "not_bounded": 0,
                     "not_in_run": 1, "mismatched": ["0x1001"]},
    }  # fmt: skip
    return collect(SOURCE, found, lifted, compile_result, exceptions=sjlj, imports=imports,
                   tags=tags, signals=signals)  # fmt: skip


def measured() -> CompileResult:
    ok = {CLEAN, BAD_TABLE, FLAGS, NEON, SJLJ, UNTAGGED, ALONE}
    failures = [
        Failure(BAD_C, "compile", "f_00001051.c:3:5: error: use of undeclared identifier 'x'"),
        Failure(None, "compile", "dispatch.c: dispatch.c:9:1: error: expected ';'"),
    ]
    flags = ["-std=c17", "-Werror"]
    return CompileResult(True, "clang", flags, 4, ["dispatch.c", "fn_000.c"], ok, failures)


# coverage.md of fake_run(measured()), checked line by line.
EXPECTED_MD = Path(__file__).parent / "recomp" / "coverage" / "expected.md"


def test_markdown_of_fake_functions() -> None:
    assert fake_run(measured()).to_markdown() == EXPECTED_MD.read_text(encoding="utf-8")


def test_json_of_fake_functions() -> None:
    cov = fake_run(measured())
    doc = json.loads(cov.to_json())
    assert doc["schema"] == 1 and doc["source"] == SOURCE and doc["target"] == 0.99
    assert doc["met"] is False
    assert doc["counts"] == {
        "functions": 9, "discovered": 7, "lifted": 3, "compiled": 7, "ok": 2,
        "failures": 7, "program_problems": 2,
    }  # fmt: skip
    assert doc["by_tag"]["engine"] == {
        "functions": 6, "discovered": 5, "lifted": 1, "compiled": 4, "ok": 0,
    }  # fmt: skip
    assert doc["compile"] == {"measured": True, "compiler": "clang", "flags": ["-std=c17",
        "-Werror"], "units": 4, "failed_units": ["dispatch.c", "fn_000.c"]}  # fmt: skip
    assert doc["program_problems"] == [
        {"stage": "imports", "reason": "bind at 0x3000: _gone is not an import"},
        {"stage": "compile", "reason": "dispatch.c: dispatch.c:9:1: error: expected ';'"},
    ]
    rows = {int(r["ptr"], 16): r for r in doc["functions"]}
    assert sorted(rows) == sorted(cov.functions) and len(rows) == 9
    assert rows[CLEAN] == {"ptr": "0x1001", "tag": "app-objc", "name": "-[Fake clean|pipe]",
                           "discovered": True, "lifted": True, "compiled": True,
                           "failures": []}  # fmt: skip
    reason = "unsupported instruction: vshr.u32 d0, d0, #1"
    assert rows[NEON]["failures"] == [{"stage": "unsupported", "address": "0x1034",
                                       "reason": reason}]  # fmt: skip
    assert rows[NOT_LIFTED]["lifted"] is False and rows[NOT_LIFTED]["compiled"] is False
    assert rows[UNTAGGED]["tag"] == "untagged" and rows[UNTAGGED]["name"] is None
    assert doc["signals"]["sjlj"] == {"contexts": 1, "lsdas": 0, "call_sites": 0, "problems": 1}
    assert doc["signals"]["dispatch"]["entries"] == 8
    assert cov.to_json() == fake_run(measured()).to_json()  # no timings: reproducible


def test_every_function_that_is_not_clean_has_a_reason() -> None:
    cov = fake_run(measured())
    bad = cov.not_clean()
    assert bad == [BAD_TABLE, FLAGS, NEON, SJLJ, BAD_C, NOT_LIFTED, ALONE]
    assert all(cov.by_ptr.get(p) for p in bad)
    assert [p for p in cov.functions if cov.is_ok(p)] == [CLEAN, UNTAGGED]


def test_report_without_compiling() -> None:
    cov = fake_run(coverage.not_measured("not asked for: --compile"))
    md = cov.to_markdown()
    assert "**3 of 9 functions (33.33%) lift cleanly.** Compiling was not measured" in md
    assert "| Compiled | not measured | not measured |" in md
    assert "| `engine` | 6 | 5 | 1 | not measured | not measured |" in md
    doc = json.loads(cov.to_json())
    assert doc["met"] is None and doc["counts"]["compiled"] is None
    assert doc["compile"] == {"measured": False, "why": "not asked for: --compile"}
    assert all(r["compiled"] is None for r in doc["functions"])
    assert cov.not_clean() == [BAD_TABLE, FLAGS, NEON, SJLJ, NOT_LIFTED, ALONE]
    line = coverage.summary_line(cov)
    assert line == (
        "recomp: coverage: 9 functions, 7 discovered, 3 lifted cleanly (33.33%), compile not "
        "measured (not asked for: --compile), 6 failures, 1 program problems; target 99% NOT "
        "met for lifting"
    )


def test_clean_run_meets_the_target() -> None:
    ptrs = [0x1001 + 0x10 * i for i in range(200)]
    found = Discovery({p: _function(p) for p in ptrs}, {}, {}, tuple(ptrs))
    result = CompileResult(True, "cc", [], 1, [], set(ptrs[:-2]))  # 2 of 200 not compiled
    result.failures = [Failure(p, "compile", "error") for p in ptrs[-2:]]
    cov = collect(SOURCE, found, [_lifted(p) for p in ptrs], result)
    assert cov.share("ok") == 0.99 and cov.met is True
    assert "Target 99%: met." in cov.to_markdown()
    assert coverage.summary_line(cov, Path("out.md")).endswith("target 99% met; wrote out.md")
    result.failures.append(Failure(ptrs[0], "compile", "error"))
    result.compiled.discard(ptrs[0])
    assert collect(SOURCE, found, [_lifted(p) for p in ptrs], result).met is False


def test_tags_difftest_and_form_signals(tmp_path) -> None:
    tags = tmp_path / "function-tags.json"
    tags.write_text(json.dumps({"functions": [
        {"address": "0x27E0", "tag": "lib:crt1", "name": "start"},
        {"address": "0x280D", "tag": "app-objc", "name": "main"},
    ]}), encoding="utf-8")  # fmt: skip
    assert coverage.load_tags(tags) == {0x27E0: ("lib:crt1", "start"),
                                        0x280D: ("app-objc", "main")}  # fmt: skip
    assert coverage.load_tags(tmp_path / "missing.json") == {}
    diff = tmp_path / "difftest.json"
    diff.write_text(json.dumps({
        "summary": {"tested": 3, "passed": 2, "with_mismatches": 1, "not_bounded": 0},
        "functions": [{"ptr": "0x1001", "passed": True, "mismatches": 0},
                      {"ptr": "0x1011", "passed": True, "mismatches": 0},
                      {"ptr": "0x9001", "passed": False, "mismatches": 3}],
    }), encoding="utf-8")  # fmt: skip
    assert coverage.difftest_signal(diff, [0x1001, 0x1011]) == {
        "tested": 3, "passed": 2, "with_mismatches": 1, "not_bounded": 0, "not_in_run": 1,
        "mismatched": ["0x9001"],
    }  # fmt: skip
    assert coverage.difftest_signal(tmp_path / "none.json", []) is None

    class Census:
        forms = {"adds r,r,#": None, "neon vshr.u32 d,d,#": None, "cond eq": None}

    lists = [tmp_path / "a.txt", tmp_path / "b.txt"]
    lists[0].write_text("# integer forms\nadds r,r,#\ncond eq\n", encoding="utf-8")
    lists[1].write_text("# vfp forms\nvadd.f32 s,s,s\n", encoding="utf-8")
    assert coverage.form_signal(Census(), lists) == {
        "forms": 3, "listed": 2, "missing": ["neon vshr.u32 d,d,#"],
    }  # fmt: skip
    assert coverage.form_signal(Census(), [tmp_path / "none.txt"]) is None


def test_compiler_flags_follow_the_build() -> None:
    gnu = coverage.Compiler("/usr/bin/clang", "gnu")
    cmd = gnu.command(Path("u.c"), Path("u.o"), [Path("inc")])
    assert cmd[1:] == ["-std=c17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O0",
                       "-ffp-contract=off", "-Iinc", "-c", "u.c", "-o", "u.o"]  # fmt: skip
    cl = coverage.Compiler("C:/VS/cl.exe", "msvc")
    assert cl.name == "cl" and "/WX" in cl.flags() and "/fp:precise" in cl.flags()
    assert cl.command(Path("u.c"), Path("u.obj"), [])[-2:] == ["u.c", "/Fou.obj"]
    assert "/clang:-ffp-contract=off" in coverage.Compiler("clang-cl", "clang-cl").flags()
    assert coverage._kind("C:/LLVM/bin/clang-cl.exe") == "clang-cl"
    assert coverage._kind("cl") == "msvc" and coverage._kind("gcc-13") == "gnu"


def _source(ptr: int, body: str, extra: str = "") -> Lifted:
    text = f"{extra}void {c_name(ptr)}(mt_cpu *MT_RESTRICT cpu)\n{{\n{body}}}\n"
    return Lifted(ptr, c_name(ptr), text)


def test_compile_errors_are_charged_to_functions(tmp_path) -> None:
    compiler = coverage.find_compiler()
    if compiler is None:
        pytest.skip("no C compiler")
    span = (0x1000, 0x1400)  # 4 units of 0x100 bytes
    good = [_source(0x1001, "    (void)cpu;\n"), _source(0x1101, "    cpu->r[0] = 1u;\n")]
    broken = _source(0x1121, "    cpu->r[0] = mt_no_such_name;\n")
    # Each compiles alone; together they define one object twice.
    twins = [_source(0x1201, "    (void)cpu;\n", "int mt_twin = 1;\n"),
             _source(0x1221, "    (void)cpu;\n", "int mt_twin = 1;\n")]  # fmt: skip
    lifted = [*good, broken, *twins]
    gen = tmp_path / "gen"
    imports = ImportTable.from_imports([])
    sources = write_program(gen, lifted, imports, 4, span)
    cells = coverage.unit_functions(lifted, 4, span)
    assert sorted(cells) == ["fn_000.c", "fn_001.c", "fn_002.c"]
    result = coverage.compile_program(gen, sources, cells, compiler, jobs=2)
    assert result.measured and result.units == 5  # 3 cells, imports.c, dispatch.c
    assert result.compiled == {0x1001, 0x1101}
    assert result.failed_units == ["fn_001.c", "fn_002.c"]
    by_ptr = {f.ptr: f for f in result.failures}
    assert sorted(by_ptr) == [0x1121, 0x1201, 0x1221]
    assert by_ptr[0x1121].reason.startswith("f_00001121.c")
    assert "mt_no_such_name" in by_ptr[0x1121].reason
    assert str(tmp_path) not in by_ptr[0x1121].reason
    assert by_ptr[0x1201].reason.startswith("fn_002.c fails although each of its functions")
    # A broken dispatch.c is a program problem; the functions still compile.
    (gen / "dispatch.c").write_text("#error broken table\n", encoding="utf-8")
    result = coverage.compile_program(gen, sources, cells, compiler, jobs=2)
    program = [f for f in result.failures if f.ptr is None]
    assert len(program) == 1 and program[0].reason.startswith("dispatch.c: ")
    assert "broken table" in program[0].reason
    assert result.compiled == {0x1001, 0x1101}
