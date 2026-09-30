"""Internet Archive (archive.org): freely licensed and public domain audio.

No account is needed. By default only music collections are searched (net
labels, live concert recordings and the general music collection); set
"collections" to change that.
"""

import concurrent.futures
import re
import urllib.parse
from typing import List, Optional

from ..models import NotFound, ProviderError, Track
from .base import Provider

MP3_FORMATS = ("VBR MP3", "128Kbps MP3", "64Kbps MP3", "MP3")
_BITRATE_SUFFIX = re.compile(r"[_-](?:64|96|128|160|192|256|320)kb(?:ps)?$", re.IGNORECASE)
MAX_ITEMS = 6
MAX_TRACKS_PER_ITEM = 12


def parse_length(value) -> int:
    """Archive file lengths are seconds ("183.4") or "mm:ss"; returns ms."""
    if value in (None, ""):
        return 0
    text = str(value)
    try:
        if ":" in text:
            seconds = 0.0
            for part in text.split(":"):
                seconds = seconds * 60 + float(part)
            return int(seconds * 1000)
        return int(float(text) * 1000)
    except ValueError:
        return 0


def _lucene_escape(text: str) -> str:
    return re.sub(r'([+\-&|!(){}\[\]^"~*?:\\/])', r"\\\1", text)


class ArchiveProvider(Provider):
    name = "archive"
    description = "Internet Archive: free and public domain music"
    DEFAULT_COLLECTIONS = ["netlabels", "audio_music", "etree"]

    @property
    def base_url(self) -> str:
        return self.config.get("base_url", "https://archive.org").rstrip("/")

    def _collection_clause(self) -> str:
        collections = self.config.get("collections", self.DEFAULT_COLLECTIONS)
        if not collections:
            return ""
        return " AND (" + " OR ".join("collection:(%s)" % c for c in collections) + ")"

    def search(self, query: str, limit: int) -> List[Track]:
        words = [w for w in query.split() if w]
        text = " ".join(_lucene_escape(w) for w in words)
        q = ("(%s) AND " % text if text else "") + "mediatype:(audio)" + self._collection_clause()
        data = self.fetcher.get_json(
            self.base_url + "/advancedsearch.php",
            [
                ("q", q),
                ("fl[]", "identifier"),
                ("fl[]", "title"),
                ("fl[]", "creator"),
                ("rows", str(min(MAX_ITEMS, max(1, limit)))),
                ("sort[]", "downloads desc"),
                ("output", "json"),
            ],
        )
        docs = (data.get("response") or {}).get("docs") or []
        if not docs:
            return []

        with concurrent.futures.ThreadPoolExecutor(max_workers=len(docs)) as pool:
            futures = [pool.submit(self._expand, doc) for doc in docs]
            expanded = []
            for future in futures:
                try:
                    expanded.append(future.result(timeout=20))
                except Exception:  # One broken item must not hide the others
                    expanded.append([])
        # Round-robin over items so one long album does not fill the list.
        tracks: List[Track] = []
        depth = 0
        while len(tracks) < limit and any(depth < len(items) for items in expanded):
            for items in expanded:
                if depth < len(items) and len(tracks) < limit:
                    tracks.append(items[depth])
            depth += 1
        return tracks

    def _expand(self, doc: dict) -> List[Track]:
        identifier = doc.get("identifier")
        if not identifier:
            return []
        meta = self.context.cache.get(("archive-meta", identifier))
        if meta is None:
            meta = self.fetcher.get_json(
                self.base_url + "/metadata/" + urllib.parse.quote(identifier, safe="")
            )
            self.context.cache.set(("archive-meta", identifier), meta, 600)
        info = meta.get("metadata") or {}
        album = _first(info.get("title")) or _first(doc.get("title")) or identifier
        creator = _first(info.get("creator")) or _first(doc.get("creator")) or ""

        files = [f for f in meta.get("files", []) if f.get("format") in MP3_FORMATS]
        # Prefer one format per song: keep the best-ranked format of each base name.
        best = {}
        for f in files:
            # Derived copies are named "<song>_64kb.mp3" next to "<song>.mp3".
            base = _BITRATE_SUFFIX.sub("", f["name"].rsplit(".", 1)[0])
            rank = MP3_FORMATS.index(f["format"])
            if base not in best or rank < best[base][0]:
                best[base] = (rank, f)
        chosen = sorted((item[1] for item in best.values()), key=lambda f: f["name"])
        tracks = []
        for f in chosen[:MAX_TRACKS_PER_ITEM]:
            title = f.get("title") or f["name"].rsplit(".", 1)[0].replace("_", " ")
            tracks.append(
                Track(
                    id="archive:%s/%s" % (identifier, f["name"]),
                    title=title,
                    artist=f.get("artist") or f.get("creator") or creator,
                    album=album,
                    duration_ms=parse_length(f.get("length")),
                    provider=self.name,
                    stream_url=self._download_url(identifier, f["name"]),
                )
            )
        return tracks

    def _download_url(self, identifier: str, filename: str) -> str:
        return "%s/download/%s/%s" % (
            self.base_url,
            urllib.parse.quote(identifier, safe=""),
            urllib.parse.quote(filename, safe=""),
        )

    def get(self, local_id: str) -> Track:
        identifier, sep, filename = local_id.partition("/")
        if not sep or not identifier or not filename:
            raise NotFound(local_id)
        title = filename.rsplit(".", 1)[0].replace("_", " ")
        return Track(
            id="archive:" + local_id,
            title=title,
            provider=self.name,
            stream_url=self._download_url(identifier, filename),
        )


def _first(value) -> Optional[str]:
    if isinstance(value, list):
        return str(value[0]) if value else None
    return str(value) if value else None
