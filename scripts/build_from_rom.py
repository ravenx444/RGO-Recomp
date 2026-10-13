#!/usr/bin/env python3
"""Experimental Windows ROM-to-native build. All generated files stay local."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid
from build_paths import windows_cache, check_binary_path
from runtime_identity import write_identity
import rgo_build_cache as cache_tools

ROOT = Path(__file__).resolve().parents[1]
ROM_SHA1 = "493ced9008dbe932d6e91179b68e8630cf23a023"
NAMES = {"jfg-decomp": "Jet-Force-Gemini", "n64recomp": "N64Recomp",
         "n64modernruntime": "N64ModernRuntime", "rt64": "rt64"}


def validate_rom(path: Path) -> str:
    if not path.is_file() or path.stat().st_size != 32 * 1024 * 1024:
        raise ValueError("Select the supported 32 MiB US big-endian .z64 ROM.")
    digest = hashlib.sha1(path.read_bytes()).hexdigest()
    if digest != ROM_SHA1:
        raise ValueError("ROM checksum does not match the supported US revision.")
    return digest


def wsl_path(path: Path) -> str:
    # This prototype supports local drive paths only; no shell interpolation.
    path = path.resolve()
    if len(path.drive) != 2 or path.drive[1] != ":":
        raise ValueError("Use a local Windows drive, not a network share.")
    return "/mnt/" + path.drive[0].lower() + path.as_posix()[2:]


def find_cmake() -> str:
    found = shutil.which("cmake")
    if found:
        return found
    base = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"))
    vswhere = base / "Microsoft Visual Studio/Installer/vswhere.exe"
    if vswhere.is_file():
        found = subprocess.check_output([str(vswhere), "-latest", "-products", "*",
            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-find", "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"],
            text=True).strip().splitlines()
        if found:
            return found[0]
    raise ValueError("Install Visual Studio 2022 C++ desktop tools with CMake support.")


# RGO: source builds also build the unified frontend (RGO-Launcher.exe), so
# --play gets the same menu bar and settings as the release launcher.
FRONTEND_MUTEX = "Local\\JFGUnifiedFrontend"


def frontend_build_command(root: Path, output: Path) -> list[str]:
    powershell = Path(os.environ.get("WINDIR", "C:/Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    return [str(powershell), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
            "-File", str(root / "scripts/build_launcher.ps1"),
            "-OutputDirectory", str(output), "-NativeBuildDirectory", str(output / "n")]


def write_profile_text(profile: Path, name: str, value: str) -> None:
    # Same format as the frontend: UTF-8 without BOM or final line break.
    temporary = profile / (name + ".tmp")
    temporary.write_bytes(value.encode("utf-8"))
    os.replace(temporary, profile / name)


def select_frontend_build(profile: Path, rom: Path, executable: Path) -> None:
    """Point the frontend's profile at this ROM and the game build just made."""
    profile.mkdir(parents=True, exist_ok=True)
    write_profile_text(profile, "frontend-rom.txt", str(rom))
    write_profile_text(profile, "frontend-runtime.txt", str(executable))


def frontend_play_command(launcher: Path, profile: Path) -> list[str]:
    return [str(launcher), "--play", "--profile", str(profile)]


def frontend_environment(source: dict[str, str], root: Path) -> dict[str, str]:
    # Inherited JFG_* variables must never reach the game (replays, captures).
    environment = {k: v for k, v in source.items() if not k.upper().startswith("JFG_")}
    # The frontend's "Set up and build" rebuilds from this checkout, not from
    # the upstream source download a release launcher uses.
    environment["JFG_SOURCE_ROOT"] = str(root)
    return environment


def frontend_is_open() -> bool:
    if os.name != "nt":
        return False
    import ctypes
    synchronize = 0x00100000
    handle = ctypes.windll.kernel32.OpenMutexW(synchronize, False, FRONTEND_MUTEX)
    if handle:
        ctypes.windll.kernel32.CloseHandle(handle)
    return bool(handle)


def plan_steps(deps: Path, lock: dict, own_dependencies: bool, generate: bool, frontend: bool) -> list[str]:
    """Step names in the order main() runs them, for "Step N of M"."""
    steps = []
    entries = [entry for entry in lock["repositories"] if entry["id"] in NAMES]
    if own_dependencies:
        for entry in entries:
            if not (deps / NAMES[entry["id"]]).exists():
                steps += ["clone-" + entry["id"], "pin-" + entry["id"]]
        steps += ["submodules-" + entry["id"] for entry in entries]
    if generate:
        steps.append("generate")
    steps += ["configure", "compile"]
    if frontend:
        steps.append("frontend")
    return steps


