"""Headless Ghidra driver for the MapleThief RE project (ADR 0007).

Run it through the wrappers in tools/ghidra/ (they set up Ghidra, the JDK and PyGhidra
first); see tools/ghidra/README.md. Commands::

    import       [--slice PATH] [--names-dir DIR] [--out DIR]
                 delete and re-create work/ghidra/MapleThief.gpr from the slice, seed the
                 LC_FUNCTION_STARTS functions (scripts/seed_functions.py), auto-analyse,
                 apply the names files (scripts/apply_names.py) and export (scripts/export.py)
    reexport     [--out DIR]                 re-export from the existing project
    apply-names  [--names-dir DIR] [--no-export]
                 apply work/re/names/*.json to the existing project, save, then re-export
    run-script   SCRIPT [ARGS...]            run any Ghidra script on the project and save

Every path defaults to its place in this checkout. The project lives in work/ghidra/, which
is git-ignored; so are the exports (functions.json, xrefs.json, strings.json there).
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPTS = Path(__file__).resolve().parent / "scripts"
sys.path.insert(0, str(SCRIPTS))
import mtport_loader  # noqa: E402,F401  (loads mtport without tools/ on sys.path)

EXPORT_DIR = REPO / "work" / "ghidra"
PROJECT_NAME = "MapleThief"
PROGRAM_NAME = "MapleThief.armv7"
SLICE = REPO / "work" / "slices" / "MapleThief.armv7"
NAMES_DIR = REPO / "work" / "re" / "names"
# The slice is cputype 12 / cpusubtype 9 (armv7). Ghidra's ARM.opinion has no entry for
# subtype 9, so pin the language that matches the CPU (ARMv7-A: Thumb-2, VFPv3, NEON).
LANGUAGE = "ARM:LE:32:v7"
COMPILER = "default"
# Auto-analysis options that differ from Ghidra's defaults (ADR 0007). The Objective-C
# Message Analyzer decompiles every function in parallel to turn objc_msgSend calls into
# call-override references; on this binary one decompile throws
# (ArrayIndexOutOfBoundsException in findMessages), which aborts the batch at a different
# point on every run, so the exports were not reproducible. ObjC calls are resolved from
# the metadata instead (#8, #12).
ANALYSIS_OPTIONS = {"Objective-C Message Analyzer": False}


def log(msg: str) -> None:
    print(f"[ghidra] {msg}", flush=True)


def _dotted(path: Path) -> bool:
    return any(part.startswith(".") for part in path.parts)


def project_dir() -> Path:
    """Where MapleThief.gpr lives: $MAPLETHIEF_GHIDRA_PROJECT_DIR, else work/ghidra/.

    Ghidra refuses a project path with a component that starts with "." (ProjectLocator),
    and every agent worktree has one (.claude/worktrees/<name>/). In a linked worktree the
    project therefore goes to the main checkout's build/ghidra-projects/<name>/; the exports
    still go to this checkout's work/ghidra/."""
    env = os.environ.get("MAPLETHIEF_GHIDRA_PROJECT_DIR")
    path = Path(env).resolve() if env else EXPORT_DIR
    dot_git = REPO / ".git"
    if not env and _dotted(path) and dot_git.is_file():
        # A linked worktree: ".git" is "gitdir: <main>/.git/worktrees/<name>".
        text = dot_git.read_text(encoding="utf-8").strip()
        if text.startswith("gitdir:"):
            gitdir = Path(text[len("gitdir:") :].strip())
            if gitdir.parent.name == "worktrees" and gitdir.parent.parent.name == ".git":
                main = gitdir.parent.parent.parent
                path = main / "build" / "ghidra-projects" / REPO.name
    if _dotted(path):
        raise SystemExit(
            f"error: Ghidra can't use a project path with a '.' component ({path}); "
            "set MAPLETHIEF_GHIDRA_PROJECT_DIR to another directory"
        )
    return path


PROJECT_DIR = project_dir()


def start_ghidra() -> None:
    import pyghidra

    launcher = pyghidra.HeadlessPyGhidraLauncher()
    tmp = os.environ.get("MAPLETHIEF_GHIDRA_TMP")
    if tmp:
        Path(tmp).mkdir(parents=True, exist_ok=True)
        launcher.add_vmargs(f"-Djava.io.tmpdir={tmp}")
    launcher.start()


def open_project(create: bool = False):
    import pyghidra

    if not create and not (PROJECT_DIR / f"{PROJECT_NAME}.gpr").is_file():
        raise SystemExit(f"error: no project at {PROJECT_DIR}; run tools/ghidra/import first")
    PROJECT_DIR.mkdir(parents=True, exist_ok=True)
    return pyghidra.open_project(str(PROJECT_DIR), PROJECT_NAME, create=create)


def run_script(project, program, name: str | Path, args: list[str]) -> None:
    import pyghidra

    path = Path(name)
    if not path.is_absolute() and not path.exists():
        path = SCRIPTS / path
    args = [str(a) for a in args]
    log(f"script {path.name} {' '.join(args)}".rstrip())
    pyghidra.ghidra_script(str(path), project, program, args)


