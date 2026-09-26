#!/usr/bin/env python3
"""Minimal music library server for the XiaoZhi music player.

Serves a local folder through the HTTP JSON API described in
docs/music-player.md. Only the Python standard library is used.

    python3 scripts/music_server/music_server.py --music-dir ~/Music --port 8090

Then configure the device from the console with
self.music.configure_source(type="http", url="http://<this-host>:8090").

Supported files: .mp3 .m4a .aac .flac .wav. Titles and artists come from ID3v2
tags when present, otherwise from "Artist - Title.ext" file names. A sidecar
"<same name>.lrc" file provides synchronized lyrics.
"""

import argparse
import hmac
import json
import os
import random
import re
import struct
import sys
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, unquote, urlparse

AUDIO_TYPES = {
    ".mp3": "audio/mpeg",
    ".m4a": "audio/mp4",
    ".aac": "audio/aac",
    ".flac": "audio/flac",
    ".wav": "audio/wav",
}
MAX_LIMIT = 50


def _decode_id3_text(data):
    if not data:
        return ""
    encoding, payload = data[0], data[1:]
    codec = {0: "latin-1", 1: "utf-16", 2: "utf-16-be", 3: "utf-8"}.get(encoding, "latin-1")
    try:
        text = payload.decode(codec)
    except UnicodeDecodeError:
        return ""
    return text.replace("\x00", " ").strip()


def read_id3_tags(path):
    """Returns {"title", "artist", "album"} from an ID3v2.3/2.4 header."""
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
    offset = 0
    version = header[3]
    while offset + 10 <= len(data):
        frame_id = data[offset:offset + 4]
        if not frame_id.strip(b"\x00"):
            break
        raw_size = data[offset + 4:offset + 8]
        if version == 4:
            frame_size = 0
            for byte in raw_size:
                frame_size = (frame_size << 7) | (byte & 0x7F)
        else:
            frame_size = struct.unpack(">I", raw_size)[0]
        body = data[offset + 10:offset + 10 + frame_size]
        if frame_id in wanted:
            text = _decode_id3_text(body)
            if text:
                tags[wanted[frame_id]] = text
        offset += 10 + frame_size
    return tags


class Library:
    def __init__(self, root):
        self.root = Path(root).resolve()
        self._lock = threading.Lock()
        self._tracks = []
        self.scan()

    def scan(self):
        tracks = []
        for path in sorted(self.root.rglob("*")):
            if not path.is_file() or path.suffix.lower() not in AUDIO_TYPES:
                continue
            relative = path.relative_to(self.root).as_posix()
            tags = read_id3_tags(path) if path.suffix.lower() == ".mp3" else {}
            stem = path.stem
            artist, title = "", stem
            match = re.match(r"^(.+?)\s+-\s+(.+)$", stem)
            if match:
                artist, title = match.group(1), match.group(2)
            track = {
                "id": relative,
                "title": tags.get("title", title),
                "artist": tags.get("artist", artist),
                "album": tags.get("album", path.parent.name if path.parent != self.root else ""),
                "path": path,
            }
            lyric = path.with_suffix(".lrc")
            track["lyric_path"] = lyric if lyric.is_file() else None
            tracks.append(track)
        with self._lock:
            self._tracks = tracks
        return len(tracks)

    def __len__(self):
        with self._lock:
            return len(self._tracks)

    def search(self, query, limit):
        with self._lock:
            tracks = list(self._tracks)
        words = [word.lower() for word in query.split()]
        if not words:
            random.shuffle(tracks)
            return tracks[:limit]

        def score(track):
            haystack = " ".join((track["title"], track["artist"], track["album"], track["id"]))
            haystack = haystack.lower()
            if not all(word in haystack for word in words):
                return 0
            return 1 + sum(word in track["title"].lower() for word in words)

        scored = [(score(track), index, track) for index, track in enumerate(tracks)]
        scored = [item for item in scored if item[0] > 0]
        scored.sort(key=lambda item: (-item[0], item[1]))
        return [track for _, _, track in scored[:limit]]

    def resolve(self, relative):
        path = (self.root / relative).resolve()
        if self.root != path and self.root not in path.parents:
            return None
        return path if path.is_file() else None


