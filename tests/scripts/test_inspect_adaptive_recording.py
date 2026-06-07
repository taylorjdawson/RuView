from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "inspect-adaptive-recording.py"


def write_jsonl(path: Path, frames: list[dict]) -> None:
    path.write_text(
        "\n".join(json.dumps(frame) for frame in frames) + "\n",
        encoding="utf-8",
    )


def legacy_frame(index: int = 0) -> dict:
    return {
        "features": {
            "variance": 1.0 + index,
            "motion_band_power": 2.0,
            "breathing_band_power": 3.0,
            "spectral_power": 4.0,
            "dominant_freq_hz": 5.0,
            "change_points": 6,
            "mean_rssi": -42.0,
        },
        "nodes": [{"amplitude": [1.0, 2.0, 3.0]}],
    }


def multi_node_frame(index: int = 0) -> dict:
    return {
        "node_features": [
            {
                "node_id": 7,
                "rssi_dbm": -70.0,
                "features": {
                    "variance": 70.0 + index,
                    "motion_band_power": 71.0,
                    "breathing_band_power": 72.0,
                    "spectral_power": 73.0,
                    "dominant_freq_hz": 74.0,
                    "change_points": 75,
                    "mean_rssi": -70.0,
                },
            },
            {
                "node_id": 2,
                "rssi_dbm": -20.0,
                "features": {
                    "variance": 20.0 + index,
                    "motion_band_power": 21.0,
                    "breathing_band_power": 22.0,
                    "spectral_power": 23.0,
                    "dominant_freq_hz": 24.0,
                    "change_points": 25,
                    "mean_rssi": -20.0,
                },
            },
        ]
    }


