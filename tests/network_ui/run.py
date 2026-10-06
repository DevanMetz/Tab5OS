"""Exercise Diagnostics with real LVGL, native threads and synthetic services."""
import argparse
from pathlib import Path
import subprocess

CASES = (
    "lookup-cached", "lookup-delayed", "lookup-ipv6", "lookup-literal",
    "lookup-multiple", "lookup-multiple-delayed", "lookup-failure", "lookup-memory",
    "lookup-post-failure", "lookup-timeout", "cancel-dns", "home-dns",
    "cancel-queued", "late-dns", "source-edits", "ping-result", "ping-failure",
    "ping-cancel", "home-ping", "mdns-result", "mdns-empty", "mdns-failure",
    "mdns-init-failure", "mdns-cancel", "mdns-home", "mdns-cleanup", "task-failure",
    "input", "offline-refresh", "pending-start", "lifecycle", "resolver-concurrent",
    "resolver-short-deadline", "resolver-invalid",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("modes", nargs="*", choices=CASES)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    modes = args.modes or CASES
    for mode in modes:
        log_path = args.output / f"{mode}.log"
        try:
            with log_path.open("w", encoding="utf-8") as log:
                result = subprocess.run([args.executable, mode, str(args.output.resolve())],
                                        stdout=log, stderr=subprocess.STDOUT, text=True, timeout=20)
        except subprocess.TimeoutExpired as error:
            diagnostic = log_path.read_text(encoding="utf-8", errors="replace")
            raise RuntimeError(f"Diagnostics case {mode} timed out:\n{diagnostic}") from error
        diagnostic = log_path.read_text(encoding="utf-8", errors="replace")
        if result.returncode:
            raise RuntimeError(f"Diagnostics case {mode} failed ({result.returncode}):\n{diagnostic}")
        print(diagnostic.strip().splitlines()[-1], flush=True)
    print(f"Network Diagnostics: {len(modes)} worker/resolver/UI scenarios passed")


if __name__ == "__main__":
    main()
