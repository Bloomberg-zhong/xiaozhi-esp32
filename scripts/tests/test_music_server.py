"""Tests for scripts/music_server against a fake upstream (no internet needed)."""

import hashlib
import json
import pathlib
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "music_server"))

from xzmusic.httpapi import serve, serve_in_thread  # noqa: E402
from xzmusic.service import MusicService  # noqa: E402

MP3_BYTES = b"ID3\x03\x00\x00\x00\x00\x00\x00" + bytes(range(256)) * 16  # 4106 bytes
SYNCED = "[00:01.00]line one\n[00:05.00]line two\n"


def id3_tag(**frames):
    data = b""
    for key, value in frames.items():
        body = b"\x03" + value.encode("utf-8")
        data += key.encode() + len(body).to_bytes(4, "big") + b"\x00\x00" + body
    size = len(data)
    syncsafe = bytes([(size >> 21) & 0x7F, (size >> 14) & 0x7F, (size >> 7) & 0x7F, size & 0x7F])
    return b"ID3\x03\x00\x00" + syncsafe + data


class FakeUpstream(BaseHTTPRequestHandler):
    """Answers like archive.org, Jamendo, Radio Browser, LRCLIB and Navidrome."""

    protocol_version = "HTTP/1.0"
    requests = []

    def log_message(self, *args):
        pass

    def _send(self, status, body=b"", content_type="application/json", extra=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for key, value in (extra or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def _json(self, payload, status=200):
        self._send(status, json.dumps(payload).encode())

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        query = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
        path = url.path
        FakeUpstream.requests.append((path, query, dict(self.headers)))

        if path == "/archive/advancedsearch.php":
            self._json({"response": {"docs": [
                {"identifier": "item1", "title": "Album One", "creator": "Band A"},
                {"identifier": "item2", "title": "Album Two"},
            ]}})
        elif path.startswith("/archive/metadata/"):
            ident = path.rsplit("/", 1)[1]
            files = [
                {"name": "t1.mp3", "format": "VBR MP3", "title": "Shared Song", "length": "61.5"},
                {"name": "t1.ogg", "format": "Ogg Vorbis"},
                {"name": "t1_64kb.mp3", "format": "64Kbps MP3"},
                {"name": "t2.mp3", "format": "VBR MP3", "length": "1:30"},
            ]
            if ident == "item2":
                files = [{"name": "x.mp3", "format": "VBR MP3", "title": "Shared Song", "artist": "Band item1"}]
            self._json({"metadata": {"title": "Album " + ident, "creator": "Band " + ident}, "files": files})
        elif path.startswith("/archive/download/"):
            self._serve_bytes(MP3_BYTES, "audio/mpeg")
        elif path == "/jamendo/tracks/":
            if query.get("client_id") == "bad":
                self._json({"headers": {"status": "failed", "error_message": "suspended"}, "results": []})
            else:
                self._json({"headers": {"status": "success"}, "results": [
                    {"id": "77", "name": "Jam Song", "artist_name": "Jam Artist", "album_name": "Jam Album",
                     "duration": 200, "audio": "http://%s/stream/jam77.mp3" % self.headers["Host"]}]})
        elif path == "/stream/jam77.mp3":
            self._serve_bytes(MP3_BYTES, "audio/mpeg")
        elif path == "/radio/json/stations/search":
            self._json([
                {"stationuuid": "good", "name": "Good FM", "codec": "MP3", "lastcheckok": 1, "hls": 0,
                 "country": "China", "tags": "pop,music,extra",
                 "url_resolved": "http://%s/live/good.mp3" % self.headers["Host"]},
                {"stationuuid": "hls", "name": "HLS FM", "codec": "AAC", "lastcheckok": 1, "hls": 1,
                 "url_resolved": "http://x/a.m3u8"},
                {"stationuuid": "dead", "name": "Dead FM", "codec": "MP3", "lastcheckok": 0, "hls": 0,
                 "url_resolved": "http://x/d.mp3"},
                {"stationuuid": "ogg", "name": "Ogg FM", "codec": "OGG", "lastcheckok": 1, "hls": 0,
                 "url_resolved": "http://x/o.ogg"},
                {"stationuuid": "evil", "name": "Evil FM", "codec": "MP3", "lastcheckok": 1, "hls": 0,
                 "url_resolved": "http://127.0.0.1:1/private.mp3"},
            ])
        elif path == "/radio/json/stations/byuuid/good":
            self._json([{"stationuuid": "good", "name": "Good FM", "codec": "MP3", "lastcheckok": 1,
                         "hls": 0, "url_resolved": "http://stale/x.mp3"}])
        elif path == "/radio/json/url/good":
            self._json({"ok": True, "url": "http://%s/live/good.mp3" % self.headers["Host"]})
        elif path == "/live/good.mp3":
            self._send(200, b"\xff\xfb" + b"\x00" * 600, "audio/mpeg")
        elif path == "/redirect-to-private":
            self._send(302, b"", extra={"Location": "http://127.0.0.1:1/secret"})
        elif path == "/lrclib/api/get":
            if query.get("track_name") == "Has Lyrics":
                self._json({"syncedLyrics": SYNCED})
            else:
                self._json({"statusCode": 404}, 404)
        elif path == "/lrclib/api/search":
            if query.get("track_name") == "Search Only":
                self._json([{"syncedLyrics": None, "duration": 100},
                            {"syncedLyrics": SYNCED, "duration": 200}])
            else:
                self._json([])
        elif path == "/subsonic/rest/search3.view":
            expected = hashlib.md5(("secret" + query.get("s", "")).encode()).hexdigest()
            if query.get("u") != "me" or query.get("t") != expected:
                self._json({"subsonic-response": {"status": "failed", "error": {"message": "Wrong username or password"}}})
            else:
                self._json({"subsonic-response": {"status": "ok", "searchResult3": {"song": [
                    {"id": "s1", "title": "Navi Song", "artist": "Navi Artist", "album": "NA", "duration": 120}]}}})
        elif path == "/subsonic/rest/getSong.view":
            self._json({"subsonic-response": {"status": "ok", "song": {
                "id": "s1", "title": "Navi Song", "artist": "Navi Artist", "album": "NA", "duration": 120}}})
        elif path == "/subsonic/rest/getLyricsBySongId.view":
            self._json({"subsonic-response": {"status": "ok", "lyricsList": {"structuredLyrics": [
                {"synced": True, "offset": 0, "line": [{"start": 1000, "value": "navi line"}]}]}}})
        elif path == "/subsonic/rest/stream.view":
            self._serve_bytes(MP3_BYTES, "audio/mpeg")
        else:
            self._send(404, b"{}")

    def _serve_bytes(self, data, content_type):
        range_header = self.headers.get("Range")
        if range_header and range_header.startswith("bytes="):
            start_text, _, end_text = range_header[6:].partition("-")
            start = int(start_text or 0)
            end = int(end_text) if end_text else len(data) - 1
            chunk = data[start : end + 1]
            self._send(206, chunk, content_type, {
                "Content-Range": "bytes %d-%d/%d" % (start, start + len(chunk) - 1, len(data)),
                "Accept-Ranges": "bytes"})
        else:
            self._send(200, data, content_type, {"Accept-Ranges": "bytes"})


class ServerCase(unittest.TestCase):
    """Starts the fake upstream and a music server configured to use it."""

    config_overrides = {}

    @classmethod
    def setUpClass(cls):
        cls.upstream = ThreadingHTTPServer(("127.0.0.1", 0), FakeUpstream)
        threading.Thread(target=cls.upstream.serve_forever, daemon=True).start()
        cls.up = "http://127.0.0.1:%d" % cls.upstream.server_address[1]
        cls.temp = tempfile.TemporaryDirectory()
        music = pathlib.Path(cls.temp.name)
        (music / "album").mkdir()
        (music / "album" / "track01.mp3").write_bytes(
            id3_tag(TIT2="稻香", TPE1="周杰伦", TALB="魔杰座") + bytes(range(256)) * 4)
        (music / "album" / "track01.lrc").write_text("[00:01.00]本地歌词\n", "utf-8")
        (music / "Beyond - 海阔天空.flac").write_bytes(b"fLaC" + b"\x00" * 64)
        cls.music_dir = music
        cls.start_server(cls.make_config())

    @classmethod
    def make_config(cls):
        config = {
            "allow_private_hosts": True,
            "lrclib_url": cls.up + "/lrclib",
            "provider_order": ["local", "subsonic", "jamendo", "archive", "radio"],
            "providers": {
                "local": {"music_dir": str(cls.music_dir)},
                "archive": {"base_url": cls.up + "/archive"},
                "jamendo": {"base_url": cls.up + "/jamendo", "client_id": "ok"},
                "radio": {"servers": [cls.up + "/radio"]},
                "subsonic": {"url": cls.up + "/subsonic", "username": "me", "password": "secret"},
            },
        }
        config.update(cls.config_overrides)
        return config

    @classmethod
    def start_server(cls, config):
        cls.service = MusicService(config)
        cls.server = serve(cls.service, "127.0.0.1", 0)
        serve_in_thread(cls.server)
        cls.base = "http://127.0.0.1:%d" % cls.server.server_address[1]

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.upstream.shutdown()
        cls.upstream.server_close()
        cls.temp.cleanup()

    def request(self, path, headers=None):
        request = urllib.request.Request(self.base + path, headers=headers or {})
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.status, dict(response.headers), response.read()

    def json(self, path, headers=None):
        return json.loads(self.request(path, headers)[2])

    def expect_error(self, path, code, headers=None):
        with self.assertRaises(urllib.error.HTTPError) as context:
            self.request(path, headers)
        self.assertEqual(context.exception.code, code)
        return json.loads(context.exception.read() or b"{}")


class SearchTests(ServerCase):
    def test_sources_list_all_catalogs(self):
        names = [s["name"] for s in self.json("/sources")["sources"]]
        self.assertEqual(names, ["local", "subsonic", "jamendo", "archive", "radio"])

    def test_mixed_search_interleaves_and_skips_live_radio(self):
        data = self.json("/search?q=song&limit=20")
        providers = [t["source"] for t in data["tracks"]]
        self.assertNotIn("radio", providers)  # Live streams never end; ask for them by name
        self.assertEqual(providers[:3], ["subsonic", "jamendo", "archive"])  # local has no match
        # "Shared Song" is offered by both archive items: the duplicate is dropped.
        titles = [t["title"].lower() for t in data["tracks"]]
        self.assertEqual(titles.count("shared song"), 1)
        self.assertTrue(all(t["url"].startswith("/stream/") for t in data["tracks"]))
        self.assertTrue(all(t["lyric_url"].startswith("/lyrics/") for t in data["tracks"]))

    def test_limit_is_respected(self):
        self.assertEqual(len(self.json("/search?q=song&limit=2")["tracks"]), 2)

    def test_local_tags_and_chinese_query(self):
        tracks = self.json("/search?" + urllib.parse.urlencode({"q": "稻香 周杰伦", "source": "local"}))["tracks"]
        self.assertEqual([(t["title"], t["artist"], t["album"]) for t in tracks], [("稻香", "周杰伦", "魔杰座")])
        tracks = self.json("/search?" + urllib.parse.urlencode({"q": "beyond", "source": "local"}))["tracks"]
        self.assertEqual([(t["title"], t["artist"]) for t in tracks], [("海阔天空", "Beyond")])

    def test_radio_filters_unsupported_stations(self):
        tracks = self.json("/search?q=music&source=radio")["tracks"]
        self.assertEqual([t["title"] for t in tracks], ["Good FM", "Evil FM"])  # no HLS, dead or Ogg
        self.assertTrue(all(t["live"] for t in tracks))
        self.assertNotIn("lyric_url", tracks[0])
        self.assertEqual(tracks[0]["artist"], "China · pop · music")

    def test_archive_picks_one_mp3_per_song(self):
        tracks = self.json("/search?q=x&source=archive&limit=10")["tracks"]
        ids = [t["id"] for t in tracks]
        self.assertIn("archive:item1/t1.mp3", ids)  # VBR beats the 64 kbps copy
        self.assertNotIn("archive:item1/t1_64kb.mp3", ids)
        self.assertNotIn("archive:item1/t1.ogg", ids)
        durations = {t["id"]: t.get("duration_ms") for t in tracks}
        self.assertEqual(durations["archive:item1/t1.mp3"], 61500)
        self.assertEqual(durations["archive:item1/t2.mp3"], 90000)

    def test_unknown_source_lists_the_available_ones(self):
        error = self.expect_error("/search?q=x&source=nope", 400)
        self.assertIn("unknown source 'nope'", error["error"])
        self.assertIn("radio", error["error"])

    def test_one_failing_catalog_does_not_hide_the_others(self):
        config = self.make_config()
        config["providers"]["archive"]["base_url"] = self.up + "/missing"  # 404 -> ProviderError
        service = MusicService(config)
        tracks, errors = service.search("song", 10)
        self.assertTrue(tracks)
        self.assertIn("archive", errors)

    def test_jamendo_error_is_reported_for_that_source(self):
        config = self.make_config()
        config["providers"]["jamendo"]["client_id"] = "bad"
        service = MusicService(config)
        tracks, errors = service.search("song", 10, "jamendo")
        self.assertEqual(tracks, [])
        self.assertIn("suspended", errors["jamendo"])

    def test_jamendo_needs_a_client_id(self):
        config = self.make_config()
        config["providers"]["jamendo"] = {"base_url": self.up + "/jamendo"}
        self.assertNotIn("jamendo", MusicService(config).providers)

    def test_track_endpoint_resolves_ids(self):
        track = self.json("/track?id=" + urllib.parse.quote("jamendo:77", safe=""))["track"]
        self.assertEqual((track["title"], track["artist"]), ("Jam Song", "Jam Artist"))
        self.expect_error("/track?id=jamendo:abc", 404)
        self.expect_error("/track?id=nothing", 404)
        self.expect_error("/track?id=unknown:1", 404)


class StreamTests(ServerCase):
    def stream(self, track_id, headers=None):
        return self.request("/stream/" + urllib.parse.quote(track_id, safe=""), headers)

    def test_archive_stream_passes_ranges_through(self):
        status, headers, body = self.stream("archive:item1/t1.mp3")
        self.assertEqual((status, body), (200, MP3_BYTES))
        status, headers, body = self.stream("archive:item1/t1.mp3", {"Range": "bytes=10-19"})
        self.assertEqual(status, 206)
        self.assertEqual(body, MP3_BYTES[10:20])
        self.assertEqual(headers["Content-Range"], "bytes 10-19/%d" % len(MP3_BYTES))

    def test_jamendo_and_subsonic_streams(self):
        self.assertEqual(self.stream("jamendo:77")[2], MP3_BYTES)
        self.assertEqual(self.stream("subsonic:s1")[2], MP3_BYTES)

    def test_subsonic_credentials_stay_on_the_server(self):
        track = self.json("/search?q=navi&source=subsonic")["tracks"][0]
        self.assertNotIn("secret", json.dumps(track))
        self.assertNotIn("t=", json.dumps(track))

    def test_radio_stream_and_click_counting(self):
        FakeUpstream.requests.clear()
        status, headers, body = self.stream("radio:good")
        self.assertEqual((status, headers["Content-Type"]), (200, "audio/mpeg"))
        self.assertEqual(len(body), 602)
        paths = [p for p, _, _ in FakeUpstream.requests]
        self.assertIn("/radio/json/url/good", paths)  # The directory counts the listen

    def test_local_file_ranges(self):
        track = self.json("/search?" + urllib.parse.urlencode({"q": "稻香", "source": "local"}))["tracks"][0]
        size = len(id3_tag(TIT2="稻香", TPE1="周杰伦", TALB="魔杰座")) + 1024
        status, headers, body = self.request(track["url"], {"Range": "bytes=100-"})
        self.assertEqual(status, 206)
        self.assertEqual(len(body), size - 100)
        self.assertEqual(headers["Content-Range"], "bytes 100-%d/%d" % (size - 1, size))
        with self.assertRaises(urllib.error.HTTPError) as context:
            self.request(track["url"], {"Range": "bytes=999999-"})
        self.assertEqual(context.exception.code, 416)

    def test_path_traversal_is_not_possible(self):
        self.expect_error("/stream/local%3A..%2F..%2Fetc%2Fpasswd", 404)
        self.expect_error("/lyrics/local%3A..%2F..%2Fetc%2Fpasswd", 404)

    def test_too_many_streams(self):
        slots = 0
        while self.service.stream_slots.acquire(blocking=False):  # Use up every slot
            slots += 1
        try:
            self.expect_error("/stream/jamendo%3A77", 503)
        finally:
            for _ in range(slots):
                self.service.stream_slots.release()


class SecurityTests(ServerCase):
    def test_private_upstream_addresses_are_refused(self):
        config = self.make_config()
        config["allow_private_hosts"] = False
        del config["providers"]["subsonic"]  # Its host would be trusted (see below)
        service = MusicService(config)
        tracks, errors = service.search("music", 5, "radio")
        self.assertEqual(tracks, [])
        self.assertIn("private address", errors["radio"])

    def test_the_operators_own_upstream_is_trusted(self):
        config = self.make_config()
        config["allow_private_hosts"] = False
        service = MusicService(config)  # Subsonic points at 127.0.0.1 here
        self.assertIn("127.0.0.1", service.fetcher.trusted_hosts)
        tracks, errors = service.search("navi", 5, "subsonic")
        self.assertEqual([t.title for t in tracks], ["Navi Song"])

    def test_redirects_into_private_space_are_refused(self):
        from xzmusic.netutil import BlockedHost, Fetcher

        fetcher = Fetcher(allow_private_hosts=False, trusted_hosts=["127.0.0.1"])
        strict = Fetcher(allow_private_hosts=False)
        with self.assertRaises(BlockedHost):
            strict.open(self.up + "/redirect-to-private")
        with self.assertRaises(BlockedHost):
            strict.check_url("http://localhost/x")
        with self.assertRaises(BlockedHost):
            strict.check_url("http://169.254.169.254/latest/meta-data")
        with self.assertRaises(BlockedHost):
            strict.check_url("file:///etc/passwd")
        fetcher.check_url(self.up + "/x")  # Trusted host passes


class AuthTests(ServerCase):
    config_overrides = {"api_keys": ["s3cret"], "rate_limit_per_minute": 1000}

    def test_health_is_public_everything_else_needs_a_key(self):
        self.assertTrue(self.json("/health")["ok"])
        self.assertIn("API key", self.expect_error("/search?q=x", 401)["error"])
        self.expect_error("/sources", 401, {"Authorization": "Bearer wrong"})
        self.expect_error("/stream/jamendo%3A77", 401)

    def test_bearer_and_header_keys(self):
        self.assertEqual(self.request("/sources", {"Authorization": "Bearer s3cret"})[0], 200)
        self.assertEqual(self.request("/sources", {"X-Api-Key": "s3cret"})[0], 200)


class RateLimitTests(ServerCase):
    config_overrides = {"rate_limit_per_minute": 3}

    def test_rate_limit(self):
        codes = []
        for _ in range(6):
            try:
                codes.append(self.request("/sources")[0])
            except urllib.error.HTTPError as error:
                codes.append(error.code)
        self.assertEqual(codes[:3], [200, 200, 200])
        self.assertIn(429, codes[3:])


class LyricsTests(ServerCase):
    def lyrics(self, track_id):
        return self.request("/lyrics/" + urllib.parse.quote(track_id, safe=""))

    def test_local_lrc_file_wins(self):
        status, headers, body = self.lyrics("local:album/track01.mp3")
        self.assertIn("text/plain", headers["Content-Type"])
        self.assertIn("本地歌词", body.decode("utf-8"))

    def test_subsonic_synced_lyrics(self):
        text = self.lyrics("subsonic:s1")[2].decode("utf-8")
        self.assertEqual(text, "[00:01.00]navi line")

    def test_lrclib_fallback_by_artist_and_title(self):
        FakeUpstream.requests.clear()
        service = self.service
        from xzmusic.models import Track

        track = Track(id="jamendo:1", title="Has Lyrics", artist="Someone", provider="jamendo")
        self.assertEqual(service.lyrics_for(track), SYNCED)
        FakeUpstream.requests.clear()
        self.assertEqual(service.lyrics_for(track), SYNCED)  # Cached
        self.assertEqual(FakeUpstream.requests, [])

    def test_lrclib_search_needs_a_matching_length(self):
        from xzmusic.models import Track

        near = Track(id="a:1", title="Search Only", artist="X", provider="a", duration_ms=201000)
        far = Track(id="a:2", title="Search Only", artist="X", provider="a", duration_ms=50000)
        self.assertEqual(self.service.lyrics_for(near), SYNCED)
        self.assertIsNone(self.service.lyrics_for(far))

    def test_no_lyrics_is_a_404(self):
        self.expect_error("/lyrics/archive%3Aitem1%2Ft2.mp3", 404)  # No artist on this file
        from xzmusic.models import Track

        unknown = Track(id="a:3", title="Nothing Here", artist="Nobody", provider="a")
        self.assertIsNone(self.service.lyrics_for(unknown))

    def test_live_radio_has_no_lyrics(self):
        self.expect_error("/lyrics/radio%3Agood", 404)


if __name__ == "__main__":
    unittest.main()
