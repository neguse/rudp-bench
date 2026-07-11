#!/usr/bin/env python3
"""Create or verify a deterministic manifest for a benchmark session bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import sys
from typing import Any


DEFAULT_MANIFEST = "bundle-manifest.json"
FINAL_SESSION_MANIFEST = "session-manifest.json"
SHA256_CHUNK_SIZE = 1024 * 1024


class ManifestError(Exception):
    """Raised when a bundle cannot be created or verified safely."""


def safe_relative_path(value: str, description: str) -> PurePosixPath:
    if not value or "\\" in value:
        raise ManifestError(f"{description} must be a non-empty POSIX relative path")
    if any(part in {"", ".", ".."} for part in value.split("/")):
        raise ManifestError(f"{description} is not normalized: {value!r}")

    path = PurePosixPath(value)
    if path.is_absolute():
        raise ManifestError(f"{description} must not be absolute: {value!r}")
    return path


def manifest_path(root: Path, manifest_name: str) -> tuple[Path, str]:
    relative = safe_relative_path(manifest_name, "manifest path")
    if relative.as_posix() == FINAL_SESSION_MANIFEST:
        raise ManifestError(
            f"manifest path is reserved for the final session manifest: {manifest_name!r}"
        )
    return root.joinpath(*relative.parts), relative.as_posix()


def sha256_regular_file(path: Path) -> tuple[int, str]:
    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW

    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise ManifestError(f"cannot open regular file {path}: {exc}") from exc

    digest = hashlib.sha256()
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode):
            raise ManifestError(f"unsupported non-regular file: {path}")
        while chunk := os.read(fd, SHA256_CHUNK_SIZE):
            digest.update(chunk)
        after = os.fstat(fd)
    except OSError as exc:
        raise ManifestError(f"cannot hash regular file {path}: {exc}") from exc
    finally:
        os.close(fd)

    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ManifestError(f"file changed while hashing: {path}")
    return after.st_size, digest.hexdigest()


def collect_files(root: Path, excluded_paths: set[str]) -> list[dict[str, Any]]:
    files: list[dict[str, Any]] = []

    def visit(directory: Path) -> None:
        try:
            with os.scandir(directory) as iterator:
                entries = sorted(iterator, key=lambda entry: entry.name)
        except OSError as exc:
            raise ManifestError(f"cannot list directory {directory}: {exc}") from exc

        for entry in entries:
            path = Path(entry.path)
            relative = path.relative_to(root).as_posix()
            safe_relative_path(relative, "bundle path")
            try:
                entry_stat = entry.stat(follow_symlinks=False)
            except OSError as exc:
                raise ManifestError(f"cannot stat bundle path {relative}: {exc}") from exc

            if stat.S_ISLNK(entry_stat.st_mode):
                raise ManifestError(f"symlink is not allowed in a session bundle: {relative}")
            if stat.S_ISDIR(entry_stat.st_mode):
                visit(path)
                continue
            if not stat.S_ISREG(entry_stat.st_mode):
                raise ManifestError(f"unsupported non-regular bundle path: {relative}")
            if relative in excluded_paths:
                continue

            size, digest = sha256_regular_file(path)
            files.append({"path": relative, "size": size, "sha256": digest})

    visit(root)
    files.sort(key=lambda item: item["path"])
    return files


def session_root(value: str) -> Path:
    candidate = Path(value)
    try:
        candidate_stat = candidate.lstat()
    except OSError as exc:
        raise ManifestError(f"cannot stat session root {candidate}: {exc}") from exc
    if stat.S_ISLNK(candidate_stat.st_mode):
        raise ManifestError(f"session root must not be a symlink: {candidate}")
    if not stat.S_ISDIR(candidate_stat.st_mode):
        raise ManifestError(f"session root is not a directory: {candidate}")
    return candidate.resolve(strict=True)


def build_manifest(root: Path, manifest_relative: str) -> dict[str, Any]:
    files = collect_files(root, {manifest_relative, FINAL_SESSION_MANIFEST})
    return {
        "algorithm": "sha256",
        "files": files,
        "version": 1,
    }


def create_manifest(root_value: str, manifest_name: str) -> tuple[Path, int]:
    root = session_root(root_value)
    output_path, manifest_relative = manifest_path(root, manifest_name)
    if output_path.parent == output_path or not output_path.parent.is_dir():
        raise ManifestError(f"manifest parent is not an existing directory: {output_path.parent}")

    payload = build_manifest(root, manifest_relative)
    serialized = json.dumps(payload, ensure_ascii=True, indent=2, sort_keys=True) + "\n"
    try:
        with output_path.open("x", encoding="utf-8", newline="\n") as output:
            output.write(serialized)
    except FileExistsError as exc:
        raise ManifestError(f"refusing to overwrite existing manifest: {output_path}") from exc
    except OSError as exc:
        raise ManifestError(f"cannot create manifest {output_path}: {exc}") from exc
    return output_path, len(payload["files"])


def load_manifest(path: Path) -> dict[str, Any]:
    size, _ = sha256_regular_file(path)
    if size == 0:
        raise ManifestError(f"manifest is empty: {path}")
    try:
        with path.open("r", encoding="utf-8") as source:
            payload = json.load(source)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ManifestError(f"cannot read manifest {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise ManifestError("manifest root must be a JSON object")
    return payload


def validate_manifest_schema(payload: dict[str, Any]) -> list[dict[str, Any]]:
    if set(payload) != {"algorithm", "files", "version"}:
        raise ManifestError("manifest must contain exactly algorithm, files, and version")
    if payload["version"] != 1 or payload["algorithm"] != "sha256":
        raise ManifestError("unsupported manifest version or digest algorithm")
    if not isinstance(payload["files"], list):
        raise ManifestError("manifest files must be a JSON array")

    previous = ""
    for index, item in enumerate(payload["files"]):
        if not isinstance(item, dict) or set(item) != {"path", "size", "sha256"}:
            raise ManifestError(f"manifest files[{index}] has an invalid shape")
        path = item["path"]
        size = item["size"]
        digest = item["sha256"]
        if not isinstance(path, str):
            raise ManifestError(f"manifest files[{index}].path must be a string")
        safe_relative_path(path, f"manifest files[{index}].path")
        if path <= previous:
            raise ManifestError("manifest file paths must be unique and sorted")
        previous = path
        if not isinstance(size, int) or isinstance(size, bool) or size < 0:
            raise ManifestError(f"manifest files[{index}].size must be a non-negative integer")
        if (
            not isinstance(digest, str)
            or len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)
        ):
            raise ManifestError(f"manifest files[{index}].sha256 is invalid")
    return payload["files"]


def verify_manifest(root_value: str, manifest_name: str) -> tuple[Path, int]:
    root = session_root(root_value)
    input_path, manifest_relative = manifest_path(root, manifest_name)
    payload = load_manifest(input_path)
    recorded_files = validate_manifest_schema(payload)
    current_files = collect_files(root, {manifest_relative, FINAL_SESSION_MANIFEST})
    if recorded_files != current_files:
        recorded = {item["path"]: item for item in recorded_files}
        current = {item["path"]: item for item in current_files}
        added = sorted(current.keys() - recorded.keys())
        removed = sorted(recorded.keys() - current.keys())
        changed = sorted(
            path for path in current.keys() & recorded.keys() if current[path] != recorded[path]
        )
        details = []
        if added:
            details.append(f"added={added}")
        if removed:
            details.append(f"removed={removed}")
        if changed:
            details.append(f"changed={changed}")
        raise ManifestError("bundle verification failed: " + ", ".join(details))
    return input_path, len(current_files)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("create", "verify"):
        subparser = subparsers.add_parser(command)
        subparser.add_argument("session_dir")
        subparser.add_argument("--manifest", default=DEFAULT_MANIFEST)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.command == "create":
            path, count = create_manifest(args.session_dir, args.manifest)
            print(f"created {path} with {count} files")
        else:
            path, count = verify_manifest(args.session_dir, args.manifest)
            print(f"verified {path} with {count} files")
    except ManifestError as exc:
        print(f"session bundle manifest: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
