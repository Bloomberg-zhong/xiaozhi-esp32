"""Jamendo: Creative Commons licensed music, free for personal use.

Register an application at https://devportal.jamendo.com to get a free
client_id and put it in the config ("client_id") or JAMENDO_CLIENT_ID.
"""

from typing import List

from ..models import NotFound, ProviderError, Track
from .base import Provider


class JamendoProvider(Provider):
    name = "jamendo"
    description = "Jamendo: Creative Commons music (needs a free client_id)"

    @property
    def base_url(self) -> str:
        return self.config.get("base_url", "https://api.jamendo.com/v3.0").rstrip("/")

    def enabled(self) -> bool:
        return bool(self.config.get("client_id"))

    def _query(self, params: dict) -> List[dict]:
        base = {
            "client_id": self.config["client_id"],
            "format": "json",
            "audioformat": "mp32",
            "include": "musicinfo",
        }
        base.update(params)
        data = self.fetcher.get_json(self.base_url + "/tracks/", base)
        headers = data.get("headers") or {}
        if headers.get("status") == "failed":
            raise ProviderError("jamendo: %s" % headers.get("error_message", "request failed"))
        return data.get("results") or []

    def _to_track(self, item: dict) -> Track:
        return Track(
            id="jamendo:%s" % item["id"],
            title=item.get("name", ""),
            artist=item.get("artist_name", ""),
            album=item.get("album_name", ""),
            duration_ms=int(float(item.get("duration") or 0) * 1000),
            provider=self.name,
            stream_url=item.get("audio", ""),
        )

    def search(self, query: str, limit: int) -> List[Track]:
        params = {"limit": str(limit)}
        if query:
            params["search"] = query
            params["order"] = "relevance"
        else:
            params["order"] = "popularity_week"
        return [self._to_track(i) for i in self._query(params) if i.get("audio")]

    def get(self, local_id: str) -> Track:
        if not local_id.isdigit():
            raise NotFound(local_id)
        results = self._query({"id": local_id, "limit": "1"})
        if not results or not results[0].get("audio"):
            raise NotFound(local_id)
        return self._to_track(results[0])
