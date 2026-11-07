# Camera review regressions

Run from the repository root:

```sh
CC=cc python -m unittest discover -s tools -p 'test_*.py' -v
```

On Windows with Zig installed:

```powershell
$env:CC='zig cc'
python -m unittest discover -s tools -p 'test_*.py' -v
```

`test_camera_review.py` compiles the actual firmware function bodies in temporary
native C executables. Hardware, task scheduling, TLS and NVS I/O are stubbed in
these fixtures; no duplicate parser, worker, navigation or HTTP handler logic is
maintained. A missing native compiler skips this suite and must not be reported
as passing it.

The firmware regression cases cover:

- Successful image followed by an offline poll, untouched failure, partial draw,
  or startup with no connection.
- Saved AMS4 restoration across an early g-code delta, eventual topology,
  explicit empty lists, missing/invalid lists and another printer's report.
- Camera settings GET/POST with legacy identity, NVS precedence, sparse slots,
  empty/example identities and NVS read failure.
- First certificate-pin persistence failure and retry, plus rejection before
  persistence when the certificate identity is invalid or unseen.
- Screen idle timeout on USB and battery across inactive, paused, finished,
  failed, unknown, offline and running states; print completion and unplugging;
  activity on another configured printer; key wake and camera resume; plugging
  in while idle remaining dark and a powered running print waking the display.

The six tests in `test_probe_camera_snapshot.py` exercise the host-side protocol
probe separately. These checks do not replace an ESP-IDF build or real-device
validation of TLS, power management and display behavior.