def executable_in_use(executable: Path) -> bool:
    """True when a running process holds the previous build's executable open."""
    if os.name != "nt" or not executable.is_file():
        return False
    try:
        with executable.open("r+b"):
            return False
    except PermissionError:
        return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--distro", default="Ubuntu-24.04")
    parser.add_argument("--jobs", type=int, help="Build parallelism (default: all logical cores, "
                        "capped at one job per 1.5 GB of RAM)")
    parser.add_argument("--dependency-root", type=Path,
                        help="Optional existing pinned upstream source/tool cache")
    parser.add_argument("--build-root", type=Path, help="Optional short writable Windows build-cache folder")
    parser.add_argument("--play", action="store_true",
                        help="Start the game through the frontend (menu bar, Video settings) after building")
    parser.add_argument("--no-frontend", action="store_true",
                        help="Skip building RGO-Launcher.exe; with --play, start the bare game window")
    parser.add_argument("--regenerate", action="store_true",
                        help="Generate again from the ROM even if a cached generation matches "
                             "(the game code is then compiled from zero too)")
    parser.add_argument("--clean", action="store_true",
                        help="Reuse nothing: regenerate, compile from zero and rebuild the frontend")
    parser.add_argument("--keep-old-builds", action="store_true",
                        help="Do not delete older build folders of this checkout")
    parser.add_argument("--resume-native", type=Path,
                        help="Resume compilation in a specific local build whose ROM generation completed")
    parser.add_argument("--generation-timeout", type=int, default=3600,
                        help="Hard limit for the Linux subprocess group, in seconds")
    args = parser.parse_args()
    if os.name != "nt" or sys.version_info < (3, 11):
        parser.error("This prototype requires Windows x64 and Python 3.11 or newer.")
    if args.jobs is None:
        args.jobs = cache_tools.default_jobs(os.cpu_count(), cache_tools.total_memory())
    if not 1 <= args.jobs <= cache_tools.MAX_JOBS:
        parser.error(f"--jobs must be between 1 and {cache_tools.MAX_JOBS}")
    if not 1 <= args.generation_timeout <= 7200:
        parser.error("--generation-timeout must be between 1 and 7200")
    if args.resume_native and (args.clean or args.regenerate):
        parser.error("--resume-native cannot be combined with --clean or --regenerate")
    rom = args.rom.resolve()
    rom_sha1 = validate_rom(rom)  # Reject before downloads or build output.
    if not args.no_frontend and frontend_is_open():
        raise ValueError("Close the RGO launcher (RGO-Launcher.exe) before a source build, or pass --no-frontend.")
    cmake = find_cmake()
    for tool in ("git", "wsl"):
        if not shutil.which(tool):
            raise ValueError(f"Required tool missing: {tool}")
    cache = windows_cache(ROOT, args.build_root)
    deps = (args.dependency_root or cache / "d").resolve()
    # The public lock file supplies repository URLs and immutable revisions.
    lock = json.loads((ROOT / "dependencies.lock.json").read_text())
    inputs = cache_tools.generation_inputs(ROOT, rom_sha1, args.distro)
    reused = False
    if args.resume_native:
        workspace = args.resume_native.resolve()
        if not any(workspace.is_relative_to(parent.resolve()) for parent in [cache / "w", ROOT / "tools/private/local-builds"]):
            raise ValueError("Resume requires this checkout's build cache or its legacy tools/private/local-builds folder.")
        problem = cache_tools.verify_generation(workspace, ROM_SHA1)
        if problem:
            raise ValueError(f"Cannot resume: {problem}; start a new build.")
        print("Resuming the given build without checking whether the generator changed.", flush=True)
        reused = True
    else:
        decision = (cache_tools.Decision(None, "Regenerating: requested with "
                                         + ("--clean" if args.clean else "--regenerate"))
                    if args.clean or args.regenerate else cache_tools.choose_workspace(cache, inputs))
        print(decision.message, flush=True)
        if decision.workspace is not None:
            workspace, reused = decision.workspace, True
        else:
            workspace = cache / "w" / uuid.uuid4().hex[:12]
            workspace.mkdir(parents=True, exist_ok=False)
    workspace_lock = cache_tools.WorkspaceLock(workspace)
    if not workspace_lock.acquire():
        raise ValueError("Another build of this checkout is using " + str(workspace)
                         + ". Wait for it to finish, then try again.")
    if reused:
        if not args.resume_native:
            cache_tools.mark_used(workspace)
        refreshed = cache_tools.refresh_copied_files(ROOT, workspace)
        if refreshed:
            print("Updated in the cached build: " + ", ".join(refreshed), flush=True)
    build = check_binary_path(workspace / ("native" if args.resume_native and (workspace / "native").exists() else "n"))
    executable = build / "Release/jfg-native-boot.exe"
    if executable_in_use(executable):
        raise ValueError("The game from the previous build is still running. Close it, then build again.")
    print("Local build workspace: " + str(workspace), flush=True)
    print(f"Build parallelism: {args.jobs} jobs", flush=True)
    status_file = workspace / "status.json"
    records: list[dict] = (json.loads(status_file.read_text()).get("steps", [])
                           if args.resume_native and status_file.is_file() else [])
    status_file.write_text(json.dumps({"complete": False, "steps": records}, indent=2) + "\n")
    steps = plan_steps(deps, lock, not args.dependency_root, not reused, not args.no_frontend)
    started_build = time.monotonic()

    def run(name: str, command: list[str], cwd: Path = ROOT) -> None:
        number = steps.index(name) + 1 if name in steps else len(steps)
        print(f"Step {number} of {len(steps)}: {name}", flush=True)
        print("JFG-SUPPORT setup_step=" + name, flush=True)
        started = time.monotonic()
        log = workspace / (name + ".log")
        if log.exists():
            attempt = 1
            while (workspace / f"{name}.{attempt}.log").exists():
                attempt += 1
            log.rename(workspace / f"{name}.{attempt}.log")
        with log.open("w", encoding="utf-8", buffering=1) as stream:
            with subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, errors="replace") as process:
                for line in process.stdout:
                    stream.write(line)
                    # Emit only stable diagnostics; raw compiler output stays local.
                    for token, category in {"Filename too long": "path-too-long", "exceeds the OS max path limit": "path-too-long", "Permission denied": "permission-denied", "Could not resolve": "dns-failed", "No space left": "disk-full", "CMake Error": "cmake-failed", "0x80370102": "virtualization-unavailable"}.items():
                        if token.lower() in line.lower():
                            print("JFG-SUPPORT setup_error=" + category, flush=True)
                    for error_code in re.findall(r"\b(?:error|fatal error) (C[0-9]{4}|LNK[0-9]{4}|MSB[0-9]{4})\b", line, re.I):
                        print("JFG-SUPPORT compiler_error=" + error_code.lower(), flush=True)
                    if name == "generate" and re.fullmatch(r"[a-z][a-z-]+\n?", line):
                        print("  " + line.strip(), flush=True)
                code = process.wait()
        seconds = time.monotonic() - started
        records.append({"step": name, "exit_code": code, "seconds": round(seconds, 3)})
        (workspace / "status.json").write_text(json.dumps(
            {"complete": False, "steps": records}, indent=2) + "\n")
        print("JFG-SUPPORT setup_exit=0x" + format(code & 0xffffffff, "08x"), flush=True)
        if code:
            raise RuntimeError(f"{name} failed. Local diagnostic log: {log}")
        if seconds >= 60:
            print(f"  {name} took {seconds / 60:.0f} min", flush=True)

    if not args.dependency_root:
        for entry in lock["repositories"]:
            if entry["id"] not in NAMES:
                continue
            dest = deps / NAMES[entry["id"]]
            if not dest.exists():
                dest.parent.mkdir(parents=True, exist_ok=True)
                run("clone-" + entry["id"], ["git", "clone", "--config", "core.autocrlf=false", "--config", "core.longpaths=true", "--no-checkout", entry["url"], str(dest)])
                run("pin-" + entry["id"], ["git", "-C", str(dest), "checkout", "--detach", entry["commit"]])

    for entry in lock["repositories"]:
        if entry["id"] in NAMES:
            dest = deps / NAMES[entry["id"]]
            head = subprocess.check_output(["git", "-C", str(dest), "rev-parse", "HEAD"], text=True).strip()
            if head != entry["commit"]:
                raise ValueError(f"Dependency revision mismatch: {entry['id']}; existing files were preserved.")
            if not args.dependency_root:
                run("submodules-" + entry["id"], ["git", "-c", "core.autocrlf=false", "-c", "core.longpaths=true", "-C", str(dest), "submodule", "update", "--init", "--recursive"])

    if not reused:
        run("generate", ["wsl", "-d", args.distro, "--exec", "timeout", "--kill-after=3s",
            str(args.generation_timeout) + "s", "python3",
            wsl_path(ROOT / "scripts/generate_from_rom.py"), "--rom", wsl_path(rom),
            "--workspace", wsl_path(workspace), "--dependency-root", wsl_path(deps),
            "--jobs", str(args.jobs)])
        problem = cache_tools.verify_generation(workspace, ROM_SHA1)
        if problem:
            raise RuntimeError("Generation finished but " + problem + ".")
        # Only now is this workspace a reusable cache entry.
        cache_tools.write_key(workspace, inputs)
    generated = workspace / "normalized"
    # Configure runs every time, so changed CMakeLists and added or removed
    # source files are picked up; MSBuild then recompiles only what changed.
    run("configure", [cmake, "-S", str(ROOT), "-B", str(build),
        "-G", "Visual Studio 17 2022", "-A", "x64", "-DBUILD_TESTING=OFF", "-DJFG_SUPPORT_SYMBOLS=ON",
        "-DJFG_MSVC_GENERATED_COMPILE_JOBS=" + str(min(args.jobs, 32)),
        "-DJFG_ENABLE_GENERATED_CODE=ON", "-DJFG_BUILD_PHASE6_NATIVE_BOOT=ON",
        "-DJFG_BUILD_PHASE8_LIVE=ON", "-DJFG_ENABLE_RT64=ON",
        "-DJFG_GENERATED_ROOT=" + str(generated),
        "-DJFG_PHASE6_LIBULTRA_IDENTIFICATION=" + str(workspace / "libultra.json"),
        "-DJFG_PHASE8_PRIVATE_AUDIO_ADAPTER_ROOT=" + str(workspace / "audio"),
        "-DJFG_PHASE8_PRIVATE_AUDIO_RUNTIME_INCLUDE=" + str(deps / "N64ModernRuntime/librecomp/include"),
        "-DJFG_RT64_ROOT=" + str(deps / "rt64")])
    run("compile", [cmake, "--build", str(build), "--config", "Release",
        "--target", "jfg-native-boot", "--parallel", str(args.jobs)])
    if not executable.is_file():
        raise RuntimeError("Build finished without the native executable.")
    (workspace / "status.json").write_text(json.dumps({"complete": True,
        "scope": "Compilation only; gameplay acceptance is separate",
        "rom_sha1": ROM_SHA1, "executable": str(executable), "steps": records}, indent=2) + "\n")
    write_identity(executable, ROOT)
    print("Built: " + str(executable), flush=True)
    print("Keep this build local. It contains code generated from your ROM.", flush=True)
    if not args.keep_old_builds and not args.resume_native:
        cache_tools.clean_old_workspaces(cache, workspace, log=lambda text: print(text, flush=True))
    launcher = None
    if not args.no_frontend:
        # ROM-free and reused between builds, so it lives beside the workspaces.
        frontend = check_binary_path(cache / "f")
        if args.clean and frontend.exists():
            cache_tools.delete_tree(frontend)
        run("frontend", frontend_build_command(ROOT, frontend))
        launcher = frontend / "RGO-Launcher.exe"
        if not launcher.is_file():
            raise RuntimeError("Frontend build finished without RGO-Launcher.exe.")
        print("Frontend: " + str(launcher), flush=True)
    print(f"Build finished in {(time.monotonic() - started_build) / 60:.0f} min", flush=True)
    # The build is done; a rebuild started from the frontend must not wait on it.
    workspace_lock.release()
    if args.play:
        profile = Path(os.environ["LOCALAPPDATA"]) / "JFGRecomp/profiles/default"
        profile.mkdir(parents=True, exist_ok=True)
        if launcher:
            select_frontend_build(profile, rom, executable)
            return subprocess.call(frontend_play_command(launcher, profile), cwd=launcher.parent,
                                   env=frontend_environment(dict(os.environ), ROOT))
        environment = {k: v for k, v in os.environ.items() if not k.startswith("JFG_")}
        return subprocess.call([str(executable), "--rom", str(rom), "--save",
            str(profile / "jfg.flash"), "--controller-pak", str(profile / "controller-1.pak"),
            "--play"], cwd=executable.parent, env=environment)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
