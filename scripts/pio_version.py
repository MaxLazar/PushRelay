"""Injects FIRMWARE_VERSION / FIRMWARE_GIT_SHA as build-time defines.

Version scheme (see README "Releases & versioning"):
  * MAJOR.MINOR come from version.txt, bumped by hand.
  * PATCH is the number of commits since version.txt last changed, so it
    resets to a small number every time MINOR is bumped and is otherwise
    monotonic on a linear main history.
  * CI passes PUSHRELAY_VERSION so the built firmware reports exactly the
    tag that gets published; local builds compute it from git.
"""
import os
import subprocess

Import("env")  # noqa: F821 - provided by PlatformIO/SCons

project_dir = env["PROJECT_DIR"]  # noqa: F821


def _git(*args):
    return (
        subprocess.check_output(["git", *args], cwd=project_dir, stderr=subprocess.DEVNULL)
        .decode()
        .strip()
    )


def _read_base():
    try:
        with open(os.path.join(project_dir, "version.txt"), "r", encoding="utf-8") as fh:
            return fh.read().strip() or "0.0"
    except OSError:
        return "0.0"


def _compute_version():
    override = os.environ.get("PUSHRELAY_VERSION")
    if override:
        return override.lstrip("v")
    base = _read_base()
    try:
        last = _git("log", "-1", "--format=%H", "--", "version.txt")
        rng = f"{last}..HEAD" if last else "HEAD"
        patch = _git("rev-list", "--count", rng)
    except Exception:
        return f"{base}.0-dev"
    return f"{base}.{patch}"


def _compute_sha():
    try:
        return _git("rev-parse", "--short", "HEAD")
    except Exception:
        return "nogit"


version = _compute_version()
sha = _compute_sha()
print(f"PushRelay firmware version: {version} ({sha})")

env.Append(  # noqa: F821
    CPPDEFINES=[
        ("FIRMWARE_VERSION", env.StringifyMacro(version)),  # noqa: F821
        ("FIRMWARE_GIT_SHA", env.StringifyMacro(sha)),  # noqa: F821
    ]
)
