#!/usr/bin/env python3
import argparse
from concurrent.futures import ProcessPoolExecutor
import html
import json
import os
from pathlib import Path
import re
import subprocess
import sys

REPORTS_DIR = Path("build/reports")
HTML_REPORT = REPORTS_DIR / "quality_report.html"
MD_REPORT = REPORTS_DIR / "quality_report.md"

IGNORED_DIRS = {"build", ".git", ".devcontainer", ".vscode", "third_party"}
CPP_EXTS = {".cpp", ".hpp", ".c", ".h"}
HEADER_EXTS = {".hpp", ".h"}
TU_EXTS = {".cpp", ".c"}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')
DIAG_RE = re.compile(r"^(.*?):(\d+):(\d+):\s+(warning|error):\s+(.*?)\s+\[(.*?)\]$")


def norm_rel(path_str: str, root: Path) -> str:
    path = Path(path_str)
    if path.is_absolute():
        try:
            path = path.relative_to(root)
        except ValueError:
            return str(path)
    return path.as_posix().lstrip("./")


def get_all_cpp_files(root: Path) -> list[str]:
    sources: list[str] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in IGNORED_DIRS]
        for filename in filenames:
            if Path(filename).suffix in CPP_EXTS:
                rel = norm_rel(os.path.join(dirpath, filename), root)
                sources.append(rel)
    sources.sort()
    return sources