def run_inspector(*args: str) -> tuple[int, dict]:
    result = subprocess.run(
        [sys.executable, str(SCRIPT), "--json", *args],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.returncode, json.loads(result.stdout)


def only_recording(report: dict) -> dict:
    assert len(report["recordings"]) == 1
    return report["recordings"][0]


def test_valid_62_dim_multi_node_recording_passes(tmp_path: Path) -> None:
    recording = tmp_path / "count1_still_smoke.jsonl"
    write_jsonl(recording, [multi_node_frame(i) for i in range(30)])

    code, report = run_inspector("--require-62", str(recording))

    rec = only_recording(report)
    assert code == 0
    assert rec["safe_to_train"] is True
    assert rec["feature_width"] == "62"
    assert rec["node_ids_seen"] == [2, 7]
    assert rec["node_feature_frame_ratio"] == 1.0
    assert rec["min_nodes_per_frame"] == 2
    assert rec["max_nodes_per_frame"] == 2
    assert rec["inferred_label"] == "count1"


def test_legacy_15_feature_recording_passes_only_without_require_62(tmp_path: Path) -> None:
    recording = tmp_path / "count0_legacy.jsonl"
    write_jsonl(recording, [legacy_frame(i) for i in range(30)])

    code, report = run_inspector(str(recording))
    assert code == 0
    assert only_recording(report)["feature_width"] == "15"
    assert only_recording(report)["safe_to_train"] is True

    code, report = run_inspector("--require-62", str(recording))
    rec = only_recording(report)
    assert code == 1
    assert rec["safe_to_train"] is False
    assert "require_62_not_met: feature_width=15" in rec["blocking_reasons"]


def test_mixed_15_and_62_recording_fails(tmp_path: Path) -> None:
    recording = tmp_path / "count2_mixed.jsonl"
    write_jsonl(
        recording,
        [multi_node_frame(i) for i in range(20)]
        + [legacy_frame(i) for i in range(10)],
    )

    code, report = run_inspector(str(recording))

    rec = only_recording(report)
    assert code == 1
    assert rec["feature_width"] == "mixed"
    assert rec["feature_widths_seen"] == [15, 62]
    assert "mixed_feature_widths" in rec["blocking_reasons"]


def test_malformed_json_fails_with_parse_error_evidence(tmp_path: Path) -> None:
    recording = tmp_path / "count1_malformed.jsonl"
    good_lines = [json.dumps(multi_node_frame(i)) for i in range(30)]
    recording.write_text(
        "\n".join(good_lines[:10] + ["{not json"] + good_lines[10:]) + "\n"
    )

    code, report = run_inspector("--require-62", str(recording))

    rec = only_recording(report)
    assert code == 1
    assert rec["parse_errors"] == 1
    assert rec["parse_error_examples"][0]["line"] == 11
    assert "parse_errors_present: 1" in rec["blocking_reasons"]


def test_unknown_filename_label_fails(tmp_path: Path) -> None:
    recording = tmp_path / "count10_smoke.jsonl"
    write_jsonl(recording, [multi_node_frame(i) for i in range(30)])

    code, report = run_inspector("--require-62", str(recording))

    rec = only_recording(report)
    assert code == 1
    assert rec["inferred_label"] is None
    assert "label_not_inferred_from_filename" in rec["blocking_reasons"]


def test_train_prefix_is_blocked_to_avoid_adaptive_classifier_ingestion(tmp_path: Path) -> None:
    recording = tmp_path / "train_count0_smoke.jsonl"
    write_jsonl(recording, [multi_node_frame(i) for i in range(30)])

    code, report = run_inspector("--require-62", str(recording))

    rec = only_recording(report)
    assert code == 1
    assert rec["inferred_label"] == "count0"
    assert "train_prefix_reserved_for_adaptive_classifier" in rec["blocking_reasons"]


def test_directory_aggregate_reports_class_coverage_and_blocking_files(tmp_path: Path) -> None:
    count0 = tmp_path / "count0_empty_smoke.jsonl"
    count1 = tmp_path / "count1_still_smoke.jsonl"
    mystery = tmp_path / "mystery_smoke.jsonl"
    write_jsonl(count0, [multi_node_frame(i) for i in range(30)])
    write_jsonl(count1, [multi_node_frame(i) for i in range(30)])
    write_jsonl(mystery, [multi_node_frame(i) for i in range(30)])

    code, report = run_inspector("--require-62", str(tmp_path))

    aggregate = report["aggregate"]
    assert code == 1
    assert aggregate["recording_count"] == 3
    assert aggregate["class_coverage"] == {"count0": 1, "count1": 1}
    assert aggregate["safe_class_coverage"] == {"count0": 1, "count1": 1}
    assert aggregate["feature_widths"] == ["62"]
    assert str(mystery) in aggregate["blocking_files"]
    assert str(mystery) in aggregate["files_blocking_62_training"]
    assert aggregate["safe_to_train"] is False


def test_directory_aggregate_can_require_count0_count1_count2_coverage(tmp_path: Path) -> None:
    count0 = tmp_path / "count0_empty_smoke.jsonl"
    count1 = tmp_path / "count1_still_smoke.jsonl"
    count2 = tmp_path / "count2_two_people_smoke.jsonl"
    write_jsonl(count0, [multi_node_frame(i) for i in range(30)])
    write_jsonl(count1, [multi_node_frame(i) for i in range(30)])
    write_jsonl(count2, [multi_node_frame(i) for i in range(30)])

    code, report = run_inspector("--require-62", "--require-count-coverage", str(tmp_path))

    aggregate = report["aggregate"]
    assert code == 0
    assert aggregate["safe_to_train"] is True
    assert aggregate["class_coverage"] == {"count0": 1, "count1": 1, "count2": 1}
    assert aggregate["safe_class_coverage"] == {"count0": 1, "count1": 1, "count2": 1}
    assert aggregate["missing_count_labels"] == []


def test_required_count_coverage_fails_when_a_count_label_is_missing(tmp_path: Path) -> None:
    count0 = tmp_path / "count0_empty_smoke.jsonl"
    count1 = tmp_path / "count1_still_smoke.jsonl"
    write_jsonl(count0, [multi_node_frame(i) for i in range(30)])
    write_jsonl(count1, [multi_node_frame(i) for i in range(30)])

    code, report = run_inspector("--require-62", "--require-count-coverage", str(tmp_path))

    aggregate = report["aggregate"]
    assert code == 1
    assert aggregate["safe_to_train"] is False
    assert aggregate["missing_count_labels"] == ["count2"]
    assert "missing_count_coverage: count2" in aggregate["blocking_reasons"]


def test_train_prefixed_count_file_does_not_satisfy_required_count_coverage(tmp_path: Path) -> None:
    count0 = tmp_path / "count0_empty_smoke.jsonl"
    count1 = tmp_path / "count1_still_smoke.jsonl"
    unsafe_count2 = tmp_path / "train_count2_two_people_smoke.jsonl"
    write_jsonl(count0, [multi_node_frame(i) for i in range(30)])
    write_jsonl(count1, [multi_node_frame(i) for i in range(30)])
    write_jsonl(unsafe_count2, [multi_node_frame(i) for i in range(30)])

    code, report = run_inspector("--require-62", "--require-count-coverage", str(tmp_path))

    aggregate = report["aggregate"]
    assert code == 1
    assert aggregate["class_coverage"] == {"count0": 1, "count1": 1, "count2": 1}
    assert aggregate["safe_class_coverage"] == {"count0": 1, "count1": 1}
    assert aggregate["missing_count_labels"] == ["count2"]
    assert str(unsafe_count2) in aggregate["blocking_files"]
