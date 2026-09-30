"""Provider interface."""

from dataclasses import dataclass, field
from typing import Dict, Iterator, List, Optional

from ..models import NotFound, Track
from ..netutil import Fetcher, TTLCache


@dataclass
class Context:
    """What the service hands to every provider."""

    fetcher: Fetcher
    cache: TTLCache


@dataclass
class StreamResponse:
    status: int
    headers: Dict[str, str]
    body: Iterator[bytes]
    close: callable = field(default=lambda: None)


class Provider:
    name = ""
    description = ""
    # Live providers (radio) never end, so they are left out of mixed searches
    # and only answer when asked for by name.
    live = False

    def __init__(self, config: dict, context: Context):
        self.config = config
        self.context = context

    @property
    def fetcher(self) -> Fetcher:
        return self.context.fetcher

    def enabled(self) -> bool:
        """False when required settings (keys, URLs) are missing."""
        return True

    def search(self, query: str, limit: int) -> List[Track]:
        """Songs matching `query`; popular or random ones when it is empty."""
        raise NotImplementedError

    def get(self, local_id: str) -> Track:
        """Track for an id returned earlier. Raises NotFound."""
        raise NotFound(local_id)

    def lyrics(self, track: Track) -> Optional[str]:
        """LRC text the catalog itself has, or None."""
        return track.lyrics

    def open_stream(self, track: Track, range_header: Optional[str]) -> StreamResponse:
        """Audio for `track`, honouring an optional Range header."""
        headers = {}
        if range_header:
            headers["Range"] = range_header
        response = self.fetcher.open(track.stream_url, headers)
        return stream_from_response(response)


PASS_HEADERS = ("Content-Type", "Content-Length", "Content-Range", "Accept-Ranges")


def stream_from_response(response, chunk_size: int = 32 * 1024) -> StreamResponse:
    """Wraps a urllib response as a StreamResponse."""
    headers = {}
    for name in PASS_HEADERS:
        value = response.headers.get(name)
        if value:
            headers[name] = value

    def body():
        while True:
            try:
                chunk = response.read(chunk_size)
            except OSError:
                return
            if not chunk:
                return
            yield chunk

    return StreamResponse(response.status, headers, body(), response.close)
