"""A folder of audio files on the server. Also the simplest way to try it out."""

import os
import random
import re
import struct
import threading
import time
import urllib.parse
from pathlib import Path
from typing import List, Optional

from ..models import NotFound, Track
from .base import Provider, StreamResponse

AUDIO_TYPES = {
    ".mp3": "audio/mpeg",
    ".m4a": "audio/mp4",
    ".aac": "audio/aac",
    ".flac": "audio/flac",
    ".wav": "audio/wav",
}
RESCAN_SECONDS = 60


def _decode_id3_text(data: bytes) -> str:
    if not data:
        return ""
    encoding, payload = data[0], data[1:]
    codec = {0: "latin-1", 1: "utf-16", 2: "utf-16-be", 3: "utf-8"}.get(encoding, "latin-1")
    try:
        return payload.decode(codec).replace("\x00", " ").strip()
    except UnicodeDecodeError:
        return ""


def read_id3_tags(path: Path) -> dict:
    """Title, artist and album from an ID3v2.3/2.4 header."""
    tags = {}
    try:
        with open(path, "rb") as file:
            header = file.read(10)
            if len(header) < 10 or header[:3] != b"ID3" or header[3] not in (3, 4):
                return tags
            size = 0
            for byte in header[6:10]:
                size = (size << 7) | (byte & 0x7F)
            data = file.read(min(size, 256 * 1024))
    except OSError:
        return tags

    wanted = {b"TIT2": "title", b"TPE1": "artist", b"TALB": "album"}
    offset, version = 0, header[3]
    while offset + 10 <= len(data):
        frame_id = data[offset : offset + 4]
        if not frame_id.strip(b"\x00"):
            break
        raw_size = data[offset + 4 : offset + 8]
        if version == 4:
            frame_size = 0
            for byte in raw_size:
                frame_size = (frame_size << 7) | (byte & 0x7F)
        else:
            frame_size = struct.unpack(">I", raw_size)[0]
        if frame_id in wanted:
            text = _decode_id3_text(data[offset + 10 : offset + 10 + frame_size])
            if text:
                tags[wanted[frame_id]] = text
        offset += 10 + frame_size
    return tags


class LocalProvider(Provider):
    name = "local"
    description = "Music files on this server"

    def __init__(self, config, context):
        super().__init__(config, context)
        self._lock = threading.Lock()
        self._tracks: List[dict] = []
        self._scanned = 0.0
        self.root: Optional[Path] = None
        if config.get("music_dir"):
            self.root = Path(config["music_dir"]).expanduser().resolve()

    def enabled(self) -> bool:
        return self.root is not None and self.root.is_dir()

    def _scan(self) -> List[dict]:
        entries = []
        for path in sorted(self.root.rglob("*")):
            if not path.is_file() or path.suffix.lower() not in AUDIO_TYPES:
                continue
            relative = path.relative_to(self.root).as_posix()
            tags = read_id3_tags(path) if path.suffix.lower() == ".mp3" else {}
            title, artist = path.stem, ""
            match = re.match(r"^(.+?)\s+-\s+(.+)$", path.stem)
            if match:
                artist, title = match.group(1), match.group(2)
            lyric = path.with_suffix(".lrc")
            entries.append(
                {
                    "id": relative,
                    "path": path,
                    "title": tags.get("title", title),
                    "artist": tags.get("artist", artist),
                    "album": tags.get("album", path.parent.name if path.parent != self.root else ""),
                    "lyric_path": lyric if lyric.is_file() else None,
                }
            )
        return entries

    def _entries(self) -> List[dict]:
        with self._lock:
            if not self._tracks or time.monotonic() - self._scanned > RESCAN_SECONDS:
                self._tracks = self._scan()
                self._scanned = time.monotonic()
            return list(self._tracks)

    def count(self) -> int:
        return len(self._entries())

    def _to_track(self, entry: dict) -> Track:
        return Track(
            id="local:" + entry["id"],
            title=entry["title"],
            artist=entry["artist"],
            album=entry["album"],
            provider=self.name,
            stream_url="file://" + entry["id"],
        )

    def search(self, query: str, limit: int) -> List[Track]:
        entries = self._entries()
        words = [w.lower() for w in query.split()]
        if not words:
            random.shuffle(entries)
            return [self._to_track(e) for e in entries[:limit]]

        scored = []
        for index, entry in enumerate(entries):
            haystack = " ".join((entry["title"], entry["artist"], entry["album"], entry["id"])).lower()
            if all(word in haystack for word in words):
                score = 1 + sum(word in entry["title"].lower() for word in words)
                scored.append((-score, index, entry))
        scored.sort(key=lambda item: item[:2])
        return [self._to_track(e) for _, _, e in scored[:limit]]

    def _find(self, local_id: str) -> dict:
        for entry in self._entries():
            if entry["id"] == local_id:
                return entry
        raise NotFound(local_id)

    def get(self, local_id: str) -> Track:
        return self._to_track(self._find(local_id))

    def lyrics(self, track: Track) -> Optional[str]:
        entry = self._find(track.id.split(":", 1)[1])
        if entry["lyric_path"] is None:
            return None
        try:
            return entry["lyric_path"].read_text("utf-8", errors="replace")
        except OSError:
            return None

    def open_stream(self, track: Track, range_header: Optional[str]) -> StreamResponse:
        entry = self._find(track.id.split(":", 1)[1])
        path = entry["path"]
        size = path.stat().st_size
        start, end, status = 0, size - 1, 200
        if range_header:
            match = re.match(r"^bytes=(\d*)-(\d*)$", range_header.strip())
            if not match or (not match.group(1) and not match.group(2)):
                return StreamResponse(416, {"Content-Range": "bytes */%d" % size}, iter(()))
            if match.group(1):
                start = int(match.group(1))
                if match.group(2):
                    end = min(int(match.group(2)), size - 1)
            else:
                start = max(size - int(match.group(2)), 0)
            if start >= size or start > end:
                return StreamResponse(416, {"Content-Range": "bytes */%d" % size}, iter(()))
            status = 206

        length = end - start + 1
        headers = {
            "Content-Type": AUDIO_TYPES.get(path.suffix.lower(), "application/octet-stream"),
            "Content-Length": str(length),
            "Accept-Ranges": "bytes",
        }
        if status == 206:
            headers["Content-Range"] = "bytes %d-%d/%d" % (start, end, size)
        file = open(path, "rb")
        file.seek(start)

        def body():
            remaining = length
            while remaining > 0:
                chunk = file.read(min(64 * 1024, remaining))
                if not chunk:
                    return
                remaining -= len(chunk)
                yield chunk

        return StreamResponse(status, headers, body(), file.close)
