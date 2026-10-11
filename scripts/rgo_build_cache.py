"""RGO: reuse ROM generation and the native build directory between source builds.

`build_from_rom.py` used to regenerate everything from the ROM and compile from
zero on every run. Generation depends only on the ROM, the pinned upstream
revisions, and the generator scripts and data in this checkout, so a workspace
whose generation was made from identical inputs can be reused, together with
its Visual Studio build directory (MSBuild then recompiles only what changed).

Everything here is plain Python so it can be unit-tested on Linux.
"""
from __future__ import annotations

import ast
from dataclasses import dataclass
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys

# Bump to invalidate every cached generation (for example when the meaning of
# the key changes or a generator input is found that the trace below misses).
CACHE_SCHEMA = 1
KEY_FILE = "generation-key.json"
LOCK_FILE = "build.lock"
WORKSPACE_NAME = re.compile(r"[0-9a-f]{12}")

GENERATOR = "scripts/generate_from_rom.py"
# Dependencies the WSL generation step clones and builds. N64ModernRuntime and
# RT64 are only compiled by MSBuild, which tracks their files itself.
GENERATION_DEPENDENCIES = ("jfg-decomp", "n64recomp")
# Repository data the generator scripts read, traced by hand from the Python
# import closure of the generator (see tests/test_rgo_build_cache.py, which
# fails when a closure script starts referring to a repository path that is not
# listed here or in COPIED_FILES).
GENERATION_DATA = ("dependencies.lock.json", "patches/n64recomp")
# Files the generator copies byte for byte from the checkout into the workspace.
# They are compiled by MSBuild, not transformed, so instead of regenerating when
# they change they are refreshed in a reused workspace before every build.
# Workspace path -> checkout path.
COPIED_FILES = {
    "audio/brokered-rsp.hpp": "src/bootstrap/brokered-rsp.hpp",
    "audio/production-audio-adapter.cpp": "src/bootstrap/production-audio-adapter.cpp",
    "normalized/jfg_original_timing_fast.h": "include/jfg/boot/original_timing_fast.h",
}
# The files generate_from_rom.py lists in generation.json.
EXPECTED_GENERATION_FILES = frozenset({
    "normalized/sources.json", "libultra.json", "audio/production-audio-adapter.cpp",
    "audio/brokered-rsp.hpp", "audio/brokered-jfg-audio-probe.cpp", "audio/jfg-audio-probe.cpp",
    "normalized/jfg_original_timing.h", "normalized/original-timing.json",
    "normalized/jfg_original_timing_fast.h", "normalized/funcs.h"})


