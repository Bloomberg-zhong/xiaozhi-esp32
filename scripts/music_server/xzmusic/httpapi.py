"""HTTP layer. Endpoints (all GET):

  /health                  {"ok": true}, no authentication
  /sources                 catalogs this server can search
  /search?q=&limit=&source=  {"tracks": [...], "errors": {...}}
  /track?id=               {"track": {...}}
  /stream/<id>             audio (Range supported); <id> is percent-encoded
  /lyrics/<id>             synchronized lyrics as LRC text, 404 if none
"""

import json
import logging
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlsplit

from . import __version__
from .models import NotFound, ProviderError, UnknownSource
from .service import MusicService

log = logging.getLogger("xzmusic")


def make_handler(service: MusicService, trust_proxy: bool = False):
    class Handler(BaseHTTPRequestHandler):
        server_version = "XiaoZhiMusic/" + __version__
        # Live radio has no Content-Length; HTTP/1.0 ends such a body by closing.
        protocol_version = "HTTP/1.0"

        def log_message(self, fmt, *args):
            log.info("%s %s", self.client_address[0], fmt % args)

        # ---- helpers ---------------------------------------------------

        def _client(self) -> str:
            if trust_proxy:
                forwarded = self.headers.get("X-Forwarded-For", "")
                if forwarded:
                    return forwarded.split(",")[0].strip()
            return self.client_address[0]

        def _json(self, status: int, payload: dict) -> None:
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _error(self, status: int, message: str) -> None:
            self._json(status, {"error": message})

        # ---- routing ---------------------------------------------------

        def do_GET(self):
            url = urlsplit(self.path)
            path = url.path
            if path == "/health":
                self._json(200, {"ok": True, "version": __version__})
                return
            if not service.authorized(
                self.headers.get("Authorization", ""), self.headers.get("X-Api-Key", "")
            ):
                self._error(401, "invalid or missing API key")
                return
            if not service.rate_limiter.allow(self._client()):
                self._error(429, "too many requests")
                return
            try:
                if path == "/sources":
                    self._json(200, {"sources": service.sources()})
                elif path == "/search":
                    self._search(parse_qs(url.query))
                elif path == "/track":
                    self._track(parse_qs(url.query))
                elif path.startswith("/stream/"):
                    self._stream(unquote(path[len("/stream/") :]))
                elif path.startswith("/lyrics/"):
                    self._lyrics(unquote(path[len("/lyrics/") :]))
                else:
                    self._error(404, "not found")
            except UnknownSource as error:
                self._error(400, str(error))
            except NotFound:
                self._error(404, "not found")
            except ProviderError as error:
                self._error(502, str(error))
            except (BrokenPipeError, ConnectionResetError):
                pass
            except Exception:
                log.exception("request failed")
                self._error(500, "internal error")

        # ---- endpoints -------------------------------------------------

        def _search(self, params):
            query = params.get("q", [""])[0]
            try:
                limit = int(params.get("limit", ["20"])[0])
            except ValueError:
                limit = 20
            tracks, errors = service.search(query, limit, params.get("source", [""])[0])
            payload = {"tracks": [t.to_json() for t in tracks]}
            if errors:
                payload["errors"] = errors
            if not tracks and errors and params.get("source", [""])[0]:
                # A single requested catalog failed: say so instead of "no results".
                self._error(502, "; ".join("%s: %s" % item for item in errors.items()))
                return
            self._json(200, payload)

        def _track(self, params):
            track = service.get(params.get("id", [""])[0])
            self._json(200, {"track": track.to_json()})

        def _lyrics(self, track_id: str):
            track = service.get(track_id)
            text = service.lyrics_for(track)
            if not text:
                self._error(404, "no lyrics")
                return
            body = text.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _stream(self, track_id: str):
            track = service.get(track_id)
            if not service.stream_slots.acquire(blocking=False):
                self._error(503, "too many streams, try again later")
                return
            try:
                response = service.open_stream(track, self.headers.get("Range"))
                try:
                    self.send_response(response.status)
                    for name, value in response.headers.items():
                        self.send_header(name, value)
                    self.end_headers()
                    for chunk in response.body:
                        self.wfile.write(chunk)
                finally:
                    response.close()
            finally:
                service.stream_slots.release()

    return Handler


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def serve(service: MusicService, host: str, port: int, trust_proxy: bool = False) -> Server:
    return Server((host, port), make_handler(service, trust_proxy))


def serve_in_thread(server: Server) -> threading.Thread:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return thread
