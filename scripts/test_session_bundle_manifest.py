#!/usr/bin/env python3

import json
from pathlib import Path
import tempfile
import unittest

import session_bundle_manifest as bundle


class SessionBundleManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.root = Path(self.temporary_directory.name)
        (self.root / "solutions" / "run-1" / "metrics").mkdir(parents=True)
        (self.root / "solutions" / "run-1" / "logs").mkdir()
        (self.root / "solutions" / "run-1" / "result.json").write_text(
            '{"outcome":"PASS"}\n', encoding="utf-8"
        )
        (self.root / "solutions" / "run-1" / "metrics" / "client.json").write_text(
            "{}\n", encoding="utf-8"
        )
        (self.root / "solutions" / "run-1" / "logs" / "client.log").write_text(
            "complete\n", encoding="utf-8"
        )

    def test_create_and_verify_nested_bundle(self) -> None:
        manifest_path, count = bundle.create_manifest(str(self.root), bundle.DEFAULT_MANIFEST)
        self.assertEqual(count, 3)

        (self.root / bundle.FINAL_SESSION_MANIFEST).write_text("{}\n", encoding="utf-8")
        _, verified_count = bundle.verify_manifest(str(self.root), bundle.DEFAULT_MANIFEST)
        self.assertEqual(verified_count, 3)

        payload = json.loads(manifest_path.read_text(encoding="utf-8"))
        paths = [item["path"] for item in payload["files"]]
        self.assertEqual(paths, sorted(paths))
        self.assertIn("solutions/run-1/result.json", paths)
        self.assertIn("solutions/run-1/metrics/client.json", paths)
        self.assertIn("solutions/run-1/logs/client.log", paths)
        self.assertNotIn(bundle.DEFAULT_MANIFEST, paths)
        self.assertNotIn(bundle.FINAL_SESSION_MANIFEST, paths)

    def test_verify_detects_tampering(self) -> None:
        bundle.create_manifest(str(self.root), bundle.DEFAULT_MANIFEST)
        target = self.root / "solutions" / "run-1" / "result.json"
        target.write_text('{"outcome":"FAIL"}\n', encoding="utf-8")
        with self.assertRaisesRegex(bundle.ManifestError, "changed=.*result.json"):
            bundle.verify_manifest(str(self.root), bundle.DEFAULT_MANIFEST)

    def test_create_refuses_to_overwrite_manifest(self) -> None:
        bundle.create_manifest(str(self.root), bundle.DEFAULT_MANIFEST)
        with self.assertRaisesRegex(bundle.ManifestError, "refusing to overwrite"):
            bundle.create_manifest(str(self.root), bundle.DEFAULT_MANIFEST)

    def test_symlink_and_manifest_traversal_are_rejected(self) -> None:
        (self.root / "outside-link").symlink_to(Path(self.temporary_directory.name).parent)
        with self.assertRaisesRegex(bundle.ManifestError, "symlink is not allowed"):
            bundle.create_manifest(str(self.root), bundle.DEFAULT_MANIFEST)
        with self.assertRaisesRegex(bundle.ManifestError, "not normalized"):
            bundle.create_manifest(str(self.root), "../bundle-manifest.json")
        with self.assertRaisesRegex(bundle.ManifestError, "reserved"):
            bundle.create_manifest(str(self.root), bundle.FINAL_SESSION_MANIFEST)


if __name__ == "__main__":
    unittest.main()
