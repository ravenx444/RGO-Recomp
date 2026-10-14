"""Admit an explicitly pinned, facts-only native generation metadata bundle.

The default bundle has a reviewed manifest digest pinned in source. Custom
bundles require a trusted manifest digest separately from the bundle. Generated code and ROM bytes stay private.
"""
from __future__ import annotations

import hashlib
import json
import re
import tomllib
from pathlib import Path

ROM_SHA1 = "493ced9008dbe932d6e91179b68e8630cf23a023"
FILES = {"bindings.json", "functions.toml", "data-symbols.toml"}
DEFAULT_BUNDLE = "symbols/native-us-v1"
DEFAULT_MANIFEST_SHA256 = "86be2628e3def35bdb2b15a2468d130ee351bd12599fcdb2b0d7894c52d14ce9"
MAX_BYTES = 2 * 1024 * 1024
LAYOUT_FIELDS = {"main_relocation_start", "overlay_reference_table_start",
                 "overlay_table_start", "overlay_data_start", "main_text_size", "main_data_size"}


def _object(value: object, fields: set[str]) -> dict:
    if not isinstance(value, dict) or set(value) != fields:
        raise ValueError("Native metadata fields are invalid")
    return value


def _uint(value: object) -> None:
    if type(value) is not int or not 0 <= value < 2**32:
        raise ValueError("Native metadata integer is invalid")


def _name(value: object) -> None:
    if not isinstance(value, str) or len(value) > 256 or not re.fullmatch(r"[A-Za-z0-9_.$]*", value):
        raise ValueError("Native metadata name is invalid")


def _rows(value: object) -> list:
    if not isinstance(value, list) or len(value) > 20000:
        raise ValueError("Native metadata inventory is invalid")
    return value


def _json(data: bytes) -> dict:
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("Duplicate native metadata field")
            result[key] = value
        return result
    return json.loads(data, object_pairs_hook=pairs)


def _bytes(path: Path) -> bytes:
    if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
        raise ValueError("Native metadata must be a regular local file")
    if not path.is_file() or path.stat().st_size > MAX_BYTES:
        raise ValueError("Native metadata file is missing or exceeds its limit")
    return path.read_bytes()


def validate_facts(functions: dict, data: dict, bindings: dict) -> None:
    """Reject executable payload fields and admit only names and numeric facts."""
    for context, member in ((functions, "functions"), (data, "symbols")):
        sections = _rows(_object(context, {"section"})["section"])
        for section in sections:
            if not isinstance(section, dict):
                raise ValueError("Native metadata section is invalid")
            fields = {"name", "vram", "size", member}
            if member == "functions" or "rom" in section:
                fields.add("rom")
            _object(section, fields)
            _name(section["name"])
            for key in fields - {"name", member}:
                _uint(section[key])
            for row in _rows(section[member]):
                row_fields = {"name", "vram", "size"} if member == "functions" else {"name", "vram"}
                _object(row, row_fields)
                _name(row["name"])
                for key in row_fields - {"name"}:
                    _uint(row[key])
    _object(bindings, {"schema_version", "rom_sha1", "layout", "covered_aliases",
                       "manual_sizes", "audio", "sdk_identified", "entrypoint_vram"})
    if type(bindings["schema_version"]) is not int or bindings["schema_version"] != 1 or bindings["rom_sha1"] != ROM_SHA1:
        raise ValueError("Native metadata version or ROM identity is invalid")
    for value in _object(bindings["layout"], LAYOUT_FIELDS).values():
        _uint(value)
    for member, numeric, names in (
        ("covered_aliases", {"section", "offset", "owner_offset", "owner_size"}, {"private_name", "private_owner_name"}),
        ("manual_sizes", {"section", "offset", "recovered_size"}, {"private_name", "generated_function"}),
    ):
        for row in _rows(bindings[member]):
            _object(row, numeric | names)
            for key in numeric: _uint(row[key])
            for key in names: _name(row[key])
    audio = _object(bindings["audio"], {"rom_start", "size", "configuration"})
    _uint(audio["rom_start"]); _uint(audio["size"])
    if audio["size"] == 0 or audio["rom_start"] + audio["size"] > 32*1024*1024:
        raise ValueError("Native audio ROM extent is invalid")
    config = _object(audio["configuration"], {"text_offset", "text_size", "text_address", "extra_indirect_branch_targets", "overlay_slots"})
    for key in ("text_offset", "text_size", "text_address"): _uint(config[key])
    if config["text_offset"] + config["text_size"] > audio["size"]:
        raise ValueError("Native audio text extent is invalid")
    for value in _rows(config["extra_indirect_branch_targets"]): _uint(value)
    for slot in _rows(config["overlay_slots"]):
        _object(slot, {"text_address", "overlays"}); _uint(slot["text_address"])
        for overlay in _rows(slot["overlays"]):
            _object(overlay, {"offset", "size"})
            _uint(overlay["offset"]); _uint(overlay["size"])
            if overlay["offset"] + overlay["size"] > audio["size"]:
                raise ValueError("Native audio overlay extent is invalid")
    for row in _rows(bindings["sdk_identified"]):
        _object(row, {"generated_function", "in_generated_set", "libultra", "vram"})
        _name(row["libultra"])
        if type(row["in_generated_set"]) is not bool:
            raise ValueError("Native SDK mapping flag is invalid")
        if row["generated_function"] is not None: _name(row["generated_function"])
        if row["in_generated_set"] != (row["generated_function"] is not None):
            raise ValueError("Native SDK mapping is inconsistent")
        if not isinstance(row["vram"], str) or not re.fullmatch(r"0x[0-9a-f]{8}", row["vram"]):
            raise ValueError("Native SDK address is invalid")
    if not isinstance(bindings["entrypoint_vram"], str) or not re.fullmatch(r"0x[0-9a-f]{8}", bindings["entrypoint_vram"]):
        raise ValueError("Native entrypoint address is invalid")


