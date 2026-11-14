"""Read-only X2D mem:/26 probe; credentials and images stay in an ignored directory.

Python 3.10+, Pillow, cryptography. See docs/LIVE_CAMERA_FEASIBILITY.md.
Protocol reference: ClusterM/open-bamboo-networking/research/06.04-port-6000.md.
This tool exposes only login, session setup, and FILE_DOWNLOAD; no printer writes.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import socket
import ssl
import struct
import time
from datetime import datetime, timezone
from pathlib import Path

from cryptography import x509
from cryptography.x509.oid import NameOID
from PIL import Image, ImageChops, ImageStat

MAX_FRAME = 1024 * 1024
MAX_JSON = 4096
READ_CHUNK = 2048


class ProbeError(Exception):
    pass


def compact(obj: dict) -> bytes:
    return json.dumps(obj, separators=(",", ":")).encode("ascii")


def read_exact(sock, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ProbeError("unexpected_eof")
        data.extend(chunk)
    return bytes(data)


class FrameReader:
    """Read JSON prefix separately so an entire JPEG/fragment is never buffered."""

    def __init__(self, sock):
        self.sock = sock
        self.max_payload = 0

    def begin(self) -> tuple[dict | None, int]:
        length, magic, _, _reserved = struct.unpack("<IIII", read_exact(self.sock, 16))
        # X2D 01.02.00.00 leaves the high magic byte / reserved bytes nonzero
        # on some replies. Validate the stable 24-bit channel tag only.
        channel = magic & 0x00FFFFFF
        if length > MAX_FRAME or channel not in (0x0001013F, 0x0002013F):
            raise ProbeError(f"invalid_frame_header:length={length},magic={magic:#x}")
        self.max_payload = max(self.max_payload, length)
        if not length:
            return None, 0
        # Login ACKs have no JSON body.
        if channel == 0x0001013F:
            ack = read_exact(self.sock, length) if length <= 64 else b"invalid"
            if ack != b"\0" * 4:
                raise ProbeError("login_rejected")
            return None, 0
        prefix = bytearray()
        depth = 0
        quoted = escaped = False
        for _ in range(min(length, MAX_JSON)):
            byte = read_exact(self.sock, 1)[0]
            prefix.append(byte)
            if len(prefix) == 1 and byte != ord("{"):
                raise ProbeError("missing_json_prefix")
            if quoted:
                if escaped:
                    escaped = False
                elif byte == 92:
                    escaped = True
                elif byte == 34:
                    quoted = False
            elif byte == 34:
                quoted = True
            elif byte == 123:
                depth += 1
            elif byte == 125:
                depth -= 1
                if depth == 0:
                    remaining = length - len(prefix)
                    if remaining:
                        if remaining < 2 or read_exact(self.sock, 2) != b"\n\n":
                            raise ProbeError("invalid_binary_separator")
                        remaining -= 2
                    obj = json.loads(prefix)
                    if not isinstance(obj, dict):
                        raise ProbeError("invalid_json_object")
                    return obj, remaining
        raise ProbeError("json_prefix_too_long")

    def chunks(self, remaining: int):
        while remaining:
            chunk = read_exact(self.sock, min(READ_CHUNK, remaining))
            remaining -= len(chunk)
            yield chunk


class SnapshotSession:
    def __init__(self, config: dict, private_dir: Path):
        self.config = config
        self.private_dir = private_dir
        self.sock = None
        self.sequence = 1

    def __enter__(self):
        # LAN printer certs are self-signed. Check expected SN and pin the first
        # observed cert locally; never silently accept a later cert change.
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        raw = socket.create_connection((self.config["host"], 6000), timeout=8)
        try:
            self.sock = ctx.wrap_socket(raw, server_hostname=self.config["serial"])
            self.sock.settimeout(12)
            cert = self.sock.getpeercert(binary_form=True)
            parsed = x509.load_der_x509_certificate(cert)
            names = parsed.subject.get_attributes_for_oid(NameOID.COMMON_NAME)
            if not names or names[0].value != self.config["serial"]:
                raise ProbeError("printer_certificate_identity_mismatch")
            pin = self.private_dir / "printer-cert.der"
            if pin.exists() and pin.read_bytes() != cert:
                raise ProbeError("printer_certificate_changed")
            if not pin.exists():
                pin.write_bytes(cert)
            self.reader = FrameReader(self.sock)
            code = self.config["access_code"].encode("ascii")
            if len(code) != 8:
                raise ProbeError("invalid_access_code_length")
            self.send(0x0101013F, b"bblp\0\0\0\0" + code)
            ack, left = self.reader.begin()
            if ack is not None or left:
                raise ProbeError("invalid_login_ack")
            self.send(0x0102013F, compact({
                "sequence": 0, "mtype": 12291,
                "req": {"t_av": 0, "mtype": 12289, "peer_t": 3,
                        "pid": "", "ver": ""},
            }))
            ack, left = self.reader.begin()
            if not ack or ack.get("mtype") != 12291 or ack.get("result") != 0 or left:
                raise ProbeError("session_setup_rejected")
            return self
        except BaseException:
            (self.sock or raw).close()
            raise

    def __exit__(self, *_):
        if self.sock:
            self.sock.close()

    def send(self, magic: int, payload: bytes):
        header = struct.pack("<IIII", len(payload), magic, self.sequence, 0)
        self.sequence += 1
        self.sock.sendall(header + payload)

    def download(self, destination: Path) -> dict:
        sequence = self.sequence
        params = compact({"command": "get_project_file", "file_rel_path": "",
                          "peer_host": "studio", "sequence_id": sequence, "version": 1})
        request = {"mtype": 12289, "cmdtype": 4, "sequence": sequence,
                   "req": {"mem_dl_param_size": len(params), "offset": 0, "path": "mem:/26"}}
        self.send(0x0102013F, compact(request) + b"\n\n" + params)
        md5 = hashlib.md5()
        size = fragments = skipped_metadata = 0
        expected_total = None
        started = time.monotonic()
        temporary = destination.with_suffix(".part")
        try:
            with temporary.open("wb") as output:
                for _ in range(512):
                    obj, remaining = self.reader.begin()
                    if not obj or obj.get("sequence") != sequence or obj.get("cmdtype") != 4:
                        raise ProbeError("unexpected_download_reply")
                    if obj.get("result") not in (0, 1):
                        raise ProbeError("download_rejected_result_" + str(obj.get("result")))
                    reply = obj.get("reply") or {}
                    metadata_size = int(reply.get("mem_dl_param_size", 0))
                    if metadata_size:
                        if metadata_size != remaining:
                            raise ProbeError("metadata_length_mismatch")
                        for chunk in self.reader.chunks(remaining):
                            skipped_metadata += len(chunk)
                    else:
                        if remaining:
                            if int(reply.get("offset", -1)) != size:
                                raise ProbeError("fragment_offset_mismatch")
                            if int(reply.get("size", -1)) != remaining:
                                raise ProbeError("fragment_length_mismatch")
                            fragments += 1
                            for chunk in self.reader.chunks(remaining):
                                size += len(chunk)
                                if size > MAX_FRAME:
                                    raise ProbeError("image_too_large")
                                output.write(chunk)
                                md5.update(chunk)
                        if "total" in reply:
                            total = int(reply["total"])
                            if expected_total is not None and total != expected_total:
                                raise ProbeError("image_total_changed")
                            expected_total = total
                    if obj.get("result") == 0:
                        if not size or expected_total != size:
                            raise ProbeError("image_total_mismatch")
                        expected_md5 = reply.get("file_md5", "").lower()
                        if not expected_md5 or expected_md5 != md5.hexdigest():
                            raise ProbeError("image_md5_mismatch")
                        break
                else:
                    raise ProbeError("too_many_fragments")
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
        return {"bytes": size, "md5": md5.hexdigest(), "fragments": fragments,
                "metadata_bytes_skipped": skipped_metadata, "md5_verified": True,
                "download_ms": round((time.monotonic() - started) * 1000, 1),
                "largest_frame_payload": self.reader.max_payload,
                "receive_chunk_limit": READ_CHUNK, "certificate_cn_matched_and_pinned": True}


def inspect_image(path: Path, previous: Image.Image | None):
    with Image.open(path) as image:
        image.load()  # Validate the entire JPEG, not only its SOI/header.
        if image.format != "JPEG":
            raise ProbeError("not_jpeg")
        thumb = image.convert("L").resize((160, 90))
        result = {"width": image.width, "height": image.height,
                  "progressive": bool(image.info.get("progressive") or image.info.get("progression")),
                  "pixel_sha256": hashlib.sha256(thumb.tobytes()).hexdigest()}
        if previous is not None:
            result["mean_pixel_delta_0_255"] = round(ImageStat.Stat(ImageChops.difference(thumb, previous)).mean[0], 4)
        return result, thumb


def run(args) -> int:
    config = json.loads(args.config.read_text(encoding="utf-8-sig"))
    args.output.mkdir(parents=True, exist_ok=True)
    samples = []
    previous = None
    started = time.monotonic()
    for index in range(args.count):
        wait = started + index * args.interval - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        sample = {"index": index, "utc": datetime.now(timezone.utc).isoformat(),
                  "elapsed_s": round(time.monotonic() - started, 3)}
        try:
            image_path = args.output / f"frame-{index:03d}.jpg"
            with SnapshotSession(config, args.config.parent) as session:
                sample.update(session.download(image_path))
            image_info, previous = inspect_image(image_path, previous)
            sample.update(image_info)
            sample["ok"] = True
        except (OSError, ValueError, ProbeError) as exc:
            sample["ok"] = False
            # Exception strings from third party libraries can include private
            # connection data; emit only our fixed protocol codes or class name.
            sample["error"] = str(exc) if isinstance(exc, ProbeError) else type(exc).__name__
        samples.append(sample)
        report = {"samples": samples, "successes": sum(s["ok"] for s in samples),
                  "distinct_jpegs": len({s["md5"] for s in samples if s["ok"]}),
                  "distinct_decoded_thumbnails": len({s["pixel_sha256"] for s in samples if s["ok"]})}
        (args.output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps(sample), flush=True)
        if not sample["ok"]:
            # A rejected read should not turn into a long repeated probe.
            return 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True, help="ignored local JSON: host, serial, access_code")
    parser.add_argument("--output", type=Path, required=True, help="private output directory outside tracked files")
    parser.add_argument("--count", type=int, default=30)
    parser.add_argument("--interval", type=float, default=3)
    args = parser.parse_args()
    if args.count < 1 or args.interval < 0:
        parser.error("count must be positive and interval nonnegative")
    raise SystemExit(run(args))


if __name__ == "__main__":
    main()
