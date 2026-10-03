"""Adapter for an operator-configured Go Music API gateway.

Uses the gateway's ordinary search, inspect, stream and lyric APIs. Unavailable
audio is omitted; this adapter does not unlock restricted tracks. Stream URLs
stay on the configured gateway, so expiring catalog URLs are resolved per play.
"""

import concurrent.futures
import re
import urllib.parse

from xzmusic.models import NotFound, ProviderError, Track, artwork_url
from xzmusic.providers.base import Provider


def _normalize(text):
    return re.sub(r"[\W_]+", "", text.casefold())


class GatewayProvider(Provider):
    name = "gateway"
    description = "Music from your configured Go Music API gateway"

    def __init__(self, config, context):
        super().__init__(config, context)
        self.url = str(config.get("url", "")).rstrip("/")
        self.sources = tuple(config.get("sources", ["kuwo", "qq", "netease"]))
        self.max_probes = max(1, min(int(config.get("max_probes", 8)), 20))
        if self.enabled():
            # Only the operator's configured host is trusted, as with Subsonic.
            # Artwork on other hosts still passes the Fetcher's SSRF guard.
            self.fetcher.trust(self.url)

    def enabled(self):
        parts = urllib.parse.urlsplit(self.url)
        return bool(parts.scheme in ("http", "https") and parts.hostname and self.sources)

    def _url(self, action, params):
        return self.url + "/api/v1/music/" + action + "?" + urllib.parse.urlencode(params, doseq=True)

    def _key(self, source, song_id):
        return ("gateway-track", self.url, source, song_id)

    def _track(self, song):
        source, song_id = song.get("source"), song.get("id")
        if source not in self.sources or not isinstance(song_id, str) or not song_id:
            return None
        title = song.get("name")
        artist = song.get("artist", "")
        album = song.get("album", "")
        album_info = album if isinstance(album, dict) else {}
        cover = next((url for value in (
            song.get("cover_url"), song.get("cover"), song.get("picUrl"),
            song.get("pic_url"), song.get("artwork_url"), song.get("albumCover"),
            album_info.get("picUrl"), album_info.get("cover"), album_info.get("cover_url"),
        ) if (url := artwork_url(value, self.url))), "")
        if not isinstance(title, str) or not title or not isinstance(artist, str):
            return None
        try:
            duration = max(0, min(int(float(song.get("duration") or 0) * 1000), 0xFFFFFFFF))
        except (TypeError, ValueError, OverflowError):
            duration = 0
        return Track(
            id="gateway:%s:%s" % (source, song_id), title=title, artist=artist,
            album=album if isinstance(album, str) else str(album_info.get("name") or ""),
            duration_ms=duration, provider=self.name, cover_url=cover,
            stream_url=self._url("stream", {"source": source, "id": song_id,
                                            "name": title, "artist": artist}),
        )

    def _inspect(self, track):
        _, source, song_id = track.id.split(":", 2)
        key = self._key(source, song_id)
        cached = self.context.cache.get(key)
        if cached is not None:
            return cached
        try:
            result = self.fetcher.get_json(self._url("inspect", {
                "source": source, "id": song_id, "duration": track.duration_ms // 1000,
            }))
            if not isinstance(result, dict) or result.get("valid") is not True:
                return None
        except ProviderError:
            return None
        self.context.cache.set(key, track, 300)
        return track

    def search(self, query, limit):
        if not query.strip():
            return []
        result = self.fetcher.get_json(self._url("search", {
            "q": query, "type": "song", "sources": self.sources,
        }))
        if not isinstance(result, dict) or result.get("code") != 200:
            raise ProviderError("music gateway search failed")
        data = result.get("data") or {}
        if not isinstance(data, dict):
            raise ProviderError("invalid music gateway search response")
        tracks = []
        seen = set()
        keyword = _normalize(query)
        for song in data.get("songs") or []:
            track = self._track(song) if isinstance(song, dict) else None
            if track is None or track.id in seen:
                continue
            title, artist = _normalize(track.title), _normalize(track.artist)
            song_keyword = keyword.replace(artist, "") if artist and artist in keyword else keyword
            if song_keyword and song_keyword not in title:
                continue
            seen.add(track.id)
            tracks.append(track)

        def rank(track):
            title, artist = _normalize(track.title), _normalize(track.artist)
            exact = keyword in (title, artist + title, title + artist)
            source = track.id.split(":", 2)[1]
            return (not exact, self.sources.index(source))

        tracks.sort(key=rank)
        candidates = tracks[:self.max_probes]
        if not candidates:
            return []
        # Probe a bounded shortlist in parallel and preserve relevance order.
        with concurrent.futures.ThreadPoolExecutor(max_workers=min(4, len(candidates))) as pool:
            playable = [track for track in pool.map(self._inspect, candidates) if track is not None]
        if not playable:
            raise ProviderError("the music gateway found no accessible audio")
        return playable[:max(1, min(limit, 50))]

    def get(self, local_id):
        source, sep, song_id = local_id.partition(":")
        if not sep or source not in self.sources or not song_id or len(song_id) > 256:
            raise NotFound("invalid gateway track id")
        cached = self.context.cache.get(self._key(source, song_id))
        if cached is not None:
            return cached
        # Favorites retain title/artist on the device. An id is sufficient to
        # resume streaming even when this server's metadata cache was restarted.
        track = self._inspect(self._track({"source": source, "id": song_id, "name": song_id}))
        if track is None:
            raise NotFound("gateway audio is unavailable")
        return track

    def _catalog_lyrics(self, track):
        _, source, song_id = track.id.split(":", 2)
        result = self.fetcher.get_json(self._url("lyric", {"source": source, "id": song_id}))
        data = result.get("data") if isinstance(result, dict) else None
        text = data.get("lyric") if isinstance(data, dict) else data
        return text if isinstance(text, str) and text.strip() else None

    def lyrics(self, track):
        try:
            return self._catalog_lyrics(track)
        except ProviderError:
            return None

    def resolve_cover(self, track):
        if track.cover_url:
            return track
        _, source, song_id = track.id.split(":", 2)
        key = self._key(source, song_id)
        cached = self.context.cache.get(key)
        if cached is not None and cached.cover_url:
            return cached

        # Saved IDs survive metadata expiry. The gateway's inspect response has
        # no cover, but explicit lyric tags can recover a catalog query. This
        # extra work is confined to cover requests, never the audio get path.
        text = self._catalog_lyrics(track)
        if not text:
            return track
        tags = {}
        for line in text.splitlines():
            if len(line) > 512:
                continue
            tag = re.fullmatch(r"\[(ti|ar):([^\]]*)\]", line.strip(), re.IGNORECASE)
            if tag:
                tags[tag[1].lower()] = tag[2].strip()
        title, artist = tags.get("ti", ""), tags.get("ar", "")
        if not title or len(title) > 256 or len(artist) > 256:
            return track
        result = self.fetcher.get_json(self._url("search", {
            "q": " ".join(value for value in (title, artist) if value),
            "type": "song", "sources": [source],
        }))
        data = result.get("data") if isinstance(result, dict) and result.get("code") == 200 else None
        songs = data.get("songs") if isinstance(data, dict) else None
        if not isinstance(songs, list):
            return track
        for song in songs:
            if not isinstance(song, dict) or song.get("source") != source or song.get("id") != song_id:
                continue
            restored = self._track(song)
            if restored is not None and restored.cover_url:
                self.context.cache.set(key, restored, 300)
                return restored
        return track


PROVIDERS = {"gateway": GatewayProvider}
