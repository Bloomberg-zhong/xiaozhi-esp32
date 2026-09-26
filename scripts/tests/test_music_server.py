import json
import pathlib
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "music_server"))

import music_server  # noqa: E402


def id3_frame(frame_id, text):
    body = b"\x03" + text.encode("utf-8")
    return frame_id + len(body).to_bytes(4, "big") + b"\x00\x00" + body


def id3_tag(**frames):
    data = b"".join(id3_frame(key.encode(), value) for key, value in frames.items())
    size = len(data)
    syncsafe = bytes([(size >> 21) & 0x7F, (size >> 14) & 0x7F, (size >> 7) & 0x7F, size & 0x7F])
    return b"ID3\x03\x00\x00" + syncsafe + data


class MusicServerTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.temp_dir.name)
        (root / "album").mkdir()
        self.audio = id3_tag(TIT2="稻香", TPE1="周杰伦", TALB="魔杰座") + bytes(range(256)) * 4
        (root / "album" / "track01.mp3").write_bytes(self.audio)
        (root / "album" / "track01.lrc").write_text("[00:01.00]还记得你说家是唯一的城堡\n", "utf-8")
        (root / "Beyond - 海阔天空.flac").write_bytes(b"fLaC" + b"\x00" * 64)
        (root / "notes.txt").write_text("ignored")
        self.start_server(api_key="")

    def start_server(self, api_key):
        library = music_server.Library(self.temp_dir.name)
        self.server = ThreadingHTTPServer(
            ("127.0.0.1", 0), music_server.make_handler(library, api_key)
        )
        self.server.RequestHandlerClass.log_message = lambda *args: None
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d" % self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.temp_dir.cleanup()

    def get(self, path, headers=None):
        request = urllib.request.Request(self.base + path, headers=headers or {})
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, dict(response.headers), response.read()

    def search(self, query):
        status, _, body = self.get("/search?q=" + urllib.request.quote(query) + "&limit=5")
        self.assertEqual(status, 200)
        return json.loads(body)["tracks"]

    def test_search_uses_tags_and_file_names(self):
        tracks = self.search("稻香 周杰伦")
        self.assertEqual(len(tracks), 1)
        self.assertEqual(tracks[0]["title"], "稻香")
        self.assertEqual(tracks[0]["artist"], "周杰伦")
        self.assertEqual(tracks[0]["album"], "魔杰座")
        self.assertTrue(tracks[0]["url"].startswith("/files/"))
        self.assertTrue(tracks[0]["lyric_url"].startswith("/lyrics/"))

        tracks = self.search("beyond")
        self.assertEqual([(t["title"], t["artist"]) for t in tracks], [("海阔天空", "Beyond")])
        self.assertNotIn("lyric_url", tracks[0])

        self.assertEqual(len(self.search("")), 2)
        self.assertEqual(self.search("not-there"), [])

    def test_stream_supports_ranges(self):
        url = self.search("稻香")[0]["url"]
        status, headers, body = self.get(url)
        self.assertEqual(status, 200)
        self.assertEqual(headers["Content-Type"], "audio/mpeg")
        self.assertEqual(body, self.audio)

        status, headers, body = self.get(url, {"Range": "bytes=100-"})
        self.assertEqual(status, 206)
        self.assertEqual(body, self.audio[100:])
        self.assertEqual(headers["Content-Range"], "bytes 100-%d/%d" % (len(self.audio) - 1, len(self.audio)))

        with self.assertRaises(urllib.error.HTTPError) as context:
            self.get(url, {"Range": "bytes=999999-"})
        self.assertEqual(context.exception.code, 416)

    def test_lyrics(self):
        lyric_url = self.search("稻香")[0]["lyric_url"]
        _, headers, body = self.get(lyric_url)
        self.assertIn("text/plain", headers["Content-Type"])
        self.assertIn("城堡", body.decode("utf-8"))

    def test_rejects_path_traversal(self):
        with self.assertRaises(urllib.error.HTTPError) as context:
            self.get("/files/..%2F..%2Fetc%2Fpasswd")
        self.assertEqual(context.exception.code, 404)

    def test_api_key(self):
        self.server.shutdown()
        self.server.server_close()
        self.start_server(api_key="secret")
        with self.assertRaises(urllib.error.HTTPError) as context:
            self.get("/search?q=")
        self.assertEqual(context.exception.code, 401)
        status, _, _ = self.get("/search?q=", {"Authorization": "Bearer secret"})
        self.assertEqual(status, 200)


if __name__ == "__main__":
    unittest.main()
