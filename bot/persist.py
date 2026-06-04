"""Durable persistence for Pillio data via Replit Object Storage.

Problem: the C++ server and the bot keep their state as JSON files under a data
directory (by default the folder of ``STORAGE_PATH`` — ``/tmp`` in production).
On Replit, that directory is wiped on every container restart / redeploy, so the
pill database and the family database "reset".

Fix: snapshot those files into Replit Object Storage (which lives outside the
container and survives restarts/redeploys), and restore them on startup.

Flow:
  * ``restore_blocking()`` runs once at startup BEFORE the server reads anything
    (we call it from the run command). It pulls the latest snapshot and writes
    the files back to the data directory.
  * ``backup_if_changed()`` is called periodically from the bot loop. If any
    tracked file changed since the last upload, it re-uploads a fresh snapshot.

Everything degrades gracefully: if Object Storage is not configured/available,
the functions log a warning and return without raising — the app keeps working,
the data just isn't durable across redeploys.
"""

from __future__ import annotations

import io
import logging
import os
import tarfile

log = logging.getLogger("pillio.persist")

STORAGE_PATH = os.environ.get("STORAGE_PATH", "./data/store.json")
DATA_DIR = os.path.dirname(os.path.abspath(STORAGE_PATH)) or "."
SNAPSHOT_KEY = "pillio/snapshot.tar"

# Files (relative to DATA_DIR) and directories we persist.
TRACKED_FILES = ["store.json", "family.json", "bot.json"]
TRACKED_DIRS = ["profiles"]

_client = None
_client_tried = False
_last_sig: dict | None = None


def _get_client():
    """Lazily build the Object Storage client; cache failure as None."""
    global _client, _client_tried
    if _client_tried:
        return _client
    _client_tried = True
    try:
        from replit.object_storage import Client
        _client = Client()
        log.info("Object Storage ready — persistence enabled")
    except Exception as e:  # noqa: BLE001 - any failure means "not available"
        _client = None
        log.warning("Object Storage unavailable (%s) — persistence disabled", e)
    return _client


def _tracked_paths() -> list[str]:
    paths: list[str] = []
    for f in TRACKED_FILES:
        p = os.path.join(DATA_DIR, f)
        if os.path.isfile(p):
            paths.append(p)
    for d in TRACKED_DIRS:
        dp = os.path.join(DATA_DIR, d)
        if os.path.isdir(dp):
            for root, _dirs, files in os.walk(dp):
                for f in files:
                    if f.endswith(".tmp"):
                        continue
                    paths.append(os.path.join(root, f))
    return paths


def _signature() -> dict:
    """Cheap change-signature over tracked files: path -> (size, mtime)."""
    sig: dict = {}
    for p in _tracked_paths():
        try:
            st = os.stat(p)
            sig[p] = (st.st_size, int(st.st_mtime))
        except OSError:
            pass
    return sig


def mark_clean() -> None:
    """Set the in-process baseline to the current on-disk state.

    Call this once at startup (after restore) so the first ``backup_if_changed``
    only fires after a real change, avoiding a redundant upload."""
    global _last_sig
    _last_sig = _signature()


def restore_blocking() -> None:
    """Download the latest snapshot and extract it into DATA_DIR."""
    client = _get_client()
    if not client:
        return
    try:
        data = client.download_as_bytes(SNAPSHOT_KEY)
    except Exception as e:  # noqa: BLE001 - missing snapshot is normal on 1st run
        log.info("No snapshot to restore (%s)", e)
        return
    try:
        os.makedirs(DATA_DIR, exist_ok=True)
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as tar:
            tar.extractall(DATA_DIR)
        log.info("Restored snapshot into %s", DATA_DIR)
    except Exception as e:  # noqa: BLE001
        log.warning("Failed to restore snapshot: %s", e)
    finally:
        mark_clean()


def backup_if_changed() -> None:
    """Upload a fresh snapshot if any tracked file changed since last upload."""
    client = _get_client()
    if not client:
        return
    global _last_sig
    sig = _signature()
    if not sig or sig == _last_sig:
        return
    try:
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:") as tar:
            for p in sig:
                tar.add(p, arcname=os.path.relpath(p, DATA_DIR))
        client.upload_from_bytes(SNAPSHOT_KEY, buf.getvalue())
        _last_sig = sig
        log.info("Backed up %d files to Object Storage", len(sig))
    except Exception as e:  # noqa: BLE001
        log.warning("Backup failed: %s", e)


if __name__ == "__main__":
    import sys

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [persist] %(levelname)s: %(message)s",
    )
    cmd = sys.argv[1] if len(sys.argv) > 1 else "restore"
    if cmd == "restore":
        restore_blocking()
    elif cmd == "backup":
        backup_if_changed()
    else:
        print(f"Unknown command: {cmd!r} (use 'restore' or 'backup')")
