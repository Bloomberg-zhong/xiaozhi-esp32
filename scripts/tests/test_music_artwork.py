"""Cover metadata and API contracts, using local fixtures only."""

import io
import pathlib
import socket
import sys
import tempfile
import unittest
import urllib.error
import urllib.parse
import urllib.request
from unittest.mock import Mock, patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "music_server"))

from gateway_plugin import GatewayProvider
from xzmusic import artwork
from xzmusic.httpapi import serve, serve_in_thread
from xzmusic.models import ArtworkUnavailable, NotFound, ProviderError, Track, artwork_url
from xzmusic.netutil import BlockedHost, Fetcher, TTLCache, _GuardedRedirects
from xzmusic.providers.archive import ArchiveProvider
from xzmusic.providers.base import Context, Provider
from xzmusic.providers.jamendo import JamendoProvider
from xzmusic.providers.local import LocalProvider
from xzmusic.providers.subsonic import SubsonicProvider
from xzmusic.service import MusicService


class ArtworkMetadataTests(unittest.TestCase):
    def setUp(self):
        self.fetcher = Mock()
        self.context = Context(self.fetcher, TTLCache())

    def test_private_original_url_never_appears_in_metadata(self):
        track = Track("subsonic:a/b", "song", cover_url="https://private/rest/cover?token=secret")
        self.assertEqual(track.to_json()["cover_url"], "/cover/subsonic%3Aa%2Fb?size=128")
        self.assertNotIn("secret", str(track.to_json()))
        self.assertNotIn("cover_url", Track("local:no-cover", "song").to_json())

    def test_supported_gateway_album_shapes_and_invalid_urls(self):
        provider = GatewayProvider({"url": "https://gateway.example", "sources": ["qq"]}, self.context)
        for extra in ({"picUrl": "https://cdn.example/cover.png"},
                      {"cover": "https://cdn.example/cover.png"},
                      {"album": {"name": "album", "picUrl": "https://cdn.example/cover.png"}}):
            track = provider._track({"id": "1", "source": "qq", "name": "song", **extra})
            self.assertEqual(track.cover_url, "https://cdn.example/cover.png")
        for value in ("file:///etc/passwd", "javascript:alert(1)", "https://u:p@cdn.example/a", "http:///bad", "https://cdn.example/a\n"):
            track = provider._track({"id": "1", "source": "qq", "name": "song", "cover": value})
            self.assertEqual(track.cover_url, "")

    def test_jamendo_and_subsonic_cover_payloads(self):
        jam = JamendoProvider({}, self.context)
        self.assertEqual(jam._to_track({"id": "1", "album_image": "https://cdn.example/a.jpg"}).cover_url,
                         "https://cdn.example/a.jpg")
        sub = SubsonicProvider({"url": "https://navi.example", "username": "me",
                                "token": "abc", "salt": "salt"}, self.context)
        track = sub._to_track({"id": "song", "coverArt": "album-cover"})
        params = urllib.parse.parse_qs(urllib.parse.urlsplit(track.cover_url).query)
        self.assertEqual(params["id"], ["album-cover"])
        self.assertEqual(params["size"], ["128"])
        self.assertEqual(params["t"], ["abc"])
        self.assertEqual(sub._to_track({"id": "song"}).cover_url, "")

    def test_archive_album_cover_and_saved_id(self):
        self.fetcher.get_json.return_value = {"files": [
            {"name": "song.mp3", "format": "MP3"},
            {"name": "back.jpg"}, {"name": "cover.jpg"}]}
        provider = ArchiveProvider({}, self.context)
        track = provider.get("album/song.mp3")
        self.assertEqual(track.cover_url, "https://archive.org/download/album/cover.jpg")

    def test_local_matching_cover_precedes_folder_cover_and_blocks_symlink_escape(self):
        with tempfile.TemporaryDirectory() as folder, tempfile.TemporaryDirectory() as outside:
            root = pathlib.Path(folder)
            (root / "song.mp3").write_bytes(b"ID3")
            (root / "cover.jpg").write_bytes(b"folder")
            (root / "song.png").write_bytes(b"matching")
            provider = LocalProvider({"music_dir": folder}, self.context)
            track = provider.get("song.mp3")
            self.assertEqual(provider.cover_bytes(track), b"matching")
            (root / "song.png").unlink()
            (root / "cover.jpg").unlink()
            target = pathlib.Path(outside) / "secret.jpg"
            target.write_bytes(b"secret")
            (root / "cover.jpg").symlink_to(target)
            self.assertEqual(provider.get("song.mp3").cover_url, "")
            with self.assertRaises(NotFound):
                provider.cover_bytes(track)

    def test_remote_cover_uses_fetcher_byte_limit_and_ssrf_guard(self):
        provider = Provider({}, self.context)
        provider.cover_bytes(Track("x:1", "song", cover_url="https://cdn.example/a"))
        self.fetcher.get_bytes.assert_called_once_with("https://cdn.example/a", max_bytes=2 * 1024 * 1024)
        guarded = Provider({}, Context(Fetcher(timeout=1), TTLCache()))
        with self.assertRaises(BlockedHost):
            guarded.cover_bytes(Track("x:1", "song", cover_url="http://127.0.0.1/private"))

    def test_url_validation(self):
        self.assertEqual(artwork_url("/cover.jpg", "https://cdn.example/api"), "https://cdn.example/cover.jpg")
        self.assertEqual(artwork_url("//cdn.example/a", "https://gateway.example"), "https://cdn.example/a")
        for value in (None, {}, "ftp://cdn.example/a", "https://cdn.example:bad/a"):
            self.assertEqual(artwork_url(value), "")

    def test_fake_ip_trust_is_exact_and_applies_only_to_artwork(self):
        fetcher = Fetcher(timeout=1)
        context = Context(fetcher, TTLCache(), ["img4.kuwo.cn"])
        provider = Provider({}, context)
        track = Track("fixture:1", "song", cover_url="https://img4.kuwo.cn/a.jpg")
        fake_dns = [(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP, "",
                     ("198.18.3.148", 443))]
        opener = Mock()
        opener.open.return_value.read.return_value = b"cover"
        with patch("xzmusic.netutil.socket.getaddrinfo", return_value=fake_dns), \
                patch("xzmusic.netutil.urllib.request.build_opener", return_value=opener):
            self.assertEqual(provider.cover_bytes(track), b"cover")
            self.assertNotIn("img4.kuwo.cn", fetcher.trusted_hosts)
            with self.assertRaises(BlockedHost):
                fetcher.open(track.cover_url)
            for host in ("sub.img4.kuwo.cn", "img4.kuwo.cn.evil.example", "img5.kuwo.cn"):
                track.cover_url = "https://" + host + "/a.jpg"
                with self.assertRaises(BlockedHost):
                    provider.cover_bytes(track)
        self.assertEqual(opener.open.call_count, 1)

    def test_artwork_redirects_to_other_private_hosts_remain_blocked(self):
        fetcher = Fetcher(trusted_hosts=["img4.kuwo.cn"])
        guard = _GuardedRedirects(fetcher)
        request = urllib.request.Request("https://img4.kuwo.cn/a.jpg")
        same_host = guard.redirect_request(request, None, 302, "redirect", {},
                                           "https://img4.kuwo.cn/b.jpg")
        self.assertEqual(same_host.full_url, "https://img4.kuwo.cn/b.jpg")
        for url in ("http://127.0.0.1/secret", "http://192.168.1.1/secret",
                    "http://198.18.3.149/secret"):
            with self.assertRaises(BlockedHost):
                guard.redirect_request(request, None, 302, "redirect", {}, url)

    def test_trusted_artwork_config_is_opt_in_and_rejects_wildcards_urls(self):
        config = {"providers": {"archive": {"enabled": False}, "radio": {"enabled": False}}}
        self.assertEqual(MusicService(config).context.trusted_artwork_hosts, [])
        service = MusicService({**config, "trusted_artwork_hosts": ["IMG4.KUWO.CN"]})
        self.assertEqual(service.context.trusted_artwork_hosts, ["img4.kuwo.cn"])
        self.assertNotIn("img4.kuwo.cn", service.fetcher.trusted_hosts)
        for hosts in ("img4.kuwo.cn", ["*.kuwo.cn"], ["https://img4.kuwo.cn"],
                      ["img4.kuwo.cn:443"], ["img4.kuwo.cn/"], [None]):
            with self.assertRaises(ValueError):
                MusicService({**config, "trusted_artwork_hosts": hosts})


