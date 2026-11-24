"""Launcher wrapper for burnshall-ui/SovietRepublic-MCP.

Reads SOVIET_DIR from .env.local (or environment) and binds SovietRepublic-MCP's
paths module to the local game installation without copying files into the
game directory or committing local paths to git.
"""

from __future__ import annotations

import asyncio
import logging
import os
import sys
from pathlib import Path


def _load_soviet_dir(repo_root: Path) -> Path:
    env_dir = os.environ.get("SOVIET_DIR", "").strip()
    if env_dir:
        return Path(env_dir)

    env_file = repo_root / ".env.local"
    if env_file.exists():
        for raw_line in env_file.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("SOVIET_DIR="):
                return Path(line.split("=", 1)[1].strip())

    raise RuntimeError(
        "SOVIET_DIR not configured. Set SOVIET_DIR in .env.local or the environment."
    )


def main() -> None:
    repo_root = Path(__file__).resolve().parent.parent
    mcp_repo = repo_root / "references" / "SovietRepublic-MCP"
    if not mcp_repo.exists():
        raise RuntimeError(
            f"SovietRepublic-MCP not found at {mcp_repo}. Run scripts/setup_soviet_mcp.ps1 first."
        )

    soviet_dir = _load_soviet_dir(repo_root)
    if not (soviet_dir / "media_soviet").exists():
        raise RuntimeError(f"media_soviet folder not found under {soviet_dir}")

    sys.path.insert(0, str(mcp_repo))

    import paths  # type: ignore[import-not-found]

    paths.SAVES_DIR = soviet_dir / "media_soviet" / "save"
    paths.BUILDINGS_DIR = soviet_dir / "media_soviet" / "buildings_types"
    paths.ACTIVE_SAVE_FILE = mcp_repo / "active_save.cfg"

    logging.basicConfig(level=logging.WARNING)
    import mcp_server  # type: ignore[import-not-found]

    mcp_server.SAVES_DIR = paths.SAVES_DIR
    mcp_server.BUILDINGS_DIR = paths.BUILDINGS_DIR
    mcp_server.ACTIVE_SAVE_FILE = paths.ACTIVE_SAVE_FILE

    asyncio.run(mcp_server.run_mcp())


if __name__ == "__main__":
    main()
