"""Compile actual firmware functions with host-only I/O stubs for CR regressions.

Requires a native C compiler. CC may be a command, e.g. `cc` or `zig cc`.
Only function bodies are extracted; no duplicate implementation is maintained.
Run: python -m unittest discover -s tools -p test_camera_review.py -v
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = Path(__file__).resolve().parent / "tests"


def function(source, name):
    """Extract a top-level function, ignoring braces inside strings/comments."""
    match = re.search(r"^[A-Za-z_][^;\n]*\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", source, re.M)
    if not match:
        raise AssertionError(f"Production function missing: {name}")
    start = match.end() - 1
    depth = 0
    pattern = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}]', re.S)
    for token in pattern.finditer(source, start):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():token.end()]
    raise AssertionError(f"Unterminated function: {name}")


class CameraReviewTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        command = os.environ.get("CC")
        cls.cc = shlex.split(command) if command else [shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")]
        if not cls.cc[0]:
            raise unittest.SkipTest("Native C compiler required (set CC)")
        cls.directory = tempfile.TemporaryDirectory(prefix="passport-camera-review-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.work = Path(cls.directory.name)
        cls.executables = {}
        sources = {name: (ROOT / "main" / f"app_{name}.c").read_text(encoding="utf-8")
                   for name in ["camera", "mqtt", "printer", "config", "provision"]}
        main = (ROOT / "main/main.c").read_text(encoding="utf-8")
        reader = re.search(r"typedef struct \{.*?\} camera_reader_t;", sources["camera"], re.S).group()
        mqtt = sources["mqtt"][sources["mqtt"].index("static void sanitize_job"):sources["mqtt"].index("static void push_pushall")]
        roster = "\n".join(function(sources["config"], name) for name in ["config_machine_count", "config_load_printers"])
        for kind, replacements in {
            "power": {"PRINTING": function(sources["mqtt"], "app_mqtt_any_printing"),
                      "SEQUENCE": function(sources["mqtt"], "app_mqtt_screen_wake_sequence"), "MQTT": mqtt,
                      "WAKE": function(main, "screen_wake"), "TICK": function(main, "screen_off_tick"),
                      "KEY": function(main, "screen_off_on_key")},
            "worker": {"READER": reader, "CURRENT": function(sources["camera"], "current"),
                       "CLOSE": function(sources["camera"], "close_camera"),
                       "STATUS": function(sources["camera"], "status"), "WORKER": function(sources["camera"], "worker")},
            "transport": {"READER": reader, "CLOSE": function(sources["camera"], "close_camera"),
                          "READ": function(sources["camera"], "read_bytes_inner"),
                          "WRITE": function(sources["camera"], "write_bytes"),
                          "FETCH": function(sources["camera"], "fetch")},
            "transport_reconnect": {"READER": reader, "CLOSE": function(sources["camera"], "close_camera"),
                          "READ": function(sources["camera"], "read_bytes_inner"),
                          "WRITE": function(sources["camera"], "write_bytes"),
                          "FETCH": function(sources["camera"], "fetch")},
            "render": {"READER": reader, "DRAW": function(sources["camera"], "draw_stripe"),
                       "ALLOCATE": function(sources["camera"], "allocate_preview"),
                       "PRESENT": function(sources["camera"], "present_frame"),
                       "OUTPUT": function(sources["camera"], "jpeg_output"),
                       "DECODE": function(sources["camera"], "decode")},
            "pages": {"PAGES": function(sources["printer"], "apply_page_vis")},
            "pin": {"READER": reader, "CURRENT": function(sources["camera"], "current"),
                    "CONNECT": function(sources["camera"], "connect_camera")},
            "topology": {"MQTT": mqtt, "CLIENT_ID": function(sources["mqtt"], "make_client_id"),
                         "REFRESH": function(sources["printer"], "refresh_all")},
            "settings": {"ROSTER": roster, "HELPERS": "\n".join(function(sources["provision"], name)
                for name in ["hexval", "url_decode", "form_field", "json_str", "utf8_copy"]),
                "GET": function(sources["provision"], "camera_get_handler"),
                "POST": function(sources["provision"], "camera_save_handler")},
        }.items():
            fixture = "transport" if kind.startswith("transport") else kind
            text = (FIXTURES / f"camera_review_{fixture}.c").read_text(encoding="utf-8")
            for key, value in replacements.items():
                marker = f"/* @{key}@ */"
                if marker not in text:
                    raise AssertionError(f"Missing fixture marker {marker}")
                text = text.replace(marker, value)
            source = cls.work / f"{kind}.c"
            source.write_text(text, encoding="utf-8")
            output = cls.work / (kind + (".exe" if os.name == "nt" else ""))
            flags = ["-DCAMERA_REUSE_SESSION=" + ("0" if kind == "transport_reconnect" else "1")] if kind.startswith("transport") else []
            result = subprocess.run(cls.cc + flags + ["-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-sign-compare",
                "-I", str(ROOT / "main"), "-I", str(FIXTURES), str(source),
                str(ROOT / "main/app_notify_transition.c"), str(ROOT / "main/app_screen_policy.c"),
                str(ROOT / "main/camera_frame.c"), str(ROOT / "main/camera_rate.c"),
                str(ROOT / "main/camera_geometry.c"),
                "-o", str(output)], capture_output=True, encoding="utf-8", errors="replace")
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.executables[kind] = output

    def run_case(self, kind, case):
        result = subprocess.run([str(self.executables[kind]), case], capture_output=True, encoding="utf-8", errors="replace", timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_offline_poll_retains_verified_image(self):
        self.run_case("worker", "offline")

    def test_camera_snapshot_session_is_reused_after_verified_frame(self):
        self.run_case("transport", "reuse")

    def test_camera_session_reconnects_after_request_or_decode_failure(self):
        self.run_case("transport", "failure")

    def test_camera_session_setup_failure_is_not_reused(self):
        self.run_case("transport", "setup-failure")

    def test_camera_reused_session_rejects_changed_identity_during_io(self):
        self.run_case("transport", "identity-io")

    def test_camera_reconnect_mode_and_unverified_stream_models_still_close(self):
        self.run_case("transport_reconnect", "reconnect")
        self.run_case("transport", "stream")

    def test_verified_preview_commits_complete_rgb565_and_dynamic_letterboxes(self):
        for case in ["landscape", "portrait", "small"]:
            self.run_case("render", case)

    def test_preview_errors_and_page_cancellation_retain_previous_image(self):
        for case in ["checksum", "cancel", "cancel-header", "input-failure", "decode-failure", "busy", "allocation", "low-memory"]:
            self.run_case("render", case)

    def test_partial_lcd_failure_ends_stream(self):
        self.run_case("render", "draw-failure")

    def test_measured_fps_window_and_loading_deadline(self):
        self.run_case("render", "rate")

    def test_navigation_bounds_page_memory_during_asynchronous_camera_stop(self):
        self.run_case("pages", "cycle")

    def test_mqtt_client_ids_distinguish_boards_and_boots(self):
        self.run_case("topology", "client-id")

    def test_inactive_print_states_sleep_with_or_without_usb(self):
        self.run_case("power", "inactive")

    def test_finishing_starts_idle_timeout_while_powered(self):
        self.run_case("power", "finish")

    def test_unplugging_running_print_starts_idle_timeout(self):
        self.run_case("power", "unplug")

    def test_running_print_on_battery_still_sleeps(self):
        self.run_case("power", "battery")

    def test_any_configured_printing_machine_keeps_powered_screen_on(self):
        self.run_case("power", "multi")

    def test_stale_or_uninitialized_printing_state_does_not_keep_awake(self):
        self.run_case("power", "offline")

    def test_usb_attach_wakes_once_even_without_printing(self):
        self.run_case("power", "wake")

    def test_start_and_finish_wake_on_both_power_sources(self):
        self.run_case("power", "events")

    def test_initial_historical_states_never_generate_wake_events(self):
        self.run_case("power", "initial")

    def test_other_printer_events_and_resume_are_detected_independently(self):
        self.run_case("power", "multi-event")

    def test_event_at_idle_deadline_wins_over_screen_off(self):
        self.run_case("power", "priority")

    def test_busy_display_retries_event_and_times_from_success(self):
        self.run_case("power", "retry")

    def test_pending_events_coalesce_and_sequence_wrap_is_detected(self):
        self.run_case("power", "wrap")

    def test_stateless_telemetry_and_reconnect_snapshot_do_not_repeat_event(self):
        self.run_case("power", "telemetry")

    def test_event_while_lit_extends_deadline_only_once(self):
        self.run_case("power", "lit-reset")

    def test_delayed_key_wake_still_consumes_the_original_click(self):
        self.run_case("power", "key-retry")

    def test_key_wake_resumes_camera_and_consumes_delayed_click(self):
        self.run_case("power", "key")

    def test_pin_save_failure_retries_until_persisted(self):
        self.run_case("pin", "retry")

    def test_identity_failure_does_not_persist_pin(self):
        self.run_case("pin", "identity")

    def test_unseen_certificate_does_not_persist_pin(self):
        self.run_case("pin", "unseen")

    def test_failed_partial_frame_invalidates_image(self):
        self.run_case("worker", "partial")

    def test_failed_fetch_without_drawing_retains_image(self):
        self.run_case("worker", "untouched")

    def test_first_offline_poll_does_not_invent_image(self):
        self.run_case("worker", "first-offline")

    def test_state_delta_does_not_cancel_saved_ams4(self):
        self.run_case("topology", "delta-then-topology")

    def test_nested_empty_topology_confirms_removed_ams(self):
        self.run_case("topology", "nested-empty")

    def test_direct_empty_topology_confirms_removed_ams(self):
        self.run_case("topology", "direct-empty")

    def test_partial_ams_status_is_not_topology(self):
        self.run_case("topology", "partial")

    def test_other_printer_topology_does_not_cancel_restore(self):
        self.run_case("topology", "other-printer")

    def test_truncated_or_null_topology_is_not_confirmation(self):
        self.run_case("topology", "invalid")

    def test_legacy_camera_get_and_save_use_same_identity(self):
        self.run_case("settings", "legacy")

    def test_nvs_printer_takes_precedence(self):
        self.run_case("settings", "nvs")

    def test_sparse_nvs_printer_does_not_inject_legacy(self):
        self.run_case("settings", "sparse")

    def test_placeholder_or_empty_legacy_is_not_a_printer(self):
        self.run_case("settings", "placeholder")

    def test_legacy_identity_survives_nvs_load_failure(self):
        self.run_case("settings", "load-error")


if __name__ == "__main__":
    unittest.main()