def make_handler(library, api_key):
    class Handler(BaseHTTPRequestHandler):
        server_version = "XiaoZhiMusic/1.0"

        def log_message(self, fmt, *args):
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

        def _authorized(self):
            if not api_key:
                return True
            header = self.headers.get("Authorization", "")
            return hmac.compare_digest(header, "Bearer " + api_key)

        def _send_json(self, status, payload):
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if not self._authorized():
                self._send_json(HTTPStatus.UNAUTHORIZED, {"error": "invalid api key"})
                return
            url = urlparse(self.path)
            if url.path == "/search":
                self._search(parse_qs(url.query))
            elif url.path.startswith("/files/"):
                self._send_file(unquote(url.path[len("/files/"):]), audio=True)
            elif url.path.startswith("/lyrics/"):
                self._send_file(unquote(url.path[len("/lyrics/"):]), audio=False)
            else:
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})

        def _search(self, params):
            query = params.get("q", [""])[0]
            try:
                limit = int(params.get("limit", ["20"])[0])
            except ValueError:
                limit = 20
            limit = max(1, min(limit, MAX_LIMIT))
            result = []
            for track in library.search(query, limit):
                item = {
                    "id": track["id"],
                    "title": track["title"],
                    "artist": track["artist"],
                    "album": track["album"],
                    "url": "/files/" + quote(track["id"]),
                }
                if track["lyric_path"] is not None:
                    item["lyric_url"] = "/lyrics/" + quote(track["id"])
                result.append(item)
            self._send_json(HTTPStatus.OK, {"tracks": result})

        def _send_file(self, relative, audio):
            path = library.resolve(relative)
            if path is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
                return
            if not audio:
                path = path.with_suffix(".lrc")
                if not path.is_file():
                    self._send_json(HTTPStatus.NOT_FOUND, {"error": "no lyrics"})
                    return
                content_type = "text/plain; charset=utf-8"
            else:
                content_type = AUDIO_TYPES.get(path.suffix.lower(), "application/octet-stream")

            size = path.stat().st_size
            start, end = 0, size - 1
            status = HTTPStatus.OK
            range_header = self.headers.get("Range")
            if range_header:
                match = re.match(r"^bytes=(\d*)-(\d*)$", range_header.strip())
                if not match or (not match.group(1) and not match.group(2)):
                    self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
                    self.send_header("Content-Range", "bytes */%d" % size)
                    self.end_headers()
                    return
                if match.group(1):
                    start = int(match.group(1))
                    if match.group(2):
                        end = min(int(match.group(2)), size - 1)
                else:
                    start = max(size - int(match.group(2)), 0)
                if start >= size or start > end:
                    self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
                    self.send_header("Content-Range", "bytes */%d" % size)
                    self.end_headers()
                    return
                status = HTTPStatus.PARTIAL_CONTENT

            length = end - start + 1
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(length))
            self.send_header("Accept-Ranges", "bytes")
            if status == HTTPStatus.PARTIAL_CONTENT:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
            self.end_headers()
            with open(path, "rb") as file:
                file.seek(start)
                remaining = length
                while remaining > 0:
                    chunk = file.read(min(64 * 1024, remaining))
                    if not chunk:
                        break
                    try:
                        self.wfile.write(chunk)
                    except (BrokenPipeError, ConnectionResetError):
                        return
                    remaining -= len(chunk)

    return Handler


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--music-dir", required=True, help="folder with music files")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument(
        "--api-key",
        default=os.environ.get("XIAOZHI_MUSIC_API_KEY", ""),
        help="require 'Authorization: Bearer <key>' (default: $XIAOZHI_MUSIC_API_KEY)",
    )
    args = parser.parse_args(argv)

    library = Library(args.music_dir)
    print("Indexed %d tracks from %s" % (len(library), library.root))
    server = ThreadingHTTPServer((args.host, args.port), make_handler(library, args.api_key))
    print("Serving on http://%s:%d" % (args.host, args.port))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
