"""Music providers. Each one wraps one catalog behind the same small interface.

To add a catalog, subclass `Provider` (see base.py), then either add it to
BUILTIN below or ship it as a plugin module that defines
`PROVIDERS = {"name": ProviderClass}` and list the module in the "plugins"
setting. Only add catalogs you are allowed to stream.
"""

import importlib

from .archive import ArchiveProvider
from .base import Context, Provider, StreamResponse
from .jamendo import JamendoProvider
from .local import LocalProvider
from .radio import RadioProvider
from .subsonic import SubsonicProvider

BUILTIN = {
    "local": LocalProvider,
    "subsonic": SubsonicProvider,
    "jamendo": JamendoProvider,
    "archive": ArchiveProvider,
    "radio": RadioProvider,
}

# Order used for mixed search results when the config does not set one.
DEFAULT_ORDER = ["local", "subsonic", "jamendo", "archive", "radio"]


def provider_classes(plugins=()):
    classes = dict(BUILTIN)
    for module_name in plugins:
        module = importlib.import_module(module_name)
        classes.update(getattr(module, "PROVIDERS", {}))
    return classes


__all__ = ["Context", "Provider", "StreamResponse", "BUILTIN", "DEFAULT_ORDER", "provider_classes"]
