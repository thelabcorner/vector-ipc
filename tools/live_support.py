"""Shared staging utilities for live Illustrator ExternalObject probes."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import tempfile
import time

LIVE_ROOT_NAME = "vector-ipc-live"
STALE_AFTER_SECONDS = 24 * 60 * 60


def live_root() -> Path:
    """Return the user-writable directory used for disposable host-loaded DLLs."""
    root = Path(tempfile.gettempdir()) / LIVE_ROOT_NAME
    root.mkdir(parents=True, exist_ok=True)
    return root


def prune_stale_live_dlls(
    root: Path | None = None,
    *,
    older_than_seconds: int = STALE_AFTER_SECONDS,
) -> int:
    """Best-effort cleanup of old, no-longer-mapped probe DLLs."""
    target = root if root is not None else live_root()
    cutoff = time.time() - max(0, older_than_seconds)
    removed = 0

    try:
        entries = tuple(target.glob("*.dll"))
    except OSError:
        return 0

    for path in entries:
        try:
            if path.stat().st_mtime > cutoff:
                continue
            path.unlink()
            removed += 1
        except OSError:
            # Illustrator may intentionally keep ExternalObject modules mapped
            # until process exit. A sharing violation is expected and harmless.
            continue
    return removed


def stage_live_dll(source_dll: Path, prefix: str) -> tuple[Path, str, Path]:
    """Create a uniquely named, user-owned copy of an ExternalObject DLL."""
    if not source_dll.is_file():
        raise FileNotFoundError(source_dll)
    if not prefix or any(ch in prefix for ch in "\\/"):
        raise ValueError("prefix must be a non-empty filename stem")

    root = live_root()
    prune_stale_live_dlls(root)

    unique = f"{os.getpid():08x}_{time.time_ns() & 0xFFFFFFFF:08x}"
    probe_name = f"{prefix}_{unique}"
    live_dll = root / f"{probe_name}.dll"

    # copyfile() intentionally copies bytes only. Do not use copy2(): these
    # disposable files must inherit the user-temp directory's ACL/metadata,
    # especially when the source checkout lives under Program Files.
    shutil.copyfile(source_dll, live_dll)
    return root, probe_name, live_dll


def try_remove_live_dll(path: Path) -> bool:
    """Best-effort removal; False normally means Illustrator still maps the DLL."""
    try:
        path.unlink()
        return True
    except FileNotFoundError:
        # Already absent is equivalent to successful cleanup for callers.
        return True
    except OSError:
        return False
