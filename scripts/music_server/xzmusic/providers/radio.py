"""Internet radio through the community Radio Browser directory.

Free and keyless. Only stations the ESP32 can decode are returned: plain MP3 or
AAC streams that were reachable at the last check (no HLS playlists).
"""

import urllib.parse
from typing import List

from ..models import NotFound, ProviderError, Track
from .base import Provider

DEFAULT_SERVERS = [
    "https://de1.api.radio-browser.info",
    "https://nl1.api.radio-browser.info",
    "https://at1.api.radio-browser.info",
]
SUPPORTED_CODECS = ("MP3", "AAC", "AAC+")


class RadioProvider(Provider):
    name = "radio"
    description = "Live radio stations (Radio Browser)"
    live = True

    @property
    def servers(self) -> List[str]:
        servers = self.config.get("servers") or DEFAULT_SERVERS
        return [s.rstrip("/") for s in servers]

    def _api(self, path: str, params=None):
        last_error = None
        for server in self.servers:
            try:
                return self.fetcher.get_json(server + path, params)
            except ProviderError as error:  # Try the next mirror
                last_error = error
        raise last_error or ProviderError("no radio directory configured")

    @staticmethod
    def _usable(station: dict) -> bool:
        return (
            station.get("lastcheckok") == 1
            and not station.get("hls")
            and station.get("codec", "").upper() in SUPPORTED_CODECS
            and bool(station.get("url_resolved") or station.get("url"))
        )

    def _to_track(self, station: dict) -> Track:
        where = station.get("country") or ""
        tags = [t for t in (station.get("tags") or "").split(",") if t][:2]
        artist = " · ".join(part for part in [where] + tags if part)
        return Track(
            id="radio:" + station["stationuuid"],
            title=(station.get("name") or "").strip(),
            artist=artist,
            provider=self.name,
            live=True,
            stream_url=station.get("url_resolved") or station.get("url") or "",
        )

    def search(self, query: str, limit: int) -> List[Track]:
        base = {
            "limit": str(max(limit * 3, 10)),  # Some results are filtered out below
            "hidebroken": "true",
            "order": "clickcount",
            "reverse": "true",
        }
        country = self.config.get("countrycode")
        if country:
            base["countrycode"] = country
        stations = []
        if query:
            stations = self._api("/json/stations/search", {**base, "name": query})
            if len(stations) < limit:  # Also match by genre or language tag
                seen = {s["stationuuid"] for s in stations}
                for station in self._api("/json/stations/search", {**base, "tag": query}):
                    if station["stationuuid"] not in seen:
                        stations.append(station)
        else:
            stations = self._api("/json/stations/search", base)
        tracks = []
        for station in stations:
            if self._usable(station):
                track = self._to_track(station)
                if track.title:
                    tracks.append(track)
            if len(tracks) >= limit:
                break
        return tracks

    def get(self, local_id: str) -> Track:
        station_id = urllib.parse.quote(local_id, safe="")
        stations = self._api("/json/stations/byuuid/" + station_id)
        if not stations or not self._usable(stations[0]):
            raise NotFound(local_id)
        track = self._to_track(stations[0])
        try:
            # Counting the click is how the directory ranks stations; it also
            # returns the freshest stream address.
            answer = self._api("/json/url/" + station_id)
            if isinstance(answer, dict) and answer.get("ok") and answer.get("url"):
                track.stream_url = answer["url"]
        except ProviderError:
            pass
        return track

    def lyrics(self, track: Track):
        return None
