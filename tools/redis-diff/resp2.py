"""resp2.py -- the small RESP2 client both redis-diff steps use.

Written on a bare socket on purpose: a third-party client would normalise replies
before this harness could see them, and the point of the harness is to compare
what the server actually answers with what the modules answer.
"""
import base64
import socket


class Resp:
    """Minimal RESP2 reader/writer: enough for commands and their replies."""

    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=30)
        self.buf = b""

    def _line(self):
        while b"\r\n" not in self.buf:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError("connection closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def _read(self):
        line = self._line()
        t, rest = line[:1], line[1:]
        if t == b"+":
            return rest.decode("utf-8", "replace")
        if t == b"-":
            return {"error": rest.decode("utf-8", "replace")}
        if t == b":":
            return int(rest)
        if t == b"$":
            n = int(rest)
            if n < 0:
                return None
            data = self._read_bytes(n)
            try:
                return data.decode("utf-8")
            except UnicodeDecodeError:
                return {"b64": base64.b64encode(data).decode("ascii")}
        if t == b"*":
            n = int(rest)
            if n < 0:
                return None
            return [self._read() for _ in range(n)]
        raise ValueError("unknown RESP type %r in %r" % (t, line))

    def _read_bytes(self, n):
        while len(self.buf) < n + 2:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError("connection closed")
            self.buf += chunk
        data, self.buf = self.buf[:n], self.buf[n + 2:]
        return data

    def cmd(self, *args):
        out = [b"*%d\r\n" % len(args)]
        for a in args:
            if isinstance(a, bytes):
                pass                        # a raw value, exactly as stored
            elif isinstance(a, bool):
                a = b"1" if a else b"0"
            elif isinstance(a, int):
                a = str(a).encode()
            elif isinstance(a, float):
                # the log's numeric args are exact decimals; Redis parses text
                a = repr(a).encode()
            else:
                a = str(a).encode("utf-8")
            out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        self.s.sendall(b"".join(out))
        return self._read()



def as_hex(v):
    """A bulk reply's raw bytes, as hex, whichever way RESP delivered them.

    Resp decodes a valid-UTF-8 bulk to str and everything else to base64, so both
    paths have to be folded back to the bytes the server actually stored.
    """
    if v is None:
        return None
    if isinstance(v, dict) and "b64" in v:
        return base64.b64decode(v["b64"]).hex()
    if isinstance(v, str):
        return v.encode("utf-8").hex()
    raise ValueError("not a bulk reply: %r" % (v,))
