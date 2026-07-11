import json
import subprocess


MAPPING_FIELDS = {"primitive", "delivery", "ordering", "realization"}
DELIVERY_VALUES = {"best_effort", "reliable"}
ORDERING_VALUES = {"unordered", "ordered"}
REALIZATION_VALUES = {"native", "emulated", "reliable_fallback", "unsupported"}
PAYLOAD_PATTERN = "splitmix64-v1"
WIRE_COMPRESSION = "none"


def validate_describe_pair(server_bin, client_bin):
    descriptions = []
    for binary in (server_bin, client_bin):
        completed = subprocess.run(
            [binary, "--describe"],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
        description = json.loads(completed.stdout)
        if description.get("payload_pattern") != PAYLOAD_PATTERN:
            raise AssertionError(
                f"{binary}: payload_pattern must be {PAYLOAD_PATTERN!r}"
            )
        if description.get("wire_compression") != WIRE_COMPRESSION:
            raise AssertionError(
                f"{binary}: wire_compression must be {WIRE_COMPRESSION!r}"
            )
        mapping = description.get("class_mapping")
        if not isinstance(mapping, dict) or set(mapping) != {
            "loss_tolerant",
            "must_deliver",
        }:
            raise AssertionError(f"{binary}: invalid class_mapping classes")
        for class_name, spec in mapping.items():
            if not isinstance(spec, dict) or set(spec) != MAPPING_FIELDS:
                raise AssertionError(
                    f"{binary}: class_mapping.{class_name} schema mismatch"
                )
            if not isinstance(spec["primitive"], str) or not spec["primitive"].strip():
                raise AssertionError(
                    f"{binary}: class_mapping.{class_name}.primitive is empty"
                )
            if spec["delivery"] not in DELIVERY_VALUES:
                raise AssertionError(
                    f"{binary}: class_mapping.{class_name}.delivery is invalid"
                )
            if spec["ordering"] not in ORDERING_VALUES:
                raise AssertionError(
                    f"{binary}: class_mapping.{class_name}.ordering is invalid"
                )
            if spec["realization"] not in REALIZATION_VALUES:
                raise AssertionError(
                    f"{binary}: class_mapping.{class_name}.realization is invalid"
                )
        descriptions.append(description)

    if descriptions[0].get("transport") != descriptions[1].get("transport"):
        raise AssertionError("server/client transport descriptions differ")
    if descriptions[0]["class_mapping"] != descriptions[1]["class_mapping"]:
        raise AssertionError("server/client class mappings differ")
    if descriptions[0]["payload_pattern"] != descriptions[1]["payload_pattern"]:
        raise AssertionError("server/client payload patterns differ")
    if descriptions[0]["wire_compression"] != descriptions[1]["wire_compression"]:
        raise AssertionError("server/client wire compression differs")
