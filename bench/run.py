#!/usr/bin/env python3
"""Runs the 7-library JSON benchmark executables of bench/CMakeLists.txt and prints/saves results.

1. Each executable runs with --verify. Every library must agree on the visited stats of every file,
   and serialization / built-copy round-trips must match (build_round_trip may be null for read-only DOMs).
2. The executables then run in alternating rounds so machine frequency or thermal shifts hit all libraries
   proportionally. For each round, ratios are computed relative to --reference in the same round.

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
ORDER = [
    "bench_yyjson",
    "bench_neojson",
    "bench_simdjson",
    "bench_rapidjson",
    "bench_cppyyjson",
    "bench_nlohmann_json",
    "bench_nlohmann_ordered",
]
WORKLOADS = ["parse", "read", "write", "build", "lookup"]


def find_executables(build_dirs):
    found = {}
    for d in build_dirs:
        for name in ORDER:
            for candidate in (Path(d) / name, Path(d) / (name + ".exe")):
                if candidate.is_file() and name not in found:
                    found[name] = candidate
    return [(name, found[name]) for name in ORDER if name in found]


def corpus_manifest():
    return json.loads((BENCH_DIR / "corpus.json").read_text())


def file_arguments(build_dirs):
    manifest = corpus_manifest()
    args = []
    file_sizes = {}
    for entry in manifest["files"]:
        path = next(
            (Path(d) / "corpus" / entry["name"] for d in build_dirs if (Path(d) / "corpus" / entry["name"]).is_file()),
            None,
        )
        if path is None:
            sys.exit(f"run.py: {entry['name']} is missing from every build directory's corpus folder")
        file_sizes[entry["name"]] = path.stat().st_size
        args += ["--file", entry["name"], str(path)]
        if "lookup" in entry:
            args += ["--lookup", *entry["lookup"]]
    return args, file_sizes


def run_json_lines(exe, args):
    proc = subprocess.run([str(exe), *args], capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"run.py: {exe.name} exited with {proc.returncode}: {proc.stderr.strip()}")
    return [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]


def close(a, b):
    scale = max(1.0, abs(a), abs(b))
    return abs(a - b) <= 1e-9 * scale


def verify(executables, files, reference_name):
    results = {name: {r["file"]: r for r in run_json_lines(exe, [*files, "--verify"])} for name, exe in executables}
    reference = results[reference_name]
    problems = []
    for name, by_file in results.items():
        for file, r in by_file.items():
            if not r["write_round_trip"]:
                problems.append(f"{name} {file}: serialization does not parse back cleanly")
            if r.get("build_round_trip") is False:
                problems.append(f"{name} {file}: built copy does not parse back cleanly")
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
    lines = [f"os: {platform.system()} {platform.release()}", f"machine: {platform.machine()}"]
    try:
        if sys.platform == "darwin":
            cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout
            if cpu.strip():
                lines.append(f"cpu: {cpu.strip()}")
        elif Path("/proc/cpuinfo").is_file():
            for line in Path("/proc/cpuinfo").read_text().splitlines():
                if line.lower().startswith("model name"):
                    lines.append("cpu: " + line.split(":", 1)[1].strip())
                    break
            else:
                out = subprocess.run(["lscpu"], capture_output=True, text=True).stdout
                for line in out.splitlines():
                    if line.lower().startswith("model name:"):
                        lines.append("cpu: " + line.split(":", 1)[1].strip())
                        break
    except OSError:
        pass
    lines.append(f"logical cpus: {os.cpu_count()}")
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", action="append", required=True, help="a build directory (repeatable)")
    ap.add_argument("--rounds", type=int, default=7)
    ap.add_argument("--batch-ms", default="25")
    ap.add_argument("--batches", default="7")
    ap.add_argument("--reference", default="bench_yyjson")
    ap.add_argument("--json", help="write every measurement here")
    ap.add_argument("--markdown", help="write the table here as well")
    opts = ap.parse_args()

    if opts.rounds < 1:
        sys.exit("run.py: --rounds must be >= 1")
    executables = find_executables(opts.build)
    if not any(name == opts.reference for name, _ in executables):
        sys.exit(f"run.py: the reference {opts.reference} was not built")
    files, file_sizes = file_arguments(opts.build)
    verify(executables, files, opts.reference)
    print(f"verified: {', '.join(name for name, _ in executables)} agree on every file", file=sys.stderr)

    times = {}  # (lib, file, workload) -> [ns per round]
    for round_index in range(opts.rounds):
        for name, exe in executables:
            for r in run_json_lines(exe, [*files, "--batch-ms", opts.batch_ms, "--batches", opts.batches]):
                times.setdefault((name, r["file"], r["workload"]), []).append(r["ns"])
        print(f"round {round_index + 1}/{opts.rounds} done", file=sys.stderr)

    file_names = [entry["name"] for entry in corpus_manifest()["files"]]
    lib_names = [name for name, _ in executables]
    out = ["# JSON benchmark", ""] + [f"- {line}" for line in machine_description()]
    out += [
        f"- rounds: {opts.rounds}; each cell: median of round ratios vs {opts.reference} (median latency)",
        "",
    ]
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
                    cells.append("—")
                    continue
                ratio = statistics.median(a / b for a, b in zip(t, ref))
                cells.append(f"{ratio:.2f}x ({statistics.median(t) / 1e3:.0f} us)")
                rows.append({"workload": workload, "file": file, "lib": lib, "ratio": ratio, "ns": t})
            out.append(f"| {workload} | {file} | " + " | ".join(cells) + " |")
    header = [
        "| workload | file | " + " | ".join(lib_names) + " |",
        "|---|---|" + "---|" * len(lib_names),
    ]
    first_row = next(i for i, line in enumerate(out) if line.startswith("| "))
    out[first_row:first_row] = header
    text = "\n".join(out) + "\n"
    print(text)
    if opts.markdown:
        Path(opts.markdown).write_text(text)
    if opts.json:
        Path(opts.json).write_text(
            json.dumps(
                {
                    "machine": machine_description(),
                    "rounds": opts.rounds,
                    "reference": opts.reference,
                    "files": file_sizes,
                    "rows": rows,
                },
                indent=1,
            )
        )


if __name__ == "__main__":
    main()
