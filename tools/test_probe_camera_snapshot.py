"""Protocol regressions for the snapshot probe; no network or credentials needed."""
import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path

from probe_camera_snapshot import FrameReader, ProbeError, SnapshotSession, compact


class FragmentedSocket:
    def __init__(self, wire):
        self.wire = bytearray(wire)
        self.max_request = 0

    def recv(self, size):
        self.max_request = max(self.max_request, size)
        # Split even the fixed-size frame header / JSON separator across reads.
        size = min(size, 3, len(self.wire))
        result = bytes(self.wire[:size])
        del self.wire[:size]
        return result

    def sendall(self, _):
        pass


def frame(obj, binary=b""):
    body = compact(obj) + (b"\n\n" + binary if binary else b"")
    # Observed X2D replies can have nonzero high magic / reserved bytes.
    return struct.pack("<IIII", len(body), 0x2202013F, 4, 0x1234) + body


class SnapshotProtocolTests(unittest.TestCase):
    def test_fragmented_json_and_binary_keep_all_image_bytes(self):
        payload = b'\xff\xd8' + bytes(range(256)) * 21 + b'\n\xff\xd9'
        obj = {"reply": {"note": 'braces {inside} and escaped " quote'}, "result": 0}
        sock = FragmentedSocket(frame(obj, payload))
        reader = FrameReader(sock)
        parsed, left = reader.begin()
        self.assertEqual(parsed, obj)
        self.assertEqual(b"".join(reader.chunks(left)), payload)
        self.assertLessEqual(sock.max_request, 2048)

    def download_fixture(self, directory, *, corrupt=False, offset=0):
        payload = b'\xff\xd8' + b'\x00\xff\n' * 2000 + b'\xff\xd9'
        metadata = b'{"result":0}'
        wire = frame({"sequence": 1, "cmdtype": 4, "result": 1,
                      "reply": {"mem_dl_param_size": len(metadata)}}, metadata)
        wire += frame({"sequence": 1, "cmdtype": 4, "result": 0,
                       "reply": {"total": len(payload), "size": len(payload), "offset": offset,
                                 "file_md5": "0" * 32 if corrupt else hashlib.md5(payload).hexdigest()}}, payload)
        sock = FragmentedSocket(wire)
        session = SnapshotSession({}, directory)
        session.sock = sock
        session.reader = FrameReader(sock)
        output = directory / "sample.jpg"
        return session, output, payload

    def test_download_skips_metadata_and_checks_integrity(self):
        with tempfile.TemporaryDirectory() as temp:
            session, output, payload = self.download_fixture(Path(temp))
            result = session.download(output)
            self.assertEqual(output.read_bytes(), payload)
            self.assertTrue(result["md5_verified"])
            self.assertEqual(result["metadata_bytes_skipped"], 12)

    def test_bad_md5_never_publishes_partial_image(self):
        with tempfile.TemporaryDirectory() as temp:
            session, output, _ = self.download_fixture(Path(temp), corrupt=True)
            with self.assertRaisesRegex(ProbeError, "image_md5_mismatch"):
                session.download(output)
            self.assertFalse(output.exists())
            self.assertFalse(output.with_suffix(".part").exists())

    def test_wrong_offset_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            session, output, _ = self.download_fixture(Path(temp), offset=1)
            with self.assertRaisesRegex(ProbeError, "fragment_offset_mismatch"):
                session.download(output)

    def test_truncated_response_is_rejected(self):
        reader = FrameReader(FragmentedSocket(frame({"ok": 1}, b"data")[:-2]))
        _, left = reader.begin()
        with self.assertRaisesRegex(ProbeError, "unexpected_eof"):
            list(reader.chunks(left))

    def test_oversized_frame_is_rejected_before_body_read(self):
        reader = FrameReader(FragmentedSocket(struct.pack("<IIII", 1024 * 1024 + 1, 0x2013F, 1, 0)))
        with self.assertRaisesRegex(ProbeError, "invalid_frame_header"):
            reader.begin()


if __name__ == "__main__":
    unittest.main()
