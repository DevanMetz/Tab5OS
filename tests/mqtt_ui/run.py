"""MQTT lifecycle checks with actual app/LVGL/files and a controlled SDK model."""
import argparse
import csv
from pathlib import Path
import subprocess
from wire_fixtures import write as write_wire_fixtures

CASES = (
    "idle", "task-failure", "buffer-failure", "init-failure", "register-failure", "start-failure",
    "subscribe-publish", "stop", "home", "destroy", "late-events", "pending-stop", "log-optout", "log-empty",
    "log-success", "log-sync", "log-close", "log-home", "log-reentry", "log-repair-error",
    "log-open-error", "log-flush-error", "log-sync-error", "log-close-error", "error-report", "lifecycle",
    "subscribe-stall", "publish-stall", "subscribe-home", "publish-home", "subscribe-stop", "publish-stop",
    "queued-subscribe", "queued-publish", "action-allocation", "subscribe-error", "publish-error", "publish-full",
    "publish-qos0", "publish-qos2", "publish-bounds", "pending-action", "actions-repeat",
    "active-log-sync", "active-log-close", "active-log-home", "active-log-stop", "active-log-action",
    "active-log-off", "active-log-overflow", "active-log-error", "active-log-error-home",
    "active-log-error-report", "active-log-before-ui", "active-log-ui-drop", "logged-lifecycle",
    "pending-connect-refresh",
    "receive-zero-start", "receive-fragments", "receive-empty", "receive-binary", "receive-cap",
    "receive-topic-view", "receive-invalid", "receive-replace", "receive-interruption",
    "receive-home", "receive-abandoned", "receive-soak",
    "connection-tls", "connection-wss", "connection-mqtt", "connection-ws",
    "connection-uri-invalid", "connection-uri-bounds", "connection-credential-bounds",
    "connection-profile-bounds", "connection-confirmation", "connection-transport-error",
    "connection-transport-socket", "connection-transport-tls", "connection-transport-verify",
    "connection-transport-combined", "connection-transport-empty", "connection-transport-extremes",
    "connection-confirmation-collision-uri", "connection-confirmation-collision-username",
    "connection-confirmation-collision-password", "connection-confirmation-edits",
    "connection-refused", "connection-generic-error", "connection-subscription-rejected",
    "connection-error-storage", "connection-error-overflow", "connection-error-history",
    "profile-load-tls", "profile-load-wss", "profile-overwrite", "profile-delete-expiry",
    "profile-delete-handoffs", "profile-invalid", "profile-wrong-type", "profile-read-errors",
    "profile-read-short", "profile-read-gone", "profile-save-open", "profile-save-before",
    "profile-save-after", "profile-save-commit", "profile-delete-open", "profile-delete-before",
    "profile-delete-after", "profile-delete-commit", "profile-live-actions", "profile-repeat",
    "profile-empty", "profile-overwrite-errors", "profile-repair",
    "profile-pending-actions",
    "wire-connect", "wire-subscribe", "wire-publish-qos0", "wire-publish-qos1", "wire-publish-qos2",
    "wire-publish-empty", "wire-receive-small", "wire-receive-zero-start", "wire-receive-fragments",
    "wire-receive-home", "wire-receive-abandoned", "wire-header-flags", "wire-lengths",
    "copy-empty", "copy-binary", "copy-bounds", "copy-fragments", "copy-latest", "copy-invalid",
    "copy-overflow", "copy-home", "copy-abandoned", "copy-clear", "copy-confirmation", "copy-soak",
    "paste-empty", "paste-binary", "paste-bounds", "paste-modes", "paste-capture",
    "paste-queued", "paste-home", "paste-confirmation", "paste-error", "paste-soak",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("modes", nargs="*", choices=CASES)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    modes = args.modes or CASES
    if any(mode.startswith("wire-") for mode in modes):
        write_wire_fixtures(args.output / "wire-fixtures")
    for mode in modes:
        log_path = args.output / f"{mode}.log"
        try:
            with log_path.open("w", encoding="utf-8") as log:
                result = subprocess.run([args.executable, mode, str(args.output.resolve())],
                                        stdout=log, stderr=subprocess.STDOUT, timeout=45 if mode == "paste-bounds" else 20)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f"MQTT case {mode} timed out:\n{log_path.read_text(errors='replace')}") from error
        diagnostic = log_path.read_text(encoding="utf-8", errors="replace")
        if result.returncode:
            raise RuntimeError(f"MQTT case {mode} failed ({result.returncode}):\n{diagnostic}")
        csv_path = args.output / f"storage-{mode}" / "MQTTLOG.CSV"
        if csv_path.exists():
            content = csv_path.read_text(encoding="utf-8")
            for secret in ("FIXTURE_USER_SECRET", "FIXTURE_PASSWORD_SECRET", "FIXTURE_PAYLOAD_SECRET", "FIXTURE_RX_SECRET"):
                assert secret not in content, (mode, secret)
            rows = list(csv.DictReader(content.splitlines()))
            if mode in ("log-success", "log-sync", "log-close", "log-home", "log-reentry"):
                assert len(rows) == 4, (mode, rows)
                assert [row["direction"] for row in rows] == ["TX", "RX", "TX", "RX"]
                assert [row["topic"] for row in rows] == ["fixture/topic", "fixture/rx"] * 2
                assert all(row["qos"] == "1" for row in rows)
                assert [row["retained"] for row in rows] == ["0", "1"] * 2
                assert [row["payload_bytes"] for row in rows] == ["22", "17"] * 2
                assert [row["outcome"] for row in rows] == ["queued", "received"] * 2
            elif (mode.startswith("log-") and mode.endswith("-error")) or mode == "error-report":
                assert 2 <= len(rows) <= 3, (mode, rows)
                assert [row["direction"] for row in rows[:2]] == ["TX", "RX"], (mode, rows)
            elif mode in ("publish-qos0", "publish-qos2", "publish-home", "publish-stop"):
                assert len(rows) == 1 and rows[0]["direction"] == "TX", (mode, rows)
                assert rows[0]["topic"] == "fixture/topic" and rows[0]["payload_bytes"] == "22", (mode, rows)
                expected_qos = "0" if mode == "publish-qos0" else "2" if mode == "publish-qos2" else "1"
                assert rows[0]["qos"] == expected_qos and rows[0]["retained"] == ("0" if mode == "publish-qos0" else "1"), (mode, rows)
            elif mode.startswith("active-log-") or mode == "logged-lifecycle":
                if mode == "active-log-overflow":
                    assert [row["topic"] for row in rows] == [f"fixture/burst/{i}" for i in (0, *range(13, 21))], (mode, rows)
                elif mode == "active-log-ui-drop":
                    assert [row["topic"] for row in rows] == [f"fixture/burst/{i}" for i in range(12)], (mode, rows)
                elif mode == "active-log-error":
                    assert [row["topic"] for row in rows] == ["fixture/log/failed", "fixture/log/recovered"], (mode, rows)
                elif mode == "active-log-before-ui":
                    assert [row["topic"] for row in rows] == ["fixture/log/accepted"], (mode, rows)
                elif mode == "active-log-action":
                    assert [row["direction"] for row in rows] == ["RX", "TX"] and [row["topic"] for row in rows] == ["fixture/burst/0", "fixture/topic"], (mode, rows)
                elif mode == "logged-lifecycle":
                    assert len(rows) == 52 and [row["direction"] for row in rows] == ["TX", "RX"] * 26, (mode, rows)
                else:
                    assert len(rows) == 1, (mode, rows)
                assert all(row["qos"] == "1" and row["outcome"] == ("received" if row["direction"] == "RX" else "queued") for row in rows), (mode, rows)
                assert all(row["retained"] == ("1" if row["direction"] == "RX" else "0") for row in rows), (mode, rows)
            elif mode in ("connection-error-storage", "connection-error-overflow"):
                assert [row["topic"] for row in rows] == [f"fixture/error/{i}" for i in
                    ((0, *range(13, 21)) if mode.endswith("-overflow") else (0,))], (mode, rows)
                assert all(row["direction"] == "RX" and row["qos"] == "1" and row["retained"] == "1"
                           and row["payload_bytes"] == "17" and row["outcome"] == "received" for row in rows)
            elif mode in ("paste-empty", "paste-binary", "paste-bounds"):
                lengths = list(range(129)) if mode == "paste-bounds" else [0 if mode == "paste-empty" else 9]
                assert [int(row["payload_bytes"]) for row in rows] == lengths, (mode, rows)
                assert all(row["direction"] == "TX" and row["topic"] == "fixture/topic" and
                           row["qos"] == ("0" if mode == "paste-empty" else "1") and
                           row["retained"] == "0" and row["outcome"] == "queued" for row in rows), (mode, rows)
            elif mode.startswith("wire-publish-"):
                empty = mode == "wire-publish-empty"
                assert len(rows) == 1 and rows[0] == {
                    "unix_time": rows[0]["unix_time"], "direction": "TX", "topic": "a/b",
                    "qos": "1" if empty else mode[-1], "retained": "0" if empty else "1",
                    "payload_bytes": "0" if empty else "3", "outcome": "queued",
                }, (mode, rows)
            elif mode.startswith("wire-receive-"):
                small = mode == "wire-receive-small"
                zero = mode == "wire-receive-zero-start"
                large = mode == "wire-receive-fragments"
                assert len(rows) == (3 if small else 1), (mode, rows)
                assert all(row["direction"] == "RX" and row["retained"] == "1" for row in rows)
                assert [row["qos"] for row in rows] == (["0", "1", "2"] if small else ["2"])
                assert [row["payload_bytes"] for row in rows] == (["0", "9", "9"] if small else ["1024" if large else "17"])
                assert all(row["topic"] == ("t" * 127 if zero else "fixture/wire") for row in rows)
                assert all(row["outcome"] == ("preview_truncated" if zero or large else "received") for row in rows)
            elif mode.startswith("receive-"):
                assert all(row["direction"] == "RX" for row in rows), (mode, rows)
                assert all(row["qos"] == ("0" if mode == "receive-empty" else "2") for row in rows), (mode, rows)
                assert all(row["retained"] == ("0" if mode == "receive-empty" else "1") for row in rows), (mode, rows)
                if mode == "receive-cap":
                    assert [row["payload_bytes"] for row in rows] == ["512", "513", "1024"], (mode, rows)
                    assert [row["outcome"] for row in rows] == ["received", "preview_truncated", "preview_truncated"], (mode, rows)
                elif mode == "receive-topic-view":
                    assert [row["topic"] for row in rows] == ["t" * 127, "t" * 127, "fixture/..", "fixture/line...", 'fixture/"quoted"'], (mode, rows)
                    assert [row["outcome"] for row in rows] == ["received", "preview_truncated", "preview_truncated", "preview_truncated", "received"], (mode, rows)
                    assert all(row["payload_bytes"] == "17" for row in rows), (mode, rows)
                else:
                    assert len(rows) == (55 if mode == "receive-soak" else 1), (mode, rows)
                    assert all(row["topic"] == ("fixture/replacement" if mode == "receive-replace" else "fixture/fragment") for row in rows), (mode, rows)
                    expected_length = "0" if mode == "receive-empty" else "9" if mode == "receive-binary" else "16" if mode == "receive-replace" else "17"
                    assert all(row["payload_bytes"] == expected_length and row["outcome"] == "received" for row in rows), (mode, rows)
        elif mode in ("log-success", "log-sync", "log-close", "log-home", "log-reentry",
                      "publish-qos0", "publish-qos2", "publish-home", "publish-stop",
                      "connection-error-storage", "connection-error-overflow", "paste-empty", "paste-binary", "paste-bounds") or mode.startswith("active-log-") or mode == "logged-lifecycle" or (mode.startswith("receive-") and mode != "receive-abandoned") or mode.startswith("wire-publish-") or (mode.startswith("wire-receive-") and mode != "wire-receive-abandoned"):
            raise AssertionError(f"Missing metadata log for {mode}")
        print(diagnostic.strip().splitlines()[-1], flush=True)
    print(f"MQTT Console: {len(modes)} lifecycle/worker/LVGL/storage scenarios passed")


if __name__ == "__main__":
    main()
