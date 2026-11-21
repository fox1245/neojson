#!/usr/bin/env python3
"""Runs the benchmark executables of bench/CMakeLists.txt and prints a table.

1. Each executable runs with --verify. Every library must find the same values in every file (the stats of
   bench.cpp), and its serialization and its built copy must parse back to the same values. Any difference stops the
   run.
2. The executables then run in alternating rounds (A B C D, A B C D, ...), so that a change of the machine's speed
   during the job hits every library alike. For every round the time of a library is divided by the time of the
   reference library in the same round; the table shows the median of these ratios and the median time.

Usage: run.py --build DIR [--build DIR ...] [--rounds N] [--reference NAME] [--json OUT] [--markdown OUT]
"""

import argparse
import json
import os
import platform
import statistics
import subprocess
import sys
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent
ORDER = ["bench_yyjson", "bench_cppyyjson_bundled", "bench_cppyyjson_same_yyjson", "bench_neojson", "bench_nlohmann"]
WORKLOADS = ["parse", "read", "write", "build", "lookup"]


def find_executables(build_dirs):
    found = {}
    for d in build_dirs:
        for name in ORDER:
            for candidate in (Path(d) / name, Path(d) / (name + ".exe")):
                if candidate.is_file() and name not in found:
                    found[name] = candidate
    return [(name, found[name]) for name in ORDER if name in found]


def file_arguments(build_dirs):
    corpus = json.loads((BENCH_DIR / "corpus.json").read_text())
    args = []
    for entry in corpus["files"]:
        path = next((Path(d) / "corpus" / entry["name"] for d in build_dirs
                     if (Path(d) / "corpus" / entry["name"]).is_file()), None)
        if path is None:
            sys.exit(f"run.py: {entry['name']} is missing from every build directory's corpus folder")
        args += ["--file", entry["name"], str(path)]
        if "lookup" in entry:
            args += ["--lookup", *entry["lookup"]]
    return args


def run_json_lines(exe, args):
    proc = subprocess.run([str(exe), *args], capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"run.py: {exe.name} exited with {proc.returncode}: {proc.stderr.strip()}")
    return [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]


def close(a, b):
    scale = max(1.0, abs(a), abs(b))
    return abs(a - b) <= 1e-9 * scale


def verify(executables, files):
    results = {name: {r["file"]: r for r in run_json_lines(exe, [*files, "--verify"])} for name, exe in executables}
    reference_name = executables[0][0]
    reference = results[reference_name]
    problems = []
    for name, by_file in results.items():
        for file, r in by_file.items():
            if not r["write_round_trip"]:
                problems.append(f"{name} {file}: the serialization does not parse back to the same values")
            if not r["build_round_trip"]:
                problems.append(f"{name} {file}: the built copy does not parse back to the same values")
            ref = reference[file]
            for key, value in r["stats"].items():
                ok = close(value, ref["stats"][key]) if key == "real_sum" else value == ref["stats"][key]
                if not ok:
                    problems.append(f"{name} {file}: {key} = {value}, {reference_name} finds {ref['stats'][key]}")
            if r.get("lookup") != ref.get("lookup"):
                problems.append(f"{name} {file}: lookup = {r.get('lookup')}, {reference_name} finds {ref.get('lookup')}")
    if problems:
        sys.exit("run.py: the libraries disagree:\n  " + "\n  ".join(problems))
    return reference


def machine_description():
    lines = [f"platform: {platform.platform()}", f"machine: {platform.machine()}"]
    try:
        if sys.platform == "darwin":
            cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout
            lines.append(f"cpu: {cpu.strip()}")
        elif Path("/proc/cpuinfo").is_file():
            for line in Path("/proc/cpuinfo").read_text().splitlines():
                if line.lower().startswith(("model name", "cpu part")):
                    lines.append("cpu: " + line.split(":", 1)[1].strip())
                    break
    except OSError:
        pass
    lines.append(f"logical cpus: {os.cpu_count()}")
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", action="append", required=True, help="a build directory (repeatable)")
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--batch-ms", default="20")
    ap.add_argument("--batches", default="7")
    ap.add_argument("--reference", default="bench_yyjson")
    ap.add_argument("--json", help="write every measurement here")
    ap.add_argument("--markdown", help="write the table here as well")
    opts = ap.parse_args()

    executables = find_executables(opts.build)
    if not any(name == opts.reference for name, _ in executables):
        sys.exit(f"run.py: the reference {opts.reference} was not built")
    files = file_arguments(opts.build)
    verify(executables, files)
    print(f"verified: {', '.join(name for name, _ in executables)} agree on every file", file=sys.stderr)

    times = {}  # (lib, file, workload) -> [ns per round]
    for round_index in range(opts.rounds):
        for name, exe in executables:
            for r in run_json_lines(exe, [*files, "--batch-ms", opts.batch_ms, "--batches", opts.batches]):
                times.setdefault((name, r["file"], r["workload"]), []).append(r["ns"])
        print(f"round {round_index + 1}/{opts.rounds} done", file=sys.stderr)

    file_names = [json.loads((BENCH_DIR / "corpus.json").read_text())["files"][i]["name"]
                  for i in range(len(json.loads((BENCH_DIR / "corpus.json").read_text())["files"]))]
    lib_names = [name for name, _ in executables]
    out = ["# JSON benchmark", ""] + [f"- {line}" for line in machine_description()]
    out += [f"- rounds: {opts.rounds}; each cell: the median over the rounds of (time / time of "
            f"{opts.reference} in the same round), and the median time", ""]
    rows = []
    for workload in WORKLOADS:
        for file in file_names:
            if (opts.reference, file, workload) not in times:
                continue
            ref = times[(opts.reference, file, workload)]
            cells = []
            for lib in lib_names:
                t = times.get((lib, file, workload))
                if not t:
                    cells.append("")
                    continue
                ratio = statistics.median(a / b for a, b in zip(t, ref))
                cells.append(f"{ratio:.2f}x ({statistics.median(t) / 1e3:.0f} us)")
                rows.append({"workload": workload, "file": file, "lib": lib, "ratio": ratio, "ns": t})
            out.append(f"| {workload} | {file} | " + " | ".join(cells) + " |")
    header = ["| workload | file | " + " | ".join(lib_names) + " |",
              "|---|---|" + "---|" * len(lib_names)]
    first_row = next(i for i, line in enumerate(out) if line.startswith("| "))
    out[first_row:first_row] = header
    text = "\n".join(out) + "\n"
    print(text)
    if opts.markdown:
        Path(opts.markdown).write_text(text)
    if opts.json:
        Path(opts.json).write_text(json.dumps({"machine": machine_description(), "rows": rows}, indent=1))


if __name__ == "__main__":
    main()
