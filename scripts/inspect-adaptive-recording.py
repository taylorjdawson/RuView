#!/usr/bin/env python3
"""Inspect count-labeled JSONL recordings for training readiness."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


LEGACY_FEATURE_WIDTH = 15
MULTI_NODE_FEATURE_WIDTH = 62
DEFAULT_MIN_FRAMES = 30
DEFAULT_MIN_NODE_FEATURE_RATIO = 0.80


COUNT_LABELS = ("count0", "count1", "count2")
COUNT_LABEL_PATTERN = re.compile(r"(^|[^a-z0-9])(count[0-2])([^a-z0-9]|$)")


@dataclass(frozen=True)
class InspectOptions:
    require_62: bool
    require_count_coverage: bool
    min_frames: int
    min_node_feature_ratio: float


def infer_label(path: Path) -> str | None:
    name = path.name.lower()
    match = COUNT_LABEL_PATTERN.search(name)
    if match:
        return match.group(2)
    return None


def expand_inputs(paths: list[Path]) -> tuple[list[Path], list[str]]:
    files: list[Path] = []
    errors: list[str] = []

    for path in paths:
        if path.is_dir():
            files.extend(
                sorted(p for p in path.iterdir() if p.is_file() and p.suffix == ".jsonl")
            )
        elif path.is_file():
            files.append(path)
        else:
            errors.append(f"{path}: path does not exist or is not a file/directory")

    return files, errors


def node_features_for_frame(frame: dict[str, Any]) -> list[Any]:
    node_features = frame.get("node_features")
    if isinstance(node_features, list):
        return node_features
    return []


def feature_width_for_frame(frame: dict[str, Any]) -> int:
    if node_features_for_frame(frame):
        return MULTI_NODE_FEATURE_WIDTH
    return LEGACY_FEATURE_WIDTH


def sorted_width_label(widths: set[int], valid_frames: int) -> str:
    if valid_frames == 0 or not widths:
        return "unknown"
    if len(widths) > 1:
        return "mixed"
    width = next(iter(widths))
    if width == LEGACY_FEATURE_WIDTH:
        return str(LEGACY_FEATURE_WIDTH)
    if width == MULTI_NODE_FEATURE_WIDTH:
        return str(MULTI_NODE_FEATURE_WIDTH)
    return "unknown"


def inspect_recording(path: Path, options: InspectOptions) -> dict[str, Any]:
    valid_frames = 0
    skipped_lines = 0
    parse_error_examples: list[dict[str, Any]] = []
    parse_errors = 0
    widths: set[int] = set()
    node_ids: set[int | str] = set()
    non_empty_node_feature_frames = 0
    node_counts: list[int] = []

    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        return {
            "path": str(path),
            "filename": path.name,
            "valid_frames": 0,
            "parse_errors": 0,
            "parse_error_examples": [],
            "skipped_lines": 0,
            "feature_width": "unknown",
            "feature_widths_seen": [],
            "node_ids_seen": [],
            "node_feature_frame_ratio": 0.0,
            "min_nodes_per_frame": 0,
            "max_nodes_per_frame": 0,
            "inferred_label": infer_label(path),
            "safe_to_train": False,
            "blocking_reasons": [f"read_error: {exc}"],
            "warnings": [],
        }

    for line_number, line in enumerate(lines, start=1):
        if not line.strip():
            skipped_lines += 1
            continue

        try:
            parsed = json.loads(line)
        except json.JSONDecodeError as exc:
            parse_errors += 1
            if len(parse_error_examples) < 5:
                parse_error_examples.append(
                    {
                        "line": line_number,
                        "message": exc.msg,
                        "column": exc.colno,
                    }
                )
            continue

        if not isinstance(parsed, dict):
            skipped_lines += 1
            continue

        valid_frames += 1
        nodes = node_features_for_frame(parsed)
        node_count = len(nodes)
        node_counts.append(node_count)
        if node_count > 0:
            non_empty_node_feature_frames += 1

        widths.add(feature_width_for_frame(parsed))
        for node in nodes:
            if isinstance(node, dict) and "node_id" in node:
                node_ids.add(node["node_id"])

    feature_width = sorted_width_label(widths, valid_frames)
    ratio = (
        non_empty_node_feature_frames / valid_frames
        if valid_frames > 0
        else 0.0
    )
    inferred_label = infer_label(path)
    warnings: list[str] = []
    blocking_reasons: list[str] = []

    if valid_frames == 0:
        blocking_reasons.append("zero_valid_frames")
    if valid_frames < options.min_frames:
        blocking_reasons.append(
            f"valid_frames_below_minimum: {valid_frames} < {options.min_frames}"
        )
    if parse_errors:
        blocking_reasons.append(f"parse_errors_present: {parse_errors}")
    if feature_width == "mixed":
        blocking_reasons.append("mixed_feature_widths")
    if options.require_62 and feature_width != str(MULTI_NODE_FEATURE_WIDTH):
        blocking_reasons.append(f"require_62_not_met: feature_width={feature_width}")
    if (
        feature_width == str(MULTI_NODE_FEATURE_WIDTH)
        and ratio < options.min_node_feature_ratio
    ):
        blocking_reasons.append(
            "node_feature_frame_ratio_below_minimum: "
            f"{ratio:.4f} < {options.min_node_feature_ratio:.4f}"
        )
    elif feature_width == str(LEGACY_FEATURE_WIDTH) and ratio < options.min_node_feature_ratio:
        warnings.append(
            "legacy_recording_has_no_62_dim_node_features; use --require-62 to block it"
        )
    if inferred_label is None:
        blocking_reasons.append("label_not_inferred_from_filename")
    if path.name.lower().startswith("train_"):
        blocking_reasons.append("train_prefix_reserved_for_adaptive_classifier")
    if skipped_lines:
        warnings.append(f"skipped_lines_present: {skipped_lines}")

    return {
        "path": str(path),
        "filename": path.name,
        "valid_frames": valid_frames,
        "parse_errors": parse_errors,
        "parse_error_examples": parse_error_examples,
        "skipped_lines": skipped_lines,
        "feature_width": feature_width,
        "feature_widths_seen": sorted(widths),
        "node_ids_seen": sorted(node_ids, key=lambda value: str(value)),
        "node_feature_frame_ratio": round(ratio, 4),
        "min_nodes_per_frame": min(node_counts) if node_counts else 0,
        "max_nodes_per_frame": max(node_counts) if node_counts else 0,
        "inferred_label": inferred_label,
        "safe_to_train": not blocking_reasons,
        "blocking_reasons": blocking_reasons,
        "warnings": warnings,
    }


def aggregate_results(recordings: list[dict[str, Any]], options: InspectOptions) -> dict[str, Any]:
    class_coverage: dict[str, int] = {}
    safe_class_coverage: dict[str, int] = {}
    feature_widths: set[str] = set()
    blocking_files: list[str] = []
    files_blocking_62_training: list[str] = []

    for recording in recordings:
        label = recording["inferred_label"]
        if label is not None:
            class_coverage[label] = class_coverage.get(label, 0) + 1
            if recording["safe_to_train"]:
                safe_class_coverage[label] = safe_class_coverage.get(label, 0) + 1

        feature_widths.add(recording["feature_width"])
        if not recording["safe_to_train"]:
            blocking_files.append(recording["path"])

        would_block_62 = (
            not recording["safe_to_train"]
            or recording["feature_width"] != str(MULTI_NODE_FEATURE_WIDTH)
            or recording["node_feature_frame_ratio"] < options.min_node_feature_ratio
        )
        if would_block_62:
            files_blocking_62_training.append(recording["path"])

    blocking_reasons: list[str] = []
    if not recordings:
        blocking_reasons.append("no_recordings_found")
    if blocking_files:
        blocking_reasons.append(f"blocking_recordings_present: {len(blocking_files)}")
    missing_count_labels = [
        label for label in COUNT_LABELS
        if safe_class_coverage.get(label, 0) == 0
    ]
    if options.require_count_coverage and missing_count_labels:
        blocking_reasons.append(
            "missing_count_coverage: " + ",".join(missing_count_labels)
        )

    return {
        "recording_count": len(recordings),
        "safe_recordings": sum(
            1 for recording in recordings if recording["safe_to_train"]
        ),
        "unsafe_recordings": len(blocking_files),
        "class_coverage": dict(sorted(class_coverage.items())),
        "safe_class_coverage": dict(sorted(safe_class_coverage.items())),
        "classes_seen": sorted(class_coverage),
        "safe_classes_seen": sorted(safe_class_coverage),
        "missing_count_labels": missing_count_labels,
        "feature_widths": sorted(feature_widths),
        "blocking_files": blocking_files,
        "files_blocking_62_training": files_blocking_62_training,
        "safe_to_train": not blocking_reasons,
        "blocking_reasons": blocking_reasons,
        "warnings": [],
    }


def build_report(paths: list[Path], options: InspectOptions) -> tuple[dict[str, Any], list[str]]:
    files, input_errors = expand_inputs(paths)
    recordings = [inspect_recording(path, options) for path in files]
    report = {
        "options": {
            "require_62": options.require_62,
            "require_count_coverage": options.require_count_coverage,
            "min_frames": options.min_frames,
            "min_node_feature_ratio": options.min_node_feature_ratio,
        },
        "recordings": recordings,
        "aggregate": aggregate_results(recordings, options),
    }
    return report, input_errors


def print_human_report(report: dict[str, Any], input_errors: list[str]) -> None:
    for error in input_errors:
        print(f"ERROR {error}", file=sys.stderr)

    for recording in report["recordings"]:
        status = "PASS" if recording["safe_to_train"] else "BLOCKED"
        print(
            f"{status} {recording['path']}: "
            f"frames={recording['valid_frames']} "
            f"width={recording['feature_width']} "
            f"label={recording['inferred_label'] or 'unknown'} "
            f"node_ratio={recording['node_feature_frame_ratio']:.4f} "
            f"nodes={recording['min_nodes_per_frame']}-{recording['max_nodes_per_frame']}"
        )
        for reason in recording["blocking_reasons"]:
            print(f"  block: {reason}")
        for warning in recording["warnings"]:
            print(f"  warn: {warning}")

    aggregate = report["aggregate"]
    status = "PASS" if aggregate["safe_to_train"] else "BLOCKED"
    print(
        f"{status} aggregate: recordings={aggregate['recording_count']} "
        f"classes={aggregate['classes_seen']} "
        f"safe_classes={aggregate['safe_classes_seen']} "
        f"widths={aggregate['feature_widths']}"
    )
    for reason in aggregate["blocking_reasons"]:
        print(f"  block: {reason}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Inspect count-labeled JSONL recordings for readiness."
    )
    parser.add_argument("paths", nargs="+", metavar="PATH", help="JSONL file or directory")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    parser.add_argument(
        "--require-62",
        action="store_true",
        help="block recordings that are not consistently 62-dimensional",
    )
    parser.add_argument(
        "--require-count-coverage",
        action="store_true",
        help="block aggregate readiness unless count0, count1, and count2 are all present",
    )
    parser.add_argument("--min-frames", type=int, default=DEFAULT_MIN_FRAMES)
    parser.add_argument(
        "--min-node-feature-ratio",
        type=float,
        default=DEFAULT_MIN_NODE_FEATURE_RATIO,
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    options = InspectOptions(
        require_62=args.require_62,
        require_count_coverage=args.require_count_coverage,
        min_frames=args.min_frames,
        min_node_feature_ratio=args.min_node_feature_ratio,
    )
    report, input_errors = build_report([Path(path) for path in args.paths], options)

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
        for error in input_errors:
            print(f"ERROR {error}", file=sys.stderr)
    else:
        print_human_report(report, input_errors)

    if input_errors:
        return 2
    return 0 if report["aggregate"]["safe_to_train"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
