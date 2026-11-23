#!/usr/bin/env python3
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil

DEP_FLAGS = {"-MMD", "-MP", "-MD"}
DEP_ARG_FLAGS = {"-MF", "-MT", "-MQ"}


def filter_flags(flags_str: str) -> list[str]:
    tokens = shlex.split(flags_str)
    filtered: list[str] = []
    skip_next = False
    for tok in tokens:
        if skip_next:
            skip_next = False
            continue
        if tok in DEP_FLAGS:
            continue
        if tok in DEP_ARG_FLAGS:
            skip_next = True
            continue
        filtered.append(tok)
    return filtered


def resolve_compiler(compiler: str) -> str:
    resolved = shutil.which(compiler)
    return resolved if resolved else compiler


def make_entry(
    root: Path,
    compiler: str,
    flags: list[str],
    includes: list[str],
    build_dir: str,
    src: str,
) -> dict:
    src_path = Path(src)
    obj_rel = Path(build_dir) / src_path.with_suffix(".o")
    abs_src = str((root / src_path).resolve())
    abs_obj = str((root / obj_rel).resolve())
    args = [
        compiler,
        *flags,
        *includes,
        "-c",
        "-o",
        str(obj_rel),
        src,
    ]
    return {
        "arguments": args,
        "directory": str(root),
        "file": abs_src,
        "output": abs_obj,
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate compile_commands.json incrementally without full rebuilds."
    )
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--cxxflags", required=True)
    parser.add_argument("--kernel-includes", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--kernel-srcs", nargs="*", default=[])
    parser.add_argument("--host-cxx", required=True)
    parser.add_argument("--host-cxxflags", required=True)
    parser.add_argument("--host-includes", required=True)
    parser.add_argument("--host-build-dir", required=True)
    parser.add_argument("--host-srcs", nargs="*", default=[])
    parser.add_argument("--output", default="compile_commands.json")
    args = parser.parse_args()

    root = Path.cwd().resolve()
    kernel_cc = resolve_compiler(args.cxx)
    host_cc = resolve_compiler(args.host_cxx)

    kernel_flags = filter_flags(args.cxxflags)
    kernel_inc = shlex.split(args.kernel_includes)
    host_flags = filter_flags(args.host_cxxflags)
    host_inc = shlex.split(args.host_includes)

    entries: list[dict] = []
    seen_files: set[str] = set()

    for src in args.kernel_srcs:
        if not src or src in seen_files:
            continue
        seen_files.add(src)
        entries.append(
            make_entry(root, kernel_cc, kernel_flags, kernel_inc, args.build_dir, src)
        )

    for src in args.host_srcs:
        if not src or src in seen_files:
            continue
        seen_files.add(src)
        entries.append(
            make_entry(root, host_cc, host_flags, host_inc, args.host_build_dir, src)
        )

    rendered = json.dumps(entries, indent=2) + "\n"
    out_path = Path(args.output)
    if out_path.exists() and out_path.read_text(encoding="utf-8") == rendered:
        return

    tmp_path = out_path.with_suffix(".json.tmp")
    tmp_path.write_text(rendered, encoding="utf-8")
    os.replace(tmp_path, out_path)


if __name__ == "__main__":
    main()