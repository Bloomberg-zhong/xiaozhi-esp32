"""A Subsonic compatible server (Navidrome, Gonic, Airsonic...) of your own.

The server keeps the credentials, so the public domain can serve a private
library behind this server's API key.
"""

import hashlib
import os
import urllib.parse
from typing import List, Optional

from ..models import NotFound, ProviderError, Track
from .base import Provider

API_VERSION = "1.16.1"


class SubsonicProvider(Provider):
    name = "subsonic"
    description = "Your own Subsonic / Navidrome library"

    def __init__(self, config, context):
        super().__init__(config, context)
        if self.enabled():
            # The operator chose this upstream, so it may be on the LAN.
            self.fetcher.trust(self.config["url"])

    def enabled(self) -> bool:
        return bool(self.config.get("url") and self.config.get("username")) and bool(
            self.config.get("password") or self.config.get("token")
        )

    def _url(self, method: str, params: dict) -> str:
        if self.config.get("token") and self.config.get("salt"):
            token, salt = self.config["token"], self.config["salt"]
        else:
            salt = os.urandom(6).hex()
            token = hashlib.md5((self.config["password"] + salt).encode("utf-8")).hexdigest()
        query = {
            "u": self.config["username"],
            "t": token,
            "s": salt,
            "v": API_VERSION,
            "c": "xiaozhi-music-server",
            "f": "json",
            **params,
        }
        base = self.config["url"].rstrip("/")
        return "%s/rest/%s?%s" % (base, method, urllib.parse.urlencode(query))

    def _call(self, method: str, params: dict) -> dict:
        data = self.fetcher.get_json(self._url(method, params))
        response = data.get("subsonic-response") or {}
        if response.get("status") != "ok":
            message = (response.get("error") or {}).get("message", "request failed")
            raise ProviderError("subsonic: %s" % message)
        return response

    def _to_track(self, song: dict) -> Track:
        bitrate = int(self.config.get("max_bitrate_kbps", 128))
        params = {"id": song["id"]}
        if bitrate > 0:
            params.update({"format": "mp3", "maxBitRate": str(bitrate)})
        return Track(
            id="subsonic:%s" % song["id"],
            title=song.get("title", ""),
            artist=song.get("artist", ""),
            album=song.get("album", ""),
            duration_ms=int(float(song.get("duration") or 0) * 1000),
            provider=self.name,
            stream_url=self._url("stream.view", params),
            cover_url=self._url("getCoverArt.view", {"id": song["coverArt"], "size": "128"})
            if song.get("coverArt") else "",
        )

    def search(self, query: str, limit: int) -> List[Track]:
        if query:
            response = self._call(
                "search3.view",
                {"query": query, "songCount": str(limit), "artistCount": "0", "albumCount": "0"},
            )
            songs = (response.get("searchResult3") or {}).get("song") or []
        else:
            response = self._call("getRandomSongs.view", {"size": str(limit)})
            songs = (response.get("randomSongs") or {}).get("song") or []
        return [self._to_track(s) for s in songs if s.get("id") and s.get("title")]

    def get(self, local_id: str) -> Track:
        song = self._call("getSong.view", {"id": local_id}).get("song")
        if not song:
            raise NotFound(local_id)
        return self._to_track(song)

    def lyrics(self, track: Track) -> Optional[str]:
        local_id = track.id.split(":", 1)[1]
        try:  # OpenSubsonic synchronized lyrics
            lists = self._call("getLyricsBySongId.view", {"id": local_id})
            for item in (lists.get("lyricsList") or {}).get("structuredLyrics") or []:
                if item.get("synced") and item.get("line"):
                    offset = int(item.get("offset") or 0)
                    lines = []
                    for line in item["line"]:
                        millis = max(int(line.get("start", 0)) - offset, 0)
                        lines.append(
                            "[%02d:%02d.%02d]%s"
                            % (millis // 60000, millis // 1000 % 60, millis % 1000 // 10, line.get("value", ""))
                        )
                    return "\n".join(lines)
        except ProviderError:
            pass
        return None
