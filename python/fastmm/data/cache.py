"""Download cache for public market-data archives.

Venue-independent: a source's fetcher describes URLs and keys, this stores them. Files live
under the cache root at the key the archive uses, so the tree mirrors the remote bucket and
the C++ side finds a file from its key alone.

    cache = Cache()
    cache.download(url, key, sha256=digest)   # resumes a partial file, verifies, then renames
    cache.extract(key)                        # unzip next to the archive

Root: ``$FASTMM_DATA_HOME``, else ``$XDG_CACHE_HOME/fastmm/data``, else
``~/.cache/fastmm/data`` (``fastmm::bt::default_data_dir()`` agrees).
"""

from __future__ import annotations

import gzip
import hashlib
import os
import shutil
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import Callable, Dict, Optional, Union

_CHUNK = 1 << 20
_TIMEOUT = 60


def cache_root(override: Optional[Union[str, "os.PathLike[str]"]] = None) -> Path:
    """Cache root, from ``override`` or the environment."""
    if override:
        return Path(override).expanduser()
    for var, suffix in (("FASTMM_DATA_HOME", ""), ("XDG_CACHE_HOME", "fastmm/data")):
        value = os.environ.get(var)
        if value:
            return Path(value).expanduser() / suffix if suffix else Path(value).expanduser()
    return Path.home() / ".cache" / "fastmm" / "data"


def sha256_file(path: Union[str, "os.PathLike[str]"]) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(_CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


class DownloadError(RuntimeError):
    """A download failed, or what arrived did not match its published checksum."""


class Cache:
    """Files under a root directory, addressed by the archive's own key."""

    def __init__(self, root: Optional[Union[str, "os.PathLike[str]"]] = None) -> None:
        self.root = cache_root(root)

    def path(self, key: str) -> Path:
        return self.root / key

    def has(self, key: str) -> bool:
        return self.path(key).is_file()

    def fetch_text(self, url: str) -> Optional[str]:
        """GET a small file as text; None on 404."""
        try:
            with urllib.request.urlopen(url, timeout=_TIMEOUT) as reply:  # noqa: S310
                return reply.read().decode("ascii", "replace")
        except urllib.error.HTTPError as exc:
            if exc.code == 404:
                return None
            raise DownloadError(f"{url}: HTTP {exc.code}") from None
        except urllib.error.URLError as exc:
            raise DownloadError(f"{url}: {exc.reason}") from None

    def download(
        self,
        url: str,
        key: str,
        *,
        sha256: Optional[str] = None,
        force: bool = False,
        headers: Optional[Dict[str, str]] = None,
        progress: Optional[Callable[[str, int, int], None]] = None,
    ) -> Path:
        """Download ``url`` to ``key``, resuming a partial file and verifying ``sha256``.

        A file already in the cache is returned untouched unless ``force``. The download goes
        to ``<key>.part`` and is renamed only after the checksum matches, so an interrupted
        run never leaves a truncated file that looks complete. A server that refuses the range
        request starts the file again.
        """
        dest = self.path(key)
        if dest.is_file() and not force:
            return dest
        dest.parent.mkdir(parents=True, exist_ok=True)
        part = dest.with_suffix(dest.suffix + ".part")
        have = part.stat().st_size if part.is_file() and not force else 0
        if force and part.is_file():
            part.unlink()
            have = 0

        request = urllib.request.Request(url, headers=headers or {})  # noqa: S310
        if have:
            request.add_header("Range", f"bytes={have}-")
        try:
            with urllib.request.urlopen(request, timeout=_TIMEOUT) as reply:  # noqa: S310
                append = reply.status == 206
                if have and not append:
                    have = 0  # the server ignored the range: start over
                total = int(reply.headers.get("Content-Length") or 0) + have
                with open(part, "ab" if append else "wb") as fh:
                    if not append:
                        fh.truncate(0)
                    while True:
                        block = reply.read(_CHUNK)
                        if not block:
                            break
                        fh.write(block)
                        have += len(block)
                        if progress:
                            progress(key, have, total)
        except urllib.error.HTTPError as exc:
            if exc.code == 416 and part.is_file():  # already complete
                pass
            elif exc.code in (403, 416) and have:  # the server refuses ranges: start again
                part.unlink(missing_ok=True)
                return self.download(
                    url, key, sha256=sha256, force=True, headers=headers, progress=progress
                )
            else:
                raise DownloadError(f"{url}: HTTP {exc.code}") from None
        except urllib.error.URLError as exc:
            raise DownloadError(f"{url}: {exc.reason}") from None

        if sha256:
            got = sha256_file(part)
            if got != sha256:
                part.unlink()
                raise DownloadError(f"{key}: sha256 {got}, expected {sha256}")
        part.replace(dest)
        return dest

    def extract(self, key: str, *, force: bool = False) -> Path:
        """Unpack a cached ``.zip`` (one member) or ``.gz`` next to it, and return the file."""
        archive = self.path(key)
        if key.endswith(".gz"):
            dest = archive.with_suffix("")
            if dest.is_file() and not force:
                return dest
            tmp = dest.with_suffix(dest.suffix + ".part")
            with gzip.open(archive, "rb") as src, open(tmp, "wb") as out:
                shutil.copyfileobj(src, out, _CHUNK)
            tmp.replace(dest)
            return dest
        with zipfile.ZipFile(archive) as zf:
            members = [m for m in zf.namelist() if not m.endswith("/")]
            if len(members) != 1:
                raise DownloadError(f"{key}: expected one file in the archive, got {len(members)}")
            dest = archive.parent / Path(members[0]).name
            if dest.is_file() and not force:
                return dest
            tmp = dest.with_suffix(dest.suffix + ".part")
            with zf.open(members[0]) as src, open(tmp, "wb") as out:
                shutil.copyfileobj(src, out, _CHUNK)
        tmp.replace(dest)
        return dest


def human(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} GiB"


def stderr_progress(key: str, have: int, total: int) -> None:
    if not sys.stderr.isatty():
        return
    name = key.rsplit("/", 1)[-1]
    bar = f"{human(have)}/{human(total)}" if total else human(have)
    sys.stderr.write(f"\r  {name}  {bar}   ")
    sys.stderr.flush()
