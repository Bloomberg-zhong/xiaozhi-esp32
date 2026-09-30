"""Synchronized lyrics: the catalog's own, otherwise the free LRCLIB service."""

import urllib.parse
from typing import Optional

from .models import ProviderError, Track
from .netutil import Fetcher, TTLCache

LRCLIB_URL = "https://lrclib.net"
NEGATIVE_TTL = 3600
POSITIVE_TTL = 24 * 3600
MAX_LYRIC_CHARS = 64 * 1024


class LyricsService:
    def __init__(self, fetcher: Fetcher, cache: TTLCache, base_url: str = LRCLIB_URL, enabled=True):
        self.fetcher = fetcher
        self.cache = cache
        self.base_url = base_url.rstrip("/")
        self.enabled = enabled

    def find(self, track: Track, provider_lyrics: Optional[str]) -> Optional[str]:
        if provider_lyrics and provider_lyrics.strip():
            return provider_lyrics[:MAX_LYRIC_CHARS]
        # Without an artist the match would be a guess; radio has no lyrics.
        if not self.enabled or track.live or not track.title or not track.artist:
            return None

        key = ("lrclib", track.artist.lower(), track.title.lower(), track.duration_ms // 5000)
        cached = self.cache.get(key, False)
        if cached is not False:
            return cached
        try:
            text = self._lookup(track)
        except ProviderError:
            return None  # Not cached: the next request may work
        self.cache.set(key, text, POSITIVE_TTL if text else NEGATIVE_TTL)
        return text

    def _lookup(self, track: Track) -> Optional[str]:
        params = {"artist_name": track.artist, "track_name": track.title}
        if track.album:
            params["album_name"] = track.album
        if track.duration_ms:
            params["duration"] = str(round(track.duration_ms / 1000))
        try:
            found = self.fetcher.get_json(self.base_url + "/api/get", params)
            text = found.get("syncedLyrics") if isinstance(found, dict) else None
            if text:
                return text[:MAX_LYRIC_CHARS]
        except ProviderError:
            pass  # 404 means no exact match; fall back to a search
        results = self.fetcher.get_json(
            self.base_url + "/api/search",
            {"artist_name": track.artist, "track_name": track.title},
        )
        if not isinstance(results, list):
            return None
        for item in results:
            text = item.get("syncedLyrics")
            if not text:
                continue
            # Accept a match only when the length is close, to avoid other versions.
            if track.duration_ms and abs(float(item.get("duration") or 0) * 1000 - track.duration_ms) > 8000:
                continue
            return text[:MAX_LYRIC_CHARS]
        return None