def load_compile_db_tus(root: Path) -> list[str]:
    db_path = root / "compile_commands.json"
    if not db_path.exists():
        return []
    try:
        entries = json.loads(db_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []

    tus: list[str] = []
    seen: set[str] = set()
    for entry in entries:
        file_val = entry.get("file", "")
        if not file_val:
            continue
        rel = norm_rel(file_val, root)
        if rel not in seen and (root / rel).exists():
            seen.add(rel)
            tus.append(rel)
    return tus


def build_reverse_include_map(root: Path, all_files: list[str]) -> dict[str, set[str]]:
    include_dirs = [
        root,
        root / "kernel",
        root / "arch" / "aarch64-linux-gnu",
        root / "tests" / "host",
    ]
    direct_includes: dict[str, set[str]] = {}

    for rel in all_files:
        file_path = root / rel
        if not file_path.exists():
            continue
        deps: set[str] = set()
        try:
            content = file_path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for line in content.splitlines():
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            inc = match.group(1)
            candidates = [file_path.parent / inc] + [d / inc for d in include_dirs]
            for cand in candidates:
                if cand.exists():
                    deps.add(norm_rel(str(cand.resolve()), root))
                    break
        direct_includes[rel] = deps

    build_dir = root / "build"
    if build_dir.exists():
        for dep_file in build_dir.rglob("*.d"):
            try:
                raw = dep_file.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            joined = raw.replace("\\\n", " ")
            if ":" not in joined:
                continue
            _, rhs = joined.split(":", 1)
            tokens = rhs.split()
            if not tokens:
                continue
            tu_rel = norm_rel(tokens[0], root)
            if Path(tu_rel).suffix in TU_EXTS:
                tu_deps = direct_includes.setdefault(tu_rel, set())
                for tok in tokens[1:]:
                    if Path(tok).suffix in HEADER_EXTS:
                        tu_deps.add(norm_rel(tok, root))

    reverse_map: dict[str, set[str]] = {}
    for src, deps in direct_includes.items():
        for dep in deps:
            reverse_map.setdefault(dep, set()).add(src)
    return reverse_map


def resolve_target_tus(
    root: Path,
    requested_files: list[str] | None,
    registered_tus: list[str],
    all_files: list[str],
) -> list[str]:
    if not requested_files:
        if registered_tus:
            return registered_tus
        return [f for f in all_files if Path(f).suffix == ".cpp"]

    registered_set = set(registered_tus)
    target_tus: set[str] = set()
    headers: list[str] = []

    for raw in requested_files:
        rel = norm_rel(raw, root)
        suffix = Path(rel).suffix
        if suffix in HEADER_EXTS:
            headers.append(rel)
        elif suffix in TU_EXTS:
            if not registered_set or rel in registered_set:
                target_tus.add(rel)

    if headers:
        reverse_map = build_reverse_include_map(root, all_files)
        visited: set[str] = set()
        stack = list(headers)
        while stack:
            hdr = stack.pop()
            if hdr in visited:
                continue
            visited.add(hdr)
            for dependent in reverse_map.get(hdr, ()):
                dep_suffix = Path(dependent).suffix
                if dep_suffix in HEADER_EXTS and dependent not in visited:
                    stack.append(dependent)
                elif dep_suffix in TU_EXTS:
                    if not registered_set or dependent in registered_set:
                        target_tus.add(dependent)

    return sorted(target_tus)


def check_clang_format(files: list[str]) -> list[str]:
    if not files:
        return []
    print(f" Running clang-format check on {len(files)} file(s)...")
    batch_res = subprocess.run(
        ["clang-format", "--dry-run", "--Werror", *files],
        capture_output=True,
        text=True,
    )
    if batch_res.returncode == 0:
        return []

    issues: list[str] = []
    for file_path in files:
        res = subprocess.run(
            ["clang-format", "--dry-run", "--Werror", file_path],
            capture_output=True,
            text=True,
        )
        if res.returncode != 0:
            issues.append(file_path)
    return issues


def lint_single_tu(tu_path: str) -> list[dict]:
    cmd = ["clang-tidy", "-extra-arg=-Wno-reserved-identifier", "-p", ".", tu_path]
    res = subprocess.run(cmd, capture_output=True, text=True)
    diagnostics: list[dict] = []

    for line in res.stdout.splitlines():
        match = DIAG_RE.match(line)
        if not match:
            continue
        check = match.group(6)
        message = match.group(5)
        if (
            "reserved-identifier" in check
            or "reserved-identifier" in message
            or "__bss" in message
            or "_text" in message
        ):
            continue
        diagnostics.append(
            {
                "file": match.group(1),
                "line": match.group(2),
                "col": match.group(3),
                "severity": match.group(4),
                "message": message,
                "check": check,
            }
        )
    return diagnostics


def run_clang_tidy(tus: list[str], jobs: int) -> list[dict]:
    if not tus:
        print(" No C++ translation units to analyze.")
        return []

    workers = max(1, min(jobs, len(tus)))
    print(f" Running clang-tidy analysis on {len(tus)} translation unit(s) ({workers} worker(s))...")

    seen: set[tuple[str, str, str, str, str, str]] = set()
    merged: list[dict] = []

    with ProcessPoolExecutor(max_workers=workers) as pool:
        for tu_diags in pool.map(lint_single_tu, tus):
            for diag in tu_diags:
                key = (
                    diag["file"],
                    diag["line"],
                    diag["col"],
                    diag["severity"],
                    diag["message"],
                    diag["check"],
                )
                if key not in seen:
                    seen.add(key)
                    merged.append(diag)

    merged.sort(key=lambda d: (d["file"], int(d["line"]), int(d["col"])))
    return merged


def generate_reports(format_issues: list[str], tidy_issues: list[dict]) -> None:
    REPORTS_DIR.mkdir(parents=True, exist_ok=True)

    md_content = ["# Code Quality Analysis Report\n"]
    md_content.append(f"**Formatting Violations:** {len(format_issues)}")
    md_content.append(f"**Static Analysis Issues:** {len(tidy_issues)}\n")

    if format_issues:
        md_content.append("## Formatting Issues")
        for f in format_issues:
            md_content.append(f"- `{f}` needs formatting")
        md_content.append("")

    if tidy_issues:
        md_content.append("## Static Analysis (clang-tidy)")
        md_content.append("| File | Line | Severity | Message | Rule |")
        md_content.append("| --- | --- | --- | --- | --- |")
        for issue in tidy_issues:
            md_content.append(
                f"| `{issue['file']}` | {issue['line']} | **{issue['severity']}** | "
                f"{issue['message']} | `{issue['check']}` |"
            )
    else:
        md_content.append(" No static analysis issues found!")

    MD_REPORT.write_text("\n".join(md_content), encoding="utf-8")

    rows = (
        "".join(
            f"<tr><td><code>{html.escape(i['file'])}</code></td>"
            f"<td>{html.escape(i['line'])}</td>"
            f"<td><span class='{html.escape(i['severity'])}'>{html.escape(i['severity'])}</span></td>"
            f"<td>{html.escape(i['message'])}</td>"
            f"<td><code>{html.escape(i['check'])}</code></td></tr>"
            for i in tidy_issues
        )
        if tidy_issues
        else "<tr><td colspan='5'>Clean report! Zero issues found.</td></tr>"
    )

    html_content = f"""<!DOCTYPE html>
<html>
<head>
    <title>Kernel Quality Report</title>
    <style>
        body {{ font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; margin: 20px; background: #f8f9fa; }}
        h1, h2 {{ color: #212529; }}
        .card {{ background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-bottom: 20px; }}
        table {{ width: 100%; border-collapse: collapse; margin-top: 10px; }}
        th, td {{ text-align: left; padding: 10px; border-bottom: 1px solid #dee2e6; }}
        th {{ background: #e9ecef; }}
        .warning {{ color: #856404; background-color: #fff3cd; padding: 2px 6px; border-radius: 4px; font-weight: bold; }}
        .error {{ color: #721c24; background-color: #f8d7da; padding: 2px 6px; border-radius: 4px; font-weight: bold; }}
        code {{ background: #e9ecef; padding: 2px 4px; border-radius: 4px; font-size: 0.9em; }}
    </style>
</head>
<body>
    <h1>Microkernel Quality Report</h1>
    <div class="card">
        <h2>Summary</h2>
        <p><strong>Clang-Format Issues:</strong> {len(format_issues)}</p>
        <p><strong>Clang-Tidy Diagnostics:</strong> {len(tidy_issues)}</p>
    </div>
    <div class="card">
        <h2>Clang-Tidy Diagnostics</h2>
        <table>
            <tr><th>File</th><th>Line</th><th>Severity</th><th>Message</th><th>Rule</th></tr>
            {rows}
        </table>
    </div>
</body>
</html>"""

    HTML_REPORT.write_text(html_content, encoding="utf-8")
    print(f"\n Reports generated:\n  - {HTML_REPORT}\n  - {MD_REPORT}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run formatting and static analysis checks.")
    parser.add_argument(
        "files",
        nargs="*",
        help="Optional subset of C/C++ files to check (defaults to entire repository).",
    )
    parser.add_argument(
        "--staged",
        action="store_true",
        help="Treat passed files as staged commit targets; exit cleanly if list is empty.",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=os.cpu_count() or 4,
        help="Number of parallel clang-tidy workers.",
    )
    args = parser.parse_args()

    root = Path.cwd().resolve()
    if args.staged and not args.files:
        print(" No staged C/C++ files to check.")
        return

    all_files = get_all_cpp_files(root)
    registered_tus = load_compile_db_tus(root)

    if args.files:
        format_targets = sorted(
            {
                norm_rel(f, root)
                for f in args.files
                if Path(f).suffix in CPP_EXTS and (root / norm_rel(f, root)).exists()
            }
        )
        tidy_targets = resolve_target_tus(root, format_targets, registered_tus, all_files)
    else:
        format_targets = all_files
        tidy_targets = resolve_target_tus(root, None, registered_tus, all_files)

    format_issues = check_clang_format(format_targets)
    tidy_issues = run_clang_tidy(tidy_targets, args.jobs)
    generate_reports(format_issues, tidy_issues)

    if format_issues or tidy_issues:
        sys.exit(1)


if __name__ == "__main__":
    main()