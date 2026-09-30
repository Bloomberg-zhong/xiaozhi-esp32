"""Ties providers, lyrics and configuration together."""

import concurrent.futures
import logging
import os
import re
import threading
from typing import Dict, List, Optional, Tuple

from .lyrics import LyricsService
from .models import NotFound, ProviderError, Track, UnknownSource, split_id
from .netutil import Fetcher, RateLimiter, TTLCache
from .providers import DEFAULT_ORDER, Context, Provider, provider_classes

log = logging.getLogger("xzmusic")

MAX_LIMIT = 50
SEARCH_TTL = 60.0


def _normalize(text: str) -> str:
    return re.sub(r"\W+", "", text.lower())


class MusicService:
    def __init__(self, config: dict, fetcher: Optional[Fetcher] = None):
        self.config = config
        self.fetcher = fetcher or Fetcher(
            allow_private_hosts=bool(config.get("allow_private_hosts", False)),
            timeout=float(config.get("upstream_timeout", 15)),
        )
        self.cache = TTLCache(int(config.get("cache_entries", 512)))
        self.context = Context(self.fetcher, self.cache)
        self.search_timeout = float(config.get("search_timeout", 8))
        self.api_keys: List[str] = [k for k in config.get("api_keys", []) if k]
        self.rate_limiter = RateLimiter(int(config.get("rate_limit_per_minute", 240)))
        self.stream_slots = threading.BoundedSemaphore(int(config.get("max_streams", 6)))
        self.lyrics = LyricsService(
            self.fetcher,
            self.cache,
            config.get("lrclib_url", "https://lrclib.net"),
            enabled=bool(config.get("lyrics_fallback", True)),
        )
        self.providers: Dict[str, Provider] = {}
        self._load_providers()

    def _load_providers(self) -> None:
        classes = provider_classes(self.config.get("plugins", []))
        settings = self.config.get("providers", {})
        order = list(self.config.get("provider_order", DEFAULT_ORDER))
        for name in classes:
            if name not in order:
                order.append(name)
        for name in order:
            cls = classes.get(name)
            cfg = dict(settings.get(name, {}))
            if cls is None or cfg.get("enabled") is False:
                continue
            provider = cls(cfg, self.context)
            if provider.enabled():
                self.providers[name] = provider
            elif cfg.get("enabled"):
                log.warning("provider %s is enabled but not configured", name)

    # ---- catalog ------------------------------------------------------

    def sources(self) -> List[dict]:
        return [
            {"name": p.name, "description": p.description, "live": p.live}
            for p in self.providers.values()
        ]

    def search(self, query: str, limit: int, source: str = "") -> Tuple[List[Track], Dict[str, str]]:
        limit = max(1, min(limit, MAX_LIMIT))
        query = query.strip()
        if source:
            if source not in self.providers:
                raise UnknownSource(source, self.providers)
            chosen = [self.providers[source]]
        else:
            chosen = [p for p in self.providers.values() if not p.live]
        if not chosen:
            return [], {}

        key = ("search", query.lower(), limit, tuple(p.name for p in chosen))
        cached = self.cache.get(key)
        if cached is not None:
            return cached

        results: Dict[str, List[Track]] = {}
        errors: Dict[str, str] = {}
        pool = concurrent.futures.ThreadPoolExecutor(max_workers=len(chosen))
        futures = {pool.submit(p.search, query, limit): p for p in chosen}
        done, pending = concurrent.futures.wait(futures, timeout=self.search_timeout)
        for future in done:
            provider = futures[future]
            try:
                results[provider.name] = future.result()
            except (ProviderError, NotFound) as error:
                errors[provider.name] = str(error)
            except Exception as error:  # A provider bug must not break the others
                log.exception("provider %s failed", provider.name)
                errors[provider.name] = "internal error"
        for future in pending:
            errors[futures[future].name] = "timed out"
            future.cancel()
        pool.shutdown(wait=False)

        tracks = self._interleave([results.get(p.name, []) for p in chosen], limit)
        outcome = (tracks, errors)
        if tracks:  # Do not remember failures
            self.cache.set(key, outcome, SEARCH_TTL)
        return outcome

    @staticmethod
    def _interleave(lists: List[List[Track]], limit: int) -> List[Track]:
        merged: List[Track] = []
        seen = set()
        depth = 0
        while len(merged) < limit and any(depth < len(items) for items in lists):
            for items in lists:
                if depth >= len(items) or len(merged) >= limit:
                    continue
                track = items[depth]
                identity = (_normalize(track.title), _normalize(track.artist))
                if track.live or identity not in seen:
                    seen.add(identity)
                    merged.append(track)
            depth += 1
        return merged

    def get(self, track_id: str) -> Track:
        name, local_id = split_id(track_id)
        provider = self.providers.get(name)
        if provider is None:
            raise NotFound("unknown source")
        cache_key = ("track", track_id)
        track = self.cache.get(cache_key)
        if track is None:
            track = provider.get(local_id)
            self.cache.set(cache_key, track, 300)
        return track

    def lyrics_for(self, track: Track) -> Optional[str]:
        provider = self.providers.get(track.provider)
        own = provider.lyrics(track) if provider else None
        return self.lyrics.find(track, own)

    def open_stream(self, track: Track, range_header: Optional[str]):
        return self.providers[track.provider].open_stream(track, range_header)

    # ---- security -----------------------------------------------------

    def authorized(self, header: str, api_key_header: str) -> bool:
        if not self.api_keys:
            return True
        import hmac

        candidates = []
        if header.lower().startswith("bearer "):
            candidates.append(header[7:].strip())
        if api_key_header:
            candidates.append(api_key_header.strip())
        return any(
            hmac.compare_digest(candidate.encode(), key.encode())
            for candidate in candidates
            for key in self.api_keys
        )


def load_config(path: Optional[str]) -> dict:
    import json

    config: dict = {}
    if path:
        with open(path, "r", encoding="utf-8") as file:
            config = json.load(file)
    providers = config.setdefault("providers", {})

    # Environment variables keep secrets out of the config file.
    env = os.environ
    if env.get("XZ_MUSIC_API_KEY"):
        config.setdefault("api_keys", []).append(env["XZ_MUSIC_API_KEY"])
    if env.get("JAMENDO_CLIENT_ID"):
        providers.setdefault("jamendo", {})["client_id"] = env["JAMENDO_CLIENT_ID"]
    if env.get("MUSIC_DIR"):
        providers.setdefault("local", {})["music_dir"] = env["MUSIC_DIR"]
    if env.get("SUBSONIC_URL"):
        sub = providers.setdefault("subsonic", {})
        sub["url"] = env["SUBSONIC_URL"]
        sub["username"] = env.get("SUBSONIC_USER", sub.get("username", ""))
        sub["password"] = env.get("SUBSONIC_PASSWORD", sub.get("password", ""))
    return config
