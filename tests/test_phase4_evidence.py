"""Check the preserved clean-checkout experiment against its raw evidence."""

import csv
import json
import statistics
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1] / "results" / "phase4" / "windows-clean-f6435cf"


class Phase4EvidenceTest(unittest.TestCase):
    def test_summary_and_provenance(self):
        manifest = json.loads((ROOT / "manifest.json").read_text(encoding="utf-8"))
        accuracy = json.loads((ROOT / "accuracy.json").read_text(encoding="utf-8"))
        with (ROOT / "samples.csv").open(newline="", encoding="utf-8") as f:
            samples = list(csv.DictReader(f))
        with (ROOT / "summary.csv").open(newline="", encoding="utf-8") as f:
            summary = list(csv.DictReader(f))
        self.assertEqual(manifest["status"], "complete")
        self.assertEqual(manifest["source_checkout"]["commit"], "f6435cfb357e328af33cfe60571412f7ed67552a")
        self.assertFalse(manifest["source_checkout"]["tracked_or_untracked_changes"])
        self.assertEqual(manifest["input_sha256"], accuracy["input_sha256"]["measurements"])
        self.assertEqual(len(samples), manifest["sample_count"])
        self.assertEqual(len(samples), 144)
        self.assertEqual(len(summary), 48)
        keys = ("records", "requested_tracks", "tracks", "batch_size", "workers")
        for row in summary:
            matching = [s for s in samples if all(s[k] == row[k] for k in keys)]
            self.assertEqual(len(matching), 3)
            self.assertAlmostEqual(float(row["median_ms"]), statistics.median(float(s["elapsed_ms"]) for s in matching))
            baseline = next(s for s in summary if s["workers"] == "1" and all(s[k] == row[k] for k in keys if k != "workers"))
            self.assertAlmostEqual(float(row["speedup_vs_one_worker"]), float(baseline["median_ms"]) / float(row["median_ms"]))


if __name__ == "__main__":
    unittest.main()