def load_bundle(directory: Path, trusted_manifest_sha256: str) -> tuple[dict, dict, dict]:
    if not re.fullmatch(r"[0-9a-f]{64}", trusted_manifest_sha256):
        raise ValueError("A trusted native metadata manifest SHA256 is required")
    manifest_bytes = _bytes(directory / "manifest.json")
    if hashlib.sha256(manifest_bytes).hexdigest() != trusted_manifest_sha256:
        raise ValueError("Native metadata manifest does not match its trusted digest")
    manifest = _object(_json(manifest_bytes), {"schema_version", "source_decomp_commit",
        "source_recompiler_commit", "rom_sha1", "files", "scope"})
    if type(manifest["schema_version"]) is not int or manifest["schema_version"] != 1 or manifest["rom_sha1"] != ROM_SHA1:
        raise ValueError("Native metadata manifest identity is invalid")
    for key in ("source_decomp_commit", "source_recompiler_commit"):
        if not isinstance(manifest[key], str) or not re.fullmatch(r"[0-9a-f]{40}", manifest[key]):
            raise ValueError("Native metadata source revision is invalid")
    files = _object(manifest["files"], FILES)
    loaded = {}
    for name, record in files.items():
        _object(record, {"size", "sha256"})
        payload = _bytes(directory / name)
        if type(record["size"]) is not int or len(payload) != record["size"] or hashlib.sha256(payload).hexdigest() != record["sha256"]:
            raise ValueError("Native metadata payload does not match its manifest")
        loaded[name] = _json(payload) if name.endswith('.json') else tomllib.loads(payload.decode('utf-8'))
    functions, data, bindings = loaded['functions.toml'], loaded['data-symbols.toml'], loaded['bindings.json']
    validate_facts(functions, data, bindings)
    if len(functions['section']) != 156 or sum(len(s['functions']) for s in functions['section']) != 2909 or len(data['section']) != 324 or len(bindings['covered_aliases']) != 824 or len(bindings['manual_sizes']) != 10:
        raise ValueError("Native metadata qualified inventories changed")
    return functions, data, bindings


def default_bundle(root: Path) -> tuple[Path, str]:
    directory = root / DEFAULT_BUNDLE
    load_bundle(directory, DEFAULT_MANIFEST_SHA256)
    manifest = _json(_bytes(directory / "manifest.json"))
    lock = json.loads((root / "dependencies.lock.json").read_text(encoding="utf-8"))
    pins = {row["id"]: row["commit"] for row in lock["repositories"]}
    if (manifest["source_decomp_commit"] != pins.get("jfg-decomp")
            or manifest["source_recompiler_commit"] != pins.get("n64recomp")):
        raise ValueError("Native metadata source revisions differ from the dependency lock")
    return directory, DEFAULT_MANIFEST_SHA256
