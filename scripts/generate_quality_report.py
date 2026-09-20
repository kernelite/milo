#!/usr/bin/env python3
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPORTS_DIR = Path("build/reports")
REPORTS_DIR.mkdir(parents=True, exist_ok=True)

HTML_REPORT = REPORTS_DIR / "quality_report.html"
MD_REPORT = REPORTS_DIR / "quality_report.md"

def get_cpp_files():
    sources = []
    for root, _, files in os.walk("."):
        if any(ignored in root for ignored in ["build", ".git", ".devcontainer", ".vscode"]):
            continue
        for file in files:
            if file.endswith((".cpp", ".hpp", ".c", ".h")):
                sources.append(os.path.join(root, file))
    return sources

def check_clang_format(files):
    print(" Running clang-format check...")
    issues = []
    for file in files:
        res = subprocess.run(
            ["clang-format", "--dry-run", "--Werror", file],
            capture_output=True,
            text=True
        )
        if res.returncode != 0:
            issues.append(file)
    return issues

def run_clang_tidy(files):
    print(" Running clang-tidy analysis...")
    cmd = ["clang-tidy", "-p", "."] + files
    res = subprocess.run(cmd, capture_output=True, text=True)
    
    diagnostics = []
    pattern = re.compile(r"^(.*?):(\d+):(\d+):\s+(warning|error):\s+(.*?)\s+\[(.*?)\]$")
    
    for line in res.stdout.splitlines():
        match = pattern.match(line)
        if match:
            diagnostics.append({
                "file": match.group(1),
                "line": match.group(2),
                "col": match.group(3),
                "severity": match.group(4),
                "message": match.group(5),
                "check": match.group(6)
            })
    return diagnostics

def generate_reports(format_issues, tidy_issues):
    # 1. Markdown Report
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
                f"| `{issue['file']}` | {issue['line']} | **{issue['severity']}** | {issue['message']} | `{issue['check']}` |"
            )
    else:
        md_content.append(" No static analysis issues found!")

    MD_REPORT.write_text("\n".join(md_content))

    # 2. HTML Report
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
            {"".join(f"<tr><td><code>{i['file']}</code></td><td>{i['line']}</td><td><span class='{i['severity']}'>{i['severity']}</span></td><td>{i['message']}</td><td><code>{i['check']}</code></td></tr>" for i in tidy_issues) if tidy_issues else "<tr><td colspan='5'>Clean report! Zero issues found.</td></tr>"}
        </table>
    </div>
</body>
</html>"""

    HTML_REPORT.write_text(html_content)
    print(f"\n Reports generated:\n  - {HTML_REPORT}\n  - {MD_REPORT}")

def main():
    files = get_cpp_files()
    format_issues = check_clang_format(files)
    tidy_issues = run_clang_tidy(files)
    generate_reports(format_issues, tidy_issues)

    if format_issues or tidy_issues:
        sys.exit(1)

if __name__ == "__main__":
    main()