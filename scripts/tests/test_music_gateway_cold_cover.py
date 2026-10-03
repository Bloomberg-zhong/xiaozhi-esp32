"""A saved gateway ID can restore artwork without a preceding device search."""

import io
import json
import pathlib
import sys
import threading
import unittest
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "music_server"))

from xzmusic import artwork
from xzmusic.httpapi import serve, serve_in_thread
from xzmusic.service import MusicService


class ColdCoverGateway(BaseHTTPRequestHandler):
    requests = []
    matching_id = True
    tagged_lyrics = True
    malformed_search = False
    transient_error = ""
    image = b""

    def log_message(self, *args):
        pass

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        params = urllib.parse.parse_qs(url.query)
        type(self).requests.append((url.path, params))
        if self.transient_error and url.path.endswith("/" + self.transient_error):
            self.send_error(502, "temporary catalog error")
            return
        if url.path.endswith("/inspect"):
            payload = {"valid": True}
        elif url.path.endswith("/lyric"):
            text = "[ti:听妈妈的话]\n[ar:周杰伦]\n[00:01.00]fixture"
            payload = {"code": 200, "data": {"lyric": text if self.tagged_lyrics else "[00:01.00]fixture"}}
        elif url.path.endswith("/search"):
            song = {"id": "138243" if self.matching_id else "other-id", "source": "kuwo",
                    "name": "听妈妈的话", "artist": "周杰伦", "album": "依然范特西",
                    "duration": 270, "cover": "http://127.0.0.1:%d/art.jpg" % self.server.server_port}
            payload = {"code": 200, "data": {"songs": [song]}}
            if self.malformed_search:
                payload["data"] = [song]
        elif url.path == "/art.jpg":
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(self.image)))
            self.end_headers()
            self.wfile.write(self.image)
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


@unittest.skipIf(artwork.Image is None, "optional Pillow unavailable")
class GatewayColdCoverTests(unittest.TestCase):
    def setUp(self):
        ColdCoverGateway.requests = []
        ColdCoverGateway.matching_id = True
        ColdCoverGateway.tagged_lyrics = True
        ColdCoverGateway.malformed_search = False
        ColdCoverGateway.transient_error = ""
        image = io.BytesIO()
        artwork.Image.new("RGB", (256, 144), (120, 40, 30)).save(image, "PNG")
        ColdCoverGateway.image = image.getvalue()
        self.gateway = ThreadingHTTPServer(("127.0.0.1", 0), ColdCoverGateway)
        self.gateway_thread = threading.Thread(target=self.gateway.serve_forever, daemon=True)
        self.gateway_thread.start()
        service = MusicService({"plugins": ["gateway_plugin"], "providers": {
            "archive": {"enabled": False}, "radio": {"enabled": False},
            "gateway": {"url": "http://127.0.0.1:%d" % self.gateway.server_port,
                        "sources": ["kuwo"], "enabled": True}}})
        self.api = serve(service, "127.0.0.1", 0)
        self.api_thread = serve_in_thread(self.api)
        self.url = "http://127.0.0.1:%d/cover/gateway%%3Akuwo%%3A138243?size=128" % self.api.server_port

    def tearDown(self):
        self.api.shutdown()
        self.api.server_close()
        self.api_thread.join()
        self.gateway.shutdown()
        self.gateway.server_close()
        self.gateway_thread.join()

    def get_cover(self):
        try:
            with urllib.request.urlopen(self.url, timeout=3) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as error:
            try:
                return error.code, error.read()
            finally:
                error.close()

    def test_cold_saved_id_recovers_real_cover_and_caches_it(self):
        status, body = self.get_cover()
        self.assertEqual(status, 200)
        with artwork.Image.open(io.BytesIO(body)) as image:
            self.assertEqual(image.size, (128, 72))
            self.assertEqual(image.format, "JPEG")
        searches = [params for path, params in ColdCoverGateway.requests if path.endswith("/search")]
        self.assertEqual(searches, [{"q": ["听妈妈的话 周杰伦"], "type": ["song"], "sources": ["kuwo"]}])
        request_count = len(ColdCoverGateway.requests)
        self.assertEqual(self.get_cover(), (200, body))
        self.assertEqual(len(ColdCoverGateway.requests), request_count)

    def test_same_title_with_a_different_id_is_not_used(self):
        ColdCoverGateway.matching_id = False
        self.assertEqual(self.get_cover()[0], 404)
        self.assertFalse(any(path == "/art.jpg" for path, _ in ColdCoverGateway.requests))

    def test_untagged_lyrics_do_not_start_a_guessing_search(self):
        ColdCoverGateway.tagged_lyrics = False
        self.assertEqual(self.get_cover()[0], 404)
        self.assertFalse(any(path.endswith("/search") for path, _ in ColdCoverGateway.requests))

    def test_malformed_catalog_does_not_become_an_internal_error(self):
        ColdCoverGateway.malformed_search = True
        self.assertEqual(self.get_cover()[0], 404)
        self.assertFalse(any(path == "/art.jpg" for path, _ in ColdCoverGateway.requests))

    def test_transient_lyric_failure_remains_retryable(self):
        ColdCoverGateway.transient_error = "lyric"
        self.assertEqual(self.get_cover()[0], 502)
        ColdCoverGateway.transient_error = ""
        self.assertEqual(self.get_cover()[0], 200)

    def test_transient_search_failure_remains_retryable(self):
        ColdCoverGateway.transient_error = "search"
        self.assertEqual(self.get_cover()[0], 502)
        ColdCoverGateway.transient_error = ""
        self.assertEqual(self.get_cover()[0], 200)


if __name__ == "__main__":
    unittest.main()