class CoverApiTests(unittest.TestCase):
    def setUp(self):
        self.service = MusicService({"api_keys": ["key"], "providers": {
            "archive": {"enabled": False}, "radio": {"enabled": False}}})
        provider = Mock()
        provider.get.return_value = Track("fixture:1", "song", provider="fixture", cover_url="https://cdn.example/a")
        provider.resolve_cover.side_effect = lambda track: track
        self.provider = provider
        self.service.providers["fixture"] = provider
        self.server = serve(self.service, "127.0.0.1", 0)
        self.thread = serve_in_thread(self.server)
        self.url = "http://127.0.0.1:%d/cover/fixture%%3A1" % self.server.server_port

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def request(self, suffix="", auth=True):
        headers = {"Authorization": "Bearer key"} if auth else {}
        return urllib.request.urlopen(urllib.request.Request(self.url + suffix, headers=headers), timeout=3)

    def error(self, code, suffix="", auth=True):
        with self.assertRaises(urllib.error.HTTPError) as context:
            self.request(suffix, auth)
        self.assertEqual(context.exception.code, code)
        context.exception.close()

    def test_cover_requires_api_key_and_valid_size(self):
        self.error(401, auth=False)
        self.provider.cover_bytes.assert_not_called()
        self.error(400, "?size=oops")

    def test_missing_cover_and_upstream_404(self):
        self.provider.get.return_value.cover_url = ""
        self.error(404)
        self.service.cache = TTLCache()
        self.provider.get.return_value.cover_url = "https://cdn.example/a"
        cause = urllib.error.HTTPError("https://cdn.example/a", 404, "missing", {}, None)
        error = ProviderError("upstream returned HTTP 404")
        error.__cause__ = cause
        self.provider.cover_bytes.side_effect = error
        self.error(404)

    def test_no_pillow_preserves_audio_service_and_reports_unavailable_cover(self):
        self.provider.cover_bytes.return_value = b"fixture"
        with patch.object(artwork, "Image", None):
            self.error(503)
        self.assertTrue(self.service.authorized("Bearer key", ""))

    @unittest.skipIf(artwork.Image is None, "optional Pillow unavailable")
    def test_normalized_jpeg_is_bounded_baseline_and_cached(self):
        source = io.BytesIO()
        artwork.Image.new("RGBA", (800, 400), (200, 50, 30, 255)).save(source, "PNG")
        self.provider.cover_bytes.return_value = source.getvalue()
        with self.request("?size=9999") as response:
            body = response.read()
            self.assertEqual(response.headers["Content-Type"], "image/jpeg")
            self.assertLessEqual(len(body), 32 * 1024)
        with artwork.Image.open(io.BytesIO(body)) as image:
            self.assertEqual(image.size, (128, 64))
            self.assertEqual(image.mode, "RGB")
            self.assertNotIn("progressive", image.info)
        with self.request("?size=128") as response:
            self.assertEqual(response.read(), body)
        self.provider.cover_bytes.assert_called_once()
        with self.request("?size=32") as response:
            with artwork.Image.open(io.BytesIO(response.read())) as image:
                self.assertEqual(image.size, (32, 16))

    @unittest.skipIf(artwork.Image is None, "optional Pillow unavailable")
    def test_invalid_image_and_excessive_dimensions_fail_closed(self):
        with self.assertRaises(ProviderError):
            artwork.normalize_cover(b"not an image")
        source = io.BytesIO()
        artwork.Image.new("RGB", (32, 32)).save(source, "PNG")
        with patch.object(artwork, "MAX_INPUT_PIXELS", 100):
            with self.assertRaises(ProviderError):
                artwork.normalize_cover(source.getvalue())


if __name__ == "__main__":
    unittest.main()