def delete_project() -> None:
    for path in (PROJECT_DIR / f"{PROJECT_NAME}.gpr", PROJECT_DIR / f"{PROJECT_NAME}.rep"):
        if path.is_dir():
            shutil.rmtree(path)
        elif path.exists():
            path.unlink()
    for lock in PROJECT_DIR.glob(f"{PROJECT_NAME}.lock*"):
        lock.unlink()


def cmd_import(args) -> None:
    import pyghidra
    from ghidra.app.util.opinion import MachoLoader
    from ghidra.program.util import GhidraProgramUtilities
    from java.lang import Object

    slice_path = Path(args.slice).resolve()
    if not slice_path.is_file():
        raise SystemExit(f"error: {slice_path} not found; run scripts/bootstrap first")
    delete_project()
    project = open_project(create=True)
    consumer = Object()
    t0 = time.monotonic()
    loader = (
        pyghidra.program_loader()
        .project(project)
        .source(str(slice_path))
        .name(PROGRAM_NAME)
        .projectFolderPath("/")
        .loaders(MachoLoader)
        .language(LANGUAGE)
        .compiler(COMPILER)
    )
    with loader.load() as results:
        results.getPrimary().save(pyghidra.task_monitor())
        program = results.getPrimaryDomainObject(consumer)
    try:
        log(
            f"imported {slice_path.name} as {program.getLanguageID()} "
            f"in {time.monotonic() - t0:.0f} s"
        )
        # Options first: seeding already schedules analyzers, with whatever is enabled then.
        with pyghidra.transaction(program, "analysis options"):
            options = pyghidra.analysis_properties(program)
            for name, value in ANALYSIS_OPTIONS.items():
                options.setBoolean(name, value)
        run_script(project, program, "seed_functions.py", [slice_path])
        t1 = time.monotonic()
        log("auto-analysis ...")
        analysis_log = pyghidra.analyze(program)
        GhidraProgramUtilities.markProgramAnalyzed(program)
        log(
            f"auto-analysis done in {time.monotonic() - t1:.0f} s "
            f"({program.getFunctionManager().getFunctionCount()} functions)"
        )
        Path(args.out).mkdir(parents=True, exist_ok=True)
        (Path(args.out) / "analysis.log").write_text(str(analysis_log), encoding="utf-8")
        run_script(project, program, "apply_names.py", [args.names_dir])
        program.save("import + analysis + names", pyghidra.task_monitor())
        if not args.no_export:
            run_script(project, program, "export.py", [args.out, slice_path, args.names_dir])
    finally:
        program.release(consumer)
        project.close()
    log(f"done in {time.monotonic() - t0:.0f} s")


def with_program(fn):
    import pyghidra

    project = open_project()
    try:
        with pyghidra.program_context(project, f"/{PROGRAM_NAME}") as program:
            fn(project, program)
    finally:
        project.close()


def cmd_export(args) -> None:
    def go(project, program):
        run_script(project, program, "export.py", [args.out, args.slice, args.names_dir])

    with_program(go)


def cmd_apply_names(args) -> None:
    import pyghidra

    def go(project, program):
        run_script(project, program, "apply_names.py", [args.names_dir])
        program.save("apply names", pyghidra.task_monitor())
        if not args.no_export:
            run_script(project, program, "export.py", [args.out, args.slice, args.names_dir])

    with_program(go)


def cmd_run_script(args) -> None:
    import pyghidra

    script = Path(args.script)
    if not script.exists() and not (SCRIPTS / script).exists():
        raise SystemExit(f"error: no script {script} (nor {SCRIPTS / script})")

    def go(project, program):
        run_script(project, program, script.resolve() if script.exists() else script, args.args)
        if program.isChanged():
            program.save(f"run-script {script.name}", pyghidra.task_monitor())

    with_program(go)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="tools/ghidra/driver.py", description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    def common(p, export=True):
        p.add_argument("--slice", default=str(SLICE), help="armv7 slice (default: %(default)s)")
        p.add_argument("--names-dir", default=str(NAMES_DIR), help="default: %(default)s")
        if export:
            p.add_argument("--out", default=str(EXPORT_DIR), help="export dir (%(default)s)")
        return p

    p = common(sub.add_parser("import", help="clean import + analysis + names + export"))
    p.add_argument("--no-export", action="store_true")
    p.set_defaults(fn=cmd_import)
    common(sub.add_parser("reexport", help="re-export the existing project")).set_defaults(
        fn=cmd_export
    )
    p = common(sub.add_parser("apply-names", help="apply names files, save, re-export"))
    p.add_argument("--no-export", action="store_true")
    p.set_defaults(fn=cmd_apply_names)
    p = sub.add_parser("run-script", help="run a Ghidra script on the project and save")
    p.add_argument("script", help="path, or a name in tools/ghidra/scripts/")
    p.add_argument("args", nargs=argparse.REMAINDER)
    p.set_defaults(fn=cmd_run_script)

    args = parser.parse_args(argv)
    if PROJECT_DIR != EXPORT_DIR:
        log(f"project directory: {PROJECT_DIR}")
    start_ghidra()
    args.fn(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
