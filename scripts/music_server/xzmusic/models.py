"""Data shared by providers and the HTTP layer."""

from dataclasses import dataclass
from typing import Optional
from urllib.parse import quote


@dataclass
class Track:
    """A playable item. `id` is "<provider>:<provider specific id>"."""

    id: str
    title: str
    artist: str = ""
    album: str = ""
    duration_ms: int = 0
    provider: str = ""
    live: bool = False
    # Where the audio comes from. Never sent to clients: they always stream
    # through /stream/<id>, so they only ever talk to this server.
    stream_url: str = ""
    # Lyrics the provider already has (LRC text), if any.
    lyrics: Optional[str] = None

    def to_json(self) -> dict:
        data = {
            "id": self.id,
            "title": self.title,
            "artist": self.artist,
            "album": self.album,
            "url": "/stream/" + quote(self.id, safe=""),
            "source": self.provider,
        }
        if self.duration_ms > 0:
            data["duration_ms"] = self.duration_ms
        if self.live:
            data["live"] = True
        else:
            data["lyric_url"] = "/lyrics/" + quote(self.id, safe="")
        return data


class ProviderError(Exception):
    """A provider could not answer (network failure, bad response, ...)."""


class UnknownSource(Exception):
    def __init__(self, name: str, available):
        super().__init__("unknown source '%s'; available: %s" % (name, ", ".join(available)))
        self.name = name
        self.available = list(available)


class NotFound(Exception):
    pass


def split_id(track_id: str):
    """"jamendo:123" -> ("jamendo", "123"). Raises NotFound for malformed ids."""
    provider, sep, local = track_id.partition(":")
    if not sep or not provider or not local:
        raise NotFound("malformed id")
    return provider, local
