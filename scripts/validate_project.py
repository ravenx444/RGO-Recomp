#!/usr/bin/env python3
"""Validate JSON schemas, manifests, dependency pins, and repository hygiene."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

from jsonschema import Draft202012Validator

try:
    from .analyze_phase3_overlays import validate_phase3_evidence
    from .check_repository_hygiene import check_repository
    from .compare_phase1_evidence import validate_aggregate_evidence
    from .validate_audio_task_bridge import (
        validate_manifest_document as validate_audio_manifest_document,
        validate_pins as validate_audio_pins,
    )
    from .validate_graphics_task_bridge import (
        validate_manifest_document as validate_graphics_manifest_document,
        validate_pins as validate_graphics_pins,
    )
    from .validate_rsp_task_manifest import (
        validate_manifest_document as validate_rsp_manifest_document,
        validate_pins as validate_rsp_pins,
    )
    from .validate_phase4_manifest import (
        validate_public_completion as validate_phase4_public_completion,
        validate_manifest_document as validate_phase4_manifest_document,
    )
    from .validate_phase7_acceptance import (
        validate_completion as validate_phase7_completion,
        validate_contract as validate_phase7_contract,
    )
    from .validate_phase8_acceptance import (
        validate_completion as validate_phase8_completion,
        validate_contract as validate_phase8_contract,
    )
    from .validate_runtime_capabilities import (
        validate_matrix_document as validate_runtime_matrix_document,
        validate_pins as validate_runtime_pins,
    )
except ImportError:  # Direct script execution places scripts/ on sys.path.
    from analyze_phase3_overlays import validate_phase3_evidence
    from check_repository_hygiene import check_repository
    from compare_phase1_evidence import validate_aggregate_evidence
    from validate_audio_task_bridge import (
        validate_manifest_document as validate_audio_manifest_document,
        validate_pins as validate_audio_pins,
    )
    from validate_graphics_task_bridge import (
        validate_manifest_document as validate_graphics_manifest_document,
        validate_pins as validate_graphics_pins,
    )
    from validate_rsp_task_manifest import (
        validate_manifest_document as validate_rsp_manifest_document,
        validate_pins as validate_rsp_pins,
    )
    from validate_phase4_manifest import (
        validate_public_completion as validate_phase4_public_completion,
        validate_manifest_document as validate_phase4_manifest_document,
    )
    from validate_phase7_acceptance import (
        validate_completion as validate_phase7_completion,
        validate_contract as validate_phase7_contract,
    )
    from validate_phase8_acceptance import (
        validate_completion as validate_phase8_completion,
        validate_contract as validate_phase8_contract,
    )
    from validate_runtime_capabilities import (
        validate_matrix_document as validate_runtime_matrix_document,
        validate_pins as validate_runtime_pins,
    )

ROOT = Path(__file__).resolve().parents[1]

ACTION_USE_PATTERN = re.compile(
    r"^\s*(?:-\s*)?uses:\s*([^\s#]+)", re.MULTILINE
)
FULL_COMMIT_PATTERN = re.compile(r"[0-9a-f]{40}")
HASH_PATTERN = re.compile(r"--hash=sha256:([0-9a-f]{64})")
REQUIREMENT_PATTERN = re.compile(r"([A-Za-z0-9_.-]+)==([^\s\\]+)")
CMAKE_MINIMUM_PATTERN = re.compile(
    r"cmake_minimum_required\s*\(\s*VERSION\s+([0-9]+)\.([0-9]+)(?:\.([0-9]+))?",
    re.IGNORECASE,
)


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON object key")
        result[key] = value
    return result


def _reject_nonstandard_constant(_: str) -> object:
    raise ValueError("non-standard JSON number")


def extract_job(workflow: str, job_name: str) -> str | None:
    """Extract a top-level job from the small, policy-owned CI workflow."""
    lines = workflow.splitlines()
    start: int | None = None
    for index, line in enumerate(lines):
        if re.fullmatch(rf"  {re.escape(job_name)}:\s*", line):
            start = index
            break
    if start is None:
        return None
    end = len(lines)
    for index in range(start + 1, len(lines)):
        if re.fullmatch(r"  [A-Za-z0-9_-]+:\s*", lines[index]):
            end = index
            break
    return "\n".join(lines[start:end])


def extract_run_commands(job: str) -> list[str]:
    """Return executable lines from scalar and block-style run steps."""
    commands: list[str] = []
    lines = job.splitlines()
    index = 0
    while index < len(lines):
        match = re.match(r"^( {8})run:\s*(.*?)\s*$", lines[index])
        if match is None:
            index += 1
            continue
        indent, value = match.groups()
        if value not in {"|", ">"}:
            commands.append(value)
            index += 1
            continue
        block_indent = len(indent) + 2
        index += 1
        while index < len(lines):
            line = lines[index]
            if not line.strip():
                index += 1
                continue
            leading = len(line) - len(line.lstrip())
            if leading < block_indent:
                break
            commands.append(line.strip())
            index += 1
    return commands


def validate_workflow(workflow: str, github_actions: dict[str, str]) -> list[str]:
    errors: list[str] = []
    uses = ACTION_USE_PATTERN.findall(workflow)
    counts: dict[str, int] = {}
    for use in uses:
        action, separator, revision = use.rpartition("@")
        if not separator or FULL_COMMIT_PATTERN.fullmatch(revision) is None:
            errors.append(f"workflow action is not pinned to a full commit: {use}")
            continue
        locked_revision = github_actions.get(action)
        if locked_revision is None:
            errors.append(f"workflow uses an action absent from the lock: {action}")
            continue
        if revision != locked_revision:
            errors.append(f"workflow action pin differs from lock: {action}")
        counts[action] = counts.get(action, 0) + 1

    for action in github_actions:
        if counts.get(action, 0) == 0:
            errors.append(f"workflow does not use locked action pin: {action}")

    if re.search(
        r"^\s*(?:pull_request_target|workflow_run|issue_comment)\s*:",
        workflow,
        re.MULTILINE,
    ):
        errors.append("workflow uses a forbidden privileged or comment-driven trigger")
    if re.search(r"^permissions:\s*\n\s+contents:\s*read\s*$", workflow, re.MULTILINE) is None:
        errors.append("workflow must declare top-level read-only contents permission")
    if re.search(
        r"^concurrency:\s*\n\s+group:\s+\$\{\{ github\.workflow \}\}-\$\{\{ github\.ref \}\}\s*\n\s+cancel-in-progress:\s*true\s*$",
        workflow,
        re.MULTILINE,
    ) is None:
        errors.append("workflow must cancel superseded runs with a concurrency group")

    policy_job = extract_job(workflow, "policy")
    build_job = extract_job(workflow, "build")
    if policy_job is None:
        errors.append("workflow is missing the policy job")
    else:
        if re.search(r"^ {4}runs-on:\s*ubuntu-24\.04\s*$", policy_job, re.MULTILINE) is None:
            errors.append("policy job must run on ubuntu-24.04")
        if re.search(r"^ {4}timeout-minutes:\s*10\s*$", policy_job, re.MULTILINE) is None:
            errors.append("policy job must have the approved timeout")
        required_commands = {
            "python -m pip install --require-hashes -r requirements-dev.lock.txt",
            "python scripts/check_repository_hygiene.py --history",
            "python scripts/validate_project.py",
            'python -m unittest discover -s tests -p "test_*.py" -v',
        }
        commands = set(extract_run_commands(policy_job))
        for command in sorted(required_commands - commands):
            errors.append(f"policy job is missing required command: {command}")
        if re.search(r"^ {10}fetch-depth:\s*0\s*$", policy_job, re.MULTILINE) is None:
            errors.append("policy checkout must fetch full history")
        if re.search(
            r"^ {10}ref:\s*\$\{\{ github\.event\.pull_request\.head\.sha \|\| github\.sha \}\}\s*$",
            policy_job,
            re.MULTILINE,
        ) is None:
            errors.append("policy checkout must scan the event head rather than a synthetic merge")

    if build_job is None:
        errors.append("workflow is missing the build job")
    else:
        if re.search(r"^ {4}timeout-minutes:\s*20\s*$", build_job, re.MULTILINE) is None:
            errors.append("build job must have the approved timeout")
        for host in ("ubuntu-24.04", "windows-2022"):
            if re.search(rf"^ {{10}}-\s+{re.escape(host)}\s*$", build_job, re.MULTILINE) is None:
                errors.append(f"build matrix is missing required host: {host}")
        required_build_commands = {
            "cmake --preset linux",
            "cmake --preset linux-release",
            "cmake --preset linux-clang",
            "cmake --build --preset linux",
            'ctest --preset linux -E "^jfg\\.g2_trap_probe_runtime$"',
            "cmake --build --preset linux-release",
            'ctest --preset linux-release -E "^jfg\\.g2_trap_probe_runtime$"',
            "cmake --build --preset linux-clang",
            'ctest --preset linux-clang -E "^jfg\\.g2_trap_probe_runtime$"',
            # The wrapper builds and tests the same full Windows target set,
            # using a short checkout-specific directory outside the source tree.
            "python scripts/build_windows.py --config Debug --test",
            "python scripts/build_windows.py --config Release --test",
        }
        build_commands = set(extract_run_commands(build_job))
        for command in sorted(required_build_commands - build_commands):
            errors.append(f"build job is missing required command: {command}")

    checkout_count = counts.get("actions/checkout", 0)
    credential_blocks = len(
        re.findall(r"^ {10}persist-credentials:\s*false\s*$", workflow, re.MULTILINE)
    )
    if credential_blocks < checkout_count:
        errors.append("every checkout must disable persisted credentials")
    return errors


def validate_hashed_requirements(lock_text: str, direct_text: str) -> list[str]:
    errors: list[str] = []
    logical_lines: list[str] = []
    pending = ""
    for raw_line in lock_text.splitlines():
        stripped = raw_line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        pending = f"{pending} {stripped}".strip()
        if pending.endswith("\\"):
            pending = pending[:-1].rstrip()
            continue
        logical_lines.append(pending)
        pending = ""
    if pending:
        errors.append("requirements lock ends with an incomplete continuation")

    locked: dict[str, str] = {}
    saw_binary_only = False
    for line in logical_lines:
        if line == "--only-binary=:all:":
            saw_binary_only = True
            continue
        tokens = line.split()
        if not tokens:
            continue
        requirement = REQUIREMENT_PATTERN.fullmatch(tokens[0])
        if requirement is None:
            errors.append(f"invalid or unpinned locked requirement: {tokens[0]}")
            continue
        package = requirement.group(1).casefold().replace("_", "-")
        version = requirement.group(2)
        if package in locked:
            errors.append(f"duplicate locked requirement: {package}")
        locked[package] = version
        hash_tokens = tokens[1:]
        if not hash_tokens or any(HASH_PATTERN.fullmatch(token) is None for token in hash_tokens):
            errors.append(f"locked requirement lacks only SHA-256 hashes: {package}")

    if not saw_binary_only:
        errors.append("requirements lock must enforce binary-only packages")

    for raw_line in direct_text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        requirement = REQUIREMENT_PATTERN.fullmatch(line)
        if requirement is None:
            errors.append(f"invalid direct development requirement: {line}")
            continue
        package = requirement.group(1).casefold().replace("_", "-")
        if package not in locked:
            errors.append(f"direct development requirement is absent from lock: {package}")
        elif locked[package] != requirement.group(2):
            errors.append(f"direct development requirement version differs from lock: {package}")
    return errors


def validate_cmake_presets(cmake_text: str, presets: dict[str, object]) -> list[str]:
    errors: list[str] = []
    match = CMAKE_MINIMUM_PATTERN.search(cmake_text)
    if match is None:
        return ["CMakeLists.txt does not declare a minimum CMake version"]
    project_minimum = tuple(int(part or 0) for part in match.groups())
    declared = presets.get("cmakeMinimumRequired")
    if not isinstance(declared, dict):
        return ["CMakePresets.json lacks cmakeMinimumRequired"]
    preset_minimum = tuple(
        int(declared.get(part, 0)) for part in ("major", "minor", "patch")
    )
    if preset_minimum != project_minimum:
        errors.append("CMake minimum version differs between project and presets")

    preset_schema_minimums = {1: (3, 19, 0), 2: (3, 20, 0), 3: (3, 21, 0)}
    version = presets.get("version")
    schema_minimum = preset_schema_minimums.get(version) if isinstance(version, int) else None
    if schema_minimum is None:
        errors.append("CMake preset schema version is not policy-approved")
    elif preset_minimum < schema_minimum:
        errors.append("CMake preset schema requires a newer CMake than declared")
    return errors


def validate_dependency_lock(lock: dict[str, object]) -> list[str]:
    """Enforce relationships that JSON Schema cannot express by itself."""
    repositories = lock.get("repositories")
    if not isinstance(repositories, list):
        return ["dependency lock repositories must be an array"]
    seen: set[str] = set()
    errors: list[str] = []
    for index, repository in enumerate(repositories):
        if not isinstance(repository, dict):
            errors.append(f"dependency repository {index} must be an object")
            continue
        repository_id = repository.get("id")
        if not isinstance(repository_id, str):
            errors.append(f"dependency repository {index} has no string id")
        elif repository_id in seen:
            errors.append(f"duplicate dependency repository id: {repository_id}")
        else:
            seen.add(repository_id)
    return errors


def validate_phase3_manifests(
    rsp_manifest: object,
    rsp_schema: object,
    runtime_matrix: object,
    runtime_schema: object,
    dependency_lock: object,
) -> list[str]:
    """Run the Phase 3 semantic gates used by the standalone validators."""
    errors = [
        f"RSP manifest: {error}"
        for error in validate_rsp_manifest_document(rsp_manifest, rsp_schema)
    ]
    errors.extend(
        f"RSP manifest: {error}"
        for error in validate_rsp_pins(rsp_manifest, dependency_lock)
    )
    errors.extend(
        f"runtime matrix: {error}"
        for error in validate_runtime_matrix_document(runtime_matrix, runtime_schema)
    )
    errors.extend(
        f"runtime matrix: {error}"
        for error in validate_runtime_pins(runtime_matrix, dependency_lock)
    )
    return sorted(set(errors))


def validate_phase4_manifest(manifest: object, schema: object) -> list[str]:
    """Validate a public non-claiming record; completion uses the trusted API."""
    return sorted(
        {
            f"Phase 4 manifest: {error}"
            for error in validate_phase4_manifest_document(manifest, schema)
        }
    )


def validate_graphics_bridge(
    manifest: object, schema: object, dependency_lock: object
) -> list[str]:
    """Run the fail-closed synthetic graphics boundary contract."""
    errors = {
        f"graphics bridge: {error}"
        for error in validate_graphics_manifest_document(manifest, schema)
    }
    errors.update(
        f"graphics bridge: {error}"
        for error in validate_graphics_pins(manifest, dependency_lock)
    )
    return sorted(errors)


def validate_audio_bridge(
    manifest: object, schema: object, dependency_lock: object
) -> list[str]:
    """Run the fail-closed synthetic audio boundary contract."""
    errors = {
        f"audio bridge: {error}"
        for error in validate_audio_manifest_document(manifest, schema)
    }
    errors.update(
        f"audio bridge: {error}"
        for error in validate_audio_pins(manifest, dependency_lock)
    )
    return sorted(errors)


def load_json(path: Path) -> object:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(
            stream,
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_nonstandard_constant,
        )


def validate_instance(instance_path: Path, schema_path: Path) -> list[str]:
    schema = load_json(schema_path)
    instance = load_json(instance_path)
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)
    if any(validator.iter_errors(instance)):
        return [f"{instance_path.relative_to(ROOT)}: schema contract violation"]
    return []


def validate_json_documents() -> list[str]:
    errors: list[str] = []
    schema_directory = ROOT / "schemas"
    for schema_path in sorted(schema_directory.glob("*.schema.json")):
        try:
            Draft202012Validator.check_schema(load_json(schema_path))
        except Exception:
            errors.append(f"{schema_path.relative_to(ROOT)}: invalid schema contract")

    mappings = [
        (
            ROOT / "dependencies.lock.json",
            schema_directory / "dependency-lock.schema.json",
        ),
        (
            ROOT / "symbols" / "provenance.json",
            schema_directory / "provenance.schema.json",
        ),
        (
            ROOT / "docs" / "upstream" / "phase1-build-evidence.json",
            schema_directory / "phase1-build-evidence.schema.json",
        ),
        (
            ROOT / "docs" / "feasibility" / "phase3-cpu-overlay-evidence.json",
            schema_directory / "phase3-cpu-overlay-evidence.schema.json",
        ),
        (
            ROOT / "examples" / "task-packet.example.json",
            schema_directory / "task-packet.schema.json",
        ),
        (
            ROOT / "examples" / "patch-manifest.example.json",
            schema_directory / "patch-manifest.schema.json",
        ),
        (
            ROOT / "examples" / "phase4-generated-manifest.example.json",
            schema_directory / "phase4-generated-manifest.schema.json",
        ),
        (
            ROOT / "evidence" / "phase4-completion.json",
            schema_directory / "phase4-generated-manifest.schema.json",
        ),
        (
            ROOT / "config" / "phase4-completion-signing.json",
            schema_directory / "phase4-completion-signing.schema.json",
        ),
        (
            ROOT / "config" / "audio-task-bridge.json",
            schema_directory / "audio-task-bridge.schema.json",
        ),
        (
            ROOT / "config" / "graphics-task-bridge.json",
            schema_directory / "graphics-task-bridge.schema.json",
        ),
    ]
    mappings.extend(
        (profile, schema_directory / "execution-profile.schema.json")
        for profile in sorted((ROOT / "config" / "profiles").glob("*.json"))
    )

    for instance_path, schema_path in mappings:
        try:
            errors.extend(validate_instance(instance_path, schema_path))
        except (OSError, ValueError, json.JSONDecodeError):
            errors.append(f"{instance_path.relative_to(ROOT)}: input could not be read")

    return errors


def validate_cross_file_contracts() -> list[str]:
    errors: list[str] = []
    lock = load_json(ROOT / "dependencies.lock.json")
    provenance = load_json(ROOT / "symbols" / "provenance.json")
    assert isinstance(lock, dict)
    assert isinstance(provenance, dict)

    errors.extend(validate_dependency_lock(lock))
    errors.extend(
        validate_phase3_manifests(
            load_json(ROOT / "config" / "rsp-task-manifest.json"),
            load_json(ROOT / "schemas" / "rsp-task-manifest.schema.json"),
            load_json(ROOT / "config" / "runtime-capability-matrix.json"),
            load_json(ROOT / "schemas" / "runtime-capability-matrix.schema.json"),
            lock,
        )
    )
    errors.extend(
        validate_phase4_manifest(
            load_json(ROOT / "examples" / "phase4-generated-manifest.example.json"),
            load_json(ROOT / "schemas" / "phase4-generated-manifest.schema.json"),
        )
    )
    errors.extend(
        f"tracked Phase 4 completion: {error}"
        for error in validate_phase4_public_completion(
            load_json(ROOT / "evidence" / "phase4-completion.json"),
            load_json(ROOT / "schemas" / "phase4-generated-manifest.schema.json"),
        )
    )
    errors.extend(
        validate_audio_bridge(
            load_json(ROOT / "config" / "audio-task-bridge.json"),
            load_json(ROOT / "schemas" / "audio-task-bridge.schema.json"),
            lock,
        )
    )
    errors.extend(
        validate_graphics_bridge(
            load_json(ROOT / "config" / "graphics-task-bridge.json"),
            load_json(ROOT / "schemas" / "graphics-task-bridge.schema.json"),
            lock,
        )
    )
    phase7_contract = load_json(ROOT / "config" / "phase7-acceptance.json")
    phase7_completion_path = ROOT / "evidence" / "phase7-completion.json"
    errors.extend(
        f"Phase 7 contract: {error}"
        for error in validate_phase7_contract(
            phase7_contract,
            load_json(ROOT / "schemas" / "phase7-acceptance.schema.json"),
            lock,
            completion_summary_exists=phase7_completion_path.is_file(),
        )
    )
    if phase7_completion_path.is_file():
        errors.extend(
            f"Phase 7 completion: {error}"
            for error in validate_phase7_completion(
                load_json(phase7_completion_path), phase7_contract
            )
        )
    phase8_contract = load_json(ROOT / "config" / "phase8-acceptance.json")
    phase8_completion_path = ROOT / "evidence" / "phase8-completion.json"
    errors.extend(
        f"Phase 8 contract: {error}"
        for error in validate_phase8_contract(
            phase8_contract,
            load_json(ROOT / "schemas" / "phase8-acceptance.schema.json"),
            completion_summary_exists=phase8_completion_path.is_file(),
        )
    )
    if phase8_completion_path.is_file():
        errors.extend(
            f"Phase 8 completion: {error}"
            for error in validate_phase8_completion(
                load_json(phase8_completion_path), phase8_contract
            )
        )

    locked = {
        item["id"]: item["commit"]
        for item in lock["repositories"]
    }
    expected = {
        "jfg-decomp": provenance["repositories"]["jfg_decomp"],
        "n64recomp": provenance["repositories"]["n64recomp"],
        "n64modernruntime": provenance["repositories"]["n64modernruntime"],
        "rt64": provenance["repositories"]["rt64"],
    }
    for dependency, commit in expected.items():
        if locked.get(dependency) != commit:
            errors.append(f"dependency pin mismatch for {dependency}")

    profile_ids: set[str] = set()
    for path in sorted((ROOT / "config" / "profiles").glob("*.json")):
        profile = load_json(path)
        assert isinstance(profile, dict)
        profile_id = profile["id"]
        if profile_id in profile_ids:
            errors.append(f"duplicate execution profile id: {profile_id}")
        profile_ids.add(profile_id)

    if "compatibility" not in profile_ids or "deterministic-compatibility" not in profile_ids:
        errors.append("required compatibility profiles are missing")

    workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
    github_actions = lock["github_actions"]
    assert isinstance(github_actions, dict)
    errors.extend(validate_workflow(workflow, github_actions))
    errors.extend(
        validate_hashed_requirements(
            (ROOT / "requirements-dev.lock.txt").read_text(encoding="utf-8"),
            (ROOT / "requirements-dev.txt").read_text(encoding="utf-8"),
        )
    )
    presets = load_json(ROOT / "CMakePresets.json")
    assert isinstance(presets, dict)
    errors.extend(
        validate_cmake_presets(
            (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"), presets
        )
    )
    phase1_evidence = load_json(
        ROOT / "docs" / "upstream" / "phase1-build-evidence.json"
    )
    assert isinstance(phase1_evidence, dict)
    try:
        validate_aggregate_evidence(phase1_evidence, require_success=True)
    except ValueError as error:
        errors.append(f"Phase 1 aggregate evidence is invalid: {error}")
    phase3_evidence = load_json(
        ROOT / "docs" / "feasibility" / "phase3-cpu-overlay-evidence.json"
    )
    assert isinstance(phase3_evidence, dict)
    try:
        validate_phase3_evidence(phase3_evidence)
    except (KeyError, TypeError, ValueError) as error:
        errors.append(f"Phase 3 aggregate evidence is invalid: {error}")
    return errors


def main() -> int:
    errors = validate_json_documents()
    try:
        try:
            from .native_generation_metadata import default_bundle
        except ImportError:
            from native_generation_metadata import default_bundle
        default_bundle(ROOT)
    except (OSError, ValueError, KeyError, TypeError):
        errors.append("native generation metadata failed its pinned facts-only contract")
    if not errors:
        try:
            errors.extend(validate_cross_file_contracts())
            errors.extend(check_repository(ROOT, history=False))
        except (OSError, ValueError, json.JSONDecodeError, KeyError, TypeError):
            errors.append("project contract inputs could not be validated")

    if errors:
        print("Project validation failed:", file=sys.stderr)
        for error in sorted(set(errors)):
            print(f"- {error}", file=sys.stderr)
        return 1
    print("Project validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
