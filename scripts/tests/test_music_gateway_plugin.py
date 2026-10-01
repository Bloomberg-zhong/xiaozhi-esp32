import json
import pathlib
import sys
import threading
import unittest
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "music_server"))

from gateway_plugin import GatewayProvider
from xzmusic.models import NotFound
from xzmusic.netutil import BlockedHost, Fetcher, TTLCache
from xzmusic.providers.base import Context


class GatewayFixture(BaseHTTPRequestHandler):
    requests = []

    def log_message(self, *args):
        pass

    def do_GET(self):
        parts = urllib.parse.urlsplit(self.path)
        params = urllib.parse.parse_qs(parts.query)
        type(self).requests.append((parts.path, params, dict(self.headers)))
        if parts.path.endswith("/search"):
            payload = {"code": 200, "data": {"songs": [
                {"id": "demo", "source": "qq", "name": "稻香 (Demo)",
                 "artist": "周杰伦", "duration": 244},
                {"id": "blocked", "source": "kuwo", "name": "稻香",
                 "artist": "周杰伦", "duration": 223},
                {"id": "440613", "source": "kuwo", "name": "稻香",
                 "artist": "周杰伦", "duration": 223},
                {"id": "bad-source", "source": "unconfigured", "name": "稻香"},
            ]}}
        elif parts.path.endswith("/inspect"):
            payload = {"valid": params["id"][0] != "blocked"}
        elif parts.path.endswith("/lyric"):
            payload = {"code": 200, "data": {"lyric": "[00:01.00]fixture"}}
        elif parts.path.endswith("/stream"):
            body = b"ID3-test-audio"
            self.send_response(206)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("Content-Range", "bytes 0-12/13")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        else:
            self.send_error(404)
            return
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class GatewayPluginTests(unittest.TestCase):
    def setUp(self):
        GatewayFixture.requests = []
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), GatewayFixture)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = "http://127.0.0.1:%d" % self.server.server_port
        self.context = Context(Fetcher(timeout=1), TTLCache())
        self.provider = GatewayProvider(
            {"url": self.url, "sources": ["kuwo", "qq"], "max_probes": 4}, self.context)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def test_prefers_exact_version_and_skips_unavailable_audio(self):
        tracks = self.provider.search("稻香 周杰伦", 10)
        self.assertEqual([t.id for t in tracks], ["gateway:kuwo:440613", "gateway:qq:demo"])
        self.assertEqual(tracks[0].title, "稻香")
        self.assertEqual(tracks[0].artist, "周杰伦")
        self.assertEqual(tracks[0].duration_ms, 223000)

    def test_stream_range_and_catalog_lyrics(self):
        track = self.provider.search("稻香 周杰伦", 1)[0]
        response = self.provider.open_stream(track, "bytes=0-12")
        try:
            self.assertEqual(response.status, 206)
            self.assertEqual(b"".join(response.body), b"ID3-test-audio")
            self.assertEqual(response.headers["Content-Type"], "audio/mpeg")
        finally:
            response.close()
        self.assertEqual(self.provider.lyrics(track), "[00:01.00]fixture")
        self.assertTrue(any(path.endswith("/stream") and headers.get("Range") == "bytes=0-12"
                            for path, _, headers in GatewayFixture.requests))

    def test_saved_id_survives_a_server_restart(self):
        provider = GatewayProvider({"url": self.url, "sources": ["kuwo"]},
                                   Context(Fetcher(timeout=1), TTLCache()))
        track = provider.get("kuwo:440613")
        self.assertEqual(urllib.parse.parse_qs(urllib.parse.urlsplit(track.stream_url).query)["id"],
                         ["440613"])
        with self.assertRaises(NotFound):
            provider.get("unconfigured:123")

    def test_only_operator_configured_gateway_is_trusted(self):
        with self.assertRaises(BlockedHost):
            self.context.fetcher.check_url("http://127.0.0.2/private")
