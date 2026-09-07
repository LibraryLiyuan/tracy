"""Human console decoding only; raw subprocess evidence is never rewritten."""

import locale


class ToolOutputDecoder:
    def __init__(self, *, fallback_encoding: str | None = None):
        self.fallback_encoding = fallback_encoding or locale.getencoding()
        self.pending = b""

    def _decode(self, payload: bytes) -> str:
        try:
            return payload.decode("utf-8")
        except UnicodeDecodeError:
            return payload.decode(self.fallback_encoding, errors="backslashreplace")

    def feed(self, payload: bytes) -> str:
        self.pending += payload
        end = self.pending.rfind(b"\n") + 1
        if end == 0:
            return ""
        complete, self.pending = self.pending[:end], self.pending[end:]
        return self._decode(complete)

    def finish(self) -> str:
        complete, self.pending = self.pending, b""
        return self._decode(complete)
