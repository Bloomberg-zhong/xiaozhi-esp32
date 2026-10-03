"""Optional bounded image normalization for the firmware's JPEG decoder."""

import io
import warnings

from .models import ArtworkUnavailable, ProviderError

try:
    from PIL import Image, ImageOps
except ImportError:
    Image = ImageOps = None

MAX_COVER_BYTES = 32 * 1024
MAX_INPUT_PIXELS = 16 * 1024 * 1024


def normalize_cover(data: bytes, size: int = 128) -> bytes:
    if Image is None:
        raise ArtworkUnavailable("install requirements-artwork.txt for album covers")
    size = max(1, min(int(size), 128))
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            with Image.open(io.BytesIO(data)) as image:
                if image.width * image.height > MAX_INPUT_PIXELS:
                    raise ProviderError("cover image too large")
                image = ImageOps.exif_transpose(image)
                image.thumbnail((size, size), Image.Resampling.LANCZOS)
                image = image.convert("RGB")
                output = io.BytesIO()
                image.save(output, format="JPEG", quality=85, progressive=False, optimize=False)
                result = output.getvalue()
                if len(result) > MAX_COVER_BYTES:
                    raise ProviderError("normalized cover too large")
                return result
    except ProviderError:
        raise
    except (OSError, ValueError, SyntaxError, Image.DecompressionBombError,
            Image.DecompressionBombWarning) as error:
        raise ProviderError("invalid cover image") from error