def sha256_file(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _local_module(root: Path, name: str) -> str | None:
    name = name.removeprefix("scripts.").split(".")[0]
    return name if name and (root / "scripts" / (name + ".py")).is_file() else None


def generator_scripts(root: Path) -> list[str]:
    """The generator plus every scripts/*.py it imports or runs, transitively.

    Follows `import x`, `from x import y`, `from .x import y`, `-m scripts.x`
    and path strings such as "scripts/x.py". Over-inclusion only costs an
    unneeded regeneration; under-inclusion would reuse stale output.
    """
    seen: set[str] = set()
    pending = [Path(GENERATOR).stem]
    while pending:
        module = pending.pop()
        if module in seen:
            continue
        seen.add(module)
        tree = ast.parse((root / "scripts" / (module + ".py")).read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            names: list[str] = []
            if isinstance(node, ast.Import):
                names = [alias.name for alias in node.names]
            elif isinstance(node, ast.ImportFrom):
                base = node.module or ""
                names = [base] + [(base + "." if base else "") + alias.name for alias in node.names]
            elif isinstance(node, ast.Constant) and isinstance(node.value, str):
                match = re.fullmatch(r"scripts[./]([A-Za-z_][A-Za-z0-9_]*)(?:\.py)?", node.value)
                if match:
                    names = [match.group(1)]
            for name in names:
                local = _local_module(root, name)
                if local:
                    pending.append(local)
    return sorted("scripts/" + name + ".py" for name in seen)


def _files_under(root: Path, relative: str) -> list[str]:
    path = root / relative
    if path.is_file():
        return [relative]
    if not path.is_dir():
        raise ValueError("Generation input is missing: " + relative)
    return sorted(p.relative_to(root).as_posix() for p in path.rglob("*")
                  if p.is_file() and "__pycache__" not in p.parts)


def generation_inputs(root: Path, rom_sha1: str, distro: str) -> dict[str, str]:
    """Name -> digest for everything the WSL generation step depends on."""
    inputs = {"cache-schema": str(CACHE_SCHEMA), "rom-sha1": rom_sha1, "wsl-distro": distro}
    lock = json.loads((root / "dependencies.lock.json").read_text(encoding="utf-8"))
    entries = {entry["id"]: entry for entry in lock["repositories"]}
    for dependency in GENERATION_DEPENDENCIES:
        # build_from_rom verifies that each checkout's HEAD is the locked commit.
        entry = json.dumps(entries[dependency], sort_keys=True)
        inputs["dependency:" + dependency] = hashlib.sha256(entry.encode("utf-8")).hexdigest()
    files = set(generator_scripts(root))
    for relative in GENERATION_DATA:
        files.update(_files_under(root, relative))
    for relative in sorted(files):
        # Normalize line endings so a CRLF checkout does not look like a change.
        data = (root / relative).read_bytes().replace(b"\r\n", b"\n")
        inputs[relative] = hashlib.sha256(data).hexdigest()
    return inputs


def generation_key(inputs: dict[str, str]) -> str:
    return hashlib.sha256(json.dumps(inputs, sort_keys=True).encode("utf-8")).hexdigest()


def describe_changes(old: dict[str, str], new: dict[str, str], limit: int = 4) -> str:
    labels = {"cache-schema": "build cache format", "rom-sha1": "ROM",
              "wsl-distro": "WSL distribution"}
    changed = []
    for name in sorted(set(old) | set(new)):
        if old.get(name) == new.get(name):
            continue
        label = labels.get(name, name.replace("dependency:", "pinned "))
        if name not in old:
            changed.append(label + " added")
        elif name not in new:
            changed.append(label + " removed")
        else:
            changed.append(label + " changed")
    if len(changed) > limit:
        changed = changed[:limit] + [f"and {len(changed) - limit} more"]
    return ", ".join(changed) or "inputs changed"


def verify_generation(workspace: Path, rom_sha1: str | None = None) -> str | None:
    """None when the workspace holds a complete, unmodified generation; else why not."""
    try:
        generation = json.loads((workspace / "generation.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return "ROM generation did not finish"
    from_checkout = set(COPIED_FILES)
    if (generation.get("complete") is not True or not isinstance(generation.get("files"), dict)
            or set(generation["files"]) != EXPECTED_GENERATION_FILES):
        return "ROM generation did not finish"
    if rom_sha1 is not None and generation.get("rom_sha1") != rom_sha1:
        return "it was generated from a different ROM"
    for name, digest in generation["files"].items():
        path = workspace / name
        if not path.is_file():
            return "generated file missing: " + name
        if name not in from_checkout and sha256_file(path) != digest:
            return "generated file changed: " + name
    return None


def refresh_copied_files(root: Path, workspace: Path) -> list[str]:
    """Copy changed checkout files into a reused workspace; return what changed.

    Unchanged files are left alone so MSBuild sees no new timestamp.
    """
    manifest_path = workspace / "generation.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    changed = []
    for inside, source in COPIED_FILES.items():
        data = (root / source).read_bytes()
        target = workspace / inside
        if target.is_file() and target.read_bytes() == data:
            continue
        temporary = target.with_name(target.name + ".tmp")
        temporary.write_bytes(data)
        os.replace(temporary, target)
        changed.append(source)
    for inside in COPIED_FILES:
        manifest["files"][inside] = sha256_file(workspace / inside)
    if changed:
        temporary = manifest_path.with_name(manifest_path.name + ".tmp")
        temporary.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        os.replace(temporary, manifest_path)
    return changed


def write_key(workspace: Path, inputs: dict[str, str]) -> None:
    record = {"schema": CACHE_SCHEMA, "key": generation_key(inputs),
              "created": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
              "inputs": inputs}
    temporary = workspace / (KEY_FILE + ".tmp")
    temporary.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, workspace / KEY_FILE)


def read_key(workspace: Path) -> dict | None:
    try:
        record = json.loads((workspace / KEY_FILE).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if (not isinstance(record, dict) or not isinstance(record.get("inputs"), dict)
            or record.get("key") != generation_key(record["inputs"])):
        return None
    return record


def mark_used(workspace: Path) -> None:
    """Make a reused workspace count as the most recently used one."""
    os.utime(workspace / KEY_FILE)


def list_workspaces(cache: Path) -> list[Path]:
    """This checkout's run workspaces (cache/w/<12 hex>), most recently used first."""
    parent = cache / "w"
    if not parent.is_dir():
        return []
    found = [p for p in parent.iterdir() if WORKSPACE_NAME.fullmatch(p.name)
             and p.is_dir() and not p.is_symlink() and not _is_junction(p)]
    return sorted(found, key=_age, reverse=True)


def _age(workspace: Path) -> float:
    # The key file is written when generation finishes and touched on every
    # reuse; fall back to the folder for workspaces without one.
    for path in (workspace / KEY_FILE, workspace):
        try:
            return path.stat().st_mtime
        except OSError:
            pass
    return 0.0


def _is_junction(path: Path) -> bool:
    is_junction = getattr(os.path, "isjunction", None)  # Python 3.12+
    if is_junction:
        return is_junction(path)
    try:
        return bool(os.lstat(path).st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)
    except (AttributeError, OSError):
        return False


@dataclass
class Decision:
    workspace: Path | None  # None: generate into a new workspace
    message: str


def choose_workspace(cache: Path, inputs: dict[str, str]) -> Decision:
    """Pick the newest complete workspace generated from these exact inputs."""
    key = generation_key(inputs)
    newest_other = None
    legacy = False
    for workspace in list_workspaces(cache):
        record = read_key(workspace)
        if record is None:
            legacy = legacy or (workspace / "generation.json").is_file()
            continue
        if verify_generation(workspace):
            continue
        if record["key"] != key:
            newest_other = newest_other or record
            continue
        created = str(record.get("created", "an earlier build"))[:16].replace("T", " ")
        return Decision(workspace, f"Reusing ROM generation from {created} (inputs unchanged)")
    if newest_other is not None:
        return Decision(None, "Regenerating: " + describe_changes(newest_other["inputs"], inputs))
    if legacy:
        return Decision(None, "Generating from the ROM: earlier builds were made before the build "
                              "cache existed, so they cannot be reused (this one can)")
    return Decision(None, "Generating from the ROM: no cached generation yet")


def select_for_cleanup(workspaces: list[Path], current: Path, is_reusable) -> list[Path]:
    """Workspaces to delete: all but `current` and the newest other reusable one.

    `workspaces` is newest first. If no other workspace is reusable, the newest
    other one is kept instead, so a failed regeneration never removes the last
    good build.
    """
    others = [w for w in workspaces if w != current]
    keep = next((w for w in others if is_reusable(w)), others[0] if others else None)
    return [w for w in others if w != keep]


class WorkspaceLock:
    """An OS file lock that is released automatically if the build process dies."""

    def __init__(self, workspace: Path):
        self.path = workspace / LOCK_FILE
        self.stream = None

    def acquire(self) -> bool:
        stream = open(self.path, "a+b")
        try:
            if os.name == "nt":
                import msvcrt
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            stream.close()
            return False
        self.stream = stream
        return True

    def release(self) -> None:
        if self.stream is None:
            return
        try:
            if os.name == "nt":
                import msvcrt
                self.stream.seek(0)
                msvcrt.locking(self.stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.stream.fileno(), fcntl.LOCK_UN)
        finally:
            self.stream.close()
            self.stream = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.release()


def workspace_in_use(workspace: Path) -> bool:
    if not (workspace / LOCK_FILE).exists():
        return False
    lock = WorkspaceLock(workspace)
    if not lock.acquire():
        return True
    lock.release()
    return False


def _long(path: Path) -> str:
    # Build trees nest deeply; use extended-length paths on Windows.
    text = str(path.resolve())
    if os.name == "nt" and not text.startswith("\\\\"):
        return "\\\\?\\" + text
    return text


def tree_size(path: Path) -> int:
    total = 0
    for directory, _, files in os.walk(_long(path)):
        for name in files:
            try:
                total += os.lstat(os.path.join(directory, name)).st_size
            except OSError:
                pass
    return total


def delete_tree(path: Path) -> None:
    def make_writable(function, name, *_):
        # Git objects and some tool outputs are read-only on Windows.
        os.chmod(name, stat.S_IWRITE)
        function(name)

    if sys.version_info >= (3, 12):
        shutil.rmtree(_long(path), onexc=make_writable)
    else:
        shutil.rmtree(_long(path), onerror=make_writable)


def remove_workspace(cache: Path, workspace: Path) -> int:
    """Delete one run workspace of this checkout; return the bytes freed."""
    parent = (cache / "w").resolve()
    if (workspace.parent.resolve() != parent or not WORKSPACE_NAME.fullmatch(workspace.name)
            or workspace.is_symlink() or _is_junction(workspace)):
        raise ValueError("Refusing to delete outside this checkout's build cache: " + str(workspace))
    lock = WorkspaceLock(workspace)
    if not lock.acquire():
        return 0  # Another build is using it.
    size = tree_size(workspace)
    lock.release()
    delete_tree(workspace)
    return size


def clean_old_workspaces(cache: Path, current: Path, log=print) -> int:
    def reusable(workspace: Path) -> bool:
        return read_key(workspace) is not None and verify_generation(workspace) is None

    freed = 0
    for workspace in select_for_cleanup(list_workspaces(cache), current, reusable):
        try:
            removed = remove_workspace(cache, workspace)
        except OSError as error:
            log(f"Could not delete old build {workspace.name}: {error}")
            continue
        if removed:
            freed += removed
        else:
            log(f"Kept old build {workspace.name}: another build is using it")
    if freed:
        log(f"Deleted old build folders, freed {freed / 2**30:.1f} GB")
    return freed


# Each parallel cl.exe on the large generated C files can use well over 1 GB.
# This is an estimate, not a measurement; it only matters on machines with
# many cores and little RAM (for example 16 threads and 16 GB).
MEMORY_PER_JOB = 3 * 2**30 // 2
MAX_JOBS = 64


def total_memory() -> int | None:
    if os.name == "nt":
        import ctypes

        class Status(ctypes.Structure):
            _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong),
                        ("total_physical", ctypes.c_ulonglong), ("available_physical", ctypes.c_ulonglong),
                        ("total_page", ctypes.c_ulonglong), ("available_page", ctypes.c_ulonglong),
                        ("total_virtual", ctypes.c_ulonglong), ("available_virtual", ctypes.c_ulonglong),
                        ("available_extended", ctypes.c_ulonglong)]
        status = Status()
        status.length = ctypes.sizeof(Status)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return int(status.total_physical)
        return None
    try:
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
    except (AttributeError, ValueError, OSError):
        return None


def default_jobs(cpu_count: int | None, memory: int | None) -> int:
    jobs = cpu_count or 4
    if memory:
        jobs = min(jobs, max(1, memory // MEMORY_PER_JOB))
    return max(1, min(jobs, MAX_JOBS))
