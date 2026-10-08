"""Package resolved LVGL and the pinned SDK source subset for Windows UI CI."""
import argparse
import hashlib
import io
import json
import re
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDK_FILES = (
    "components/http_parser/http_parser.c",
    "components/http_parser/http_parser.h",
    "components/esp_common/include/esp_idf_version.h",
    "LICENSE",
    "components/mqtt/esp-mqtt/lib/mqtt_msg.c",
    "components/mqtt/esp-mqtt/lib/include/mqtt_msg.h",
    "components/mqtt/esp-mqtt/lib/include/mqtt_config.h",
    "components/mqtt/esp-mqtt/lib/include/platform.h",
    "components/mqtt/esp-mqtt/LICENSE",
    "components/mqtt/esp-mqtt/mqtt_client.c",
    "components/mqtt/esp-mqtt/lib/mqtt_outbox.c",
    "components/mqtt/esp-mqtt/include/mqtt_client.h",
    "components/mqtt/esp-mqtt/include/mqtt_supported_features.h",
    "components/mqtt/esp-mqtt/lib/include/mqtt_client_priv.h",
    "components/mqtt/esp-mqtt/lib/include/mqtt_outbox.h",
    "components/mqtt/esp-mqtt/host_test/mocks/include/sys/queue.h",
    "components/tcp_transport/transport.c",
    "components/tcp_transport/transport_internal.c",
    "components/tcp_transport/include/esp_transport.h",
    "components/tcp_transport/include/esp_transport_tcp.h",
    "components/tcp_transport/include/esp_transport_ssl.h",
    "components/tcp_transport/include/esp_transport_ws.h",
    "components/tcp_transport/private_include/esp_transport_internal.h",
    "components/esp_http_client/esp_http_client.c",
    "components/esp_http_client/include/esp_http_client.h",
    "components/esp_http_client/lib/http_header.c",
    "components/esp_http_client/lib/http_utils.c",
    "components/esp_http_client/lib/include/http_header.h",
    "components/esp_http_client/lib/include/http_utils.h",
    "components/esp_http_client/lib/include/http_auth.h",
    "components/json/cJSON/cJSON.c",
    "components/json/cJSON/cJSON.h",
    "components/json/cJSON/LICENSE",
    "components/esp_https_ota/src/esp_https_ota.c",
    "components/esp_https_ota/include/esp_https_ota.h",
    "components/esp_app_format/include/esp_app_desc.h",
    "components/bootloader_support/include/esp_app_format.h",
    "components/mbedtls/mbedtls/library/sha256.c",
    "components/mbedtls/mbedtls/library/common.h",
    "components/mbedtls/mbedtls/library/alignment.h",
    "components/mbedtls/mbedtls/include/mbedtls/build_info.h",
    "components/mbedtls/mbedtls/include/mbedtls/config_adjust_legacy_crypto.h",
    "components/mbedtls/mbedtls/include/mbedtls/config_adjust_x509.h",
    "components/mbedtls/mbedtls/include/mbedtls/config_adjust_ssl.h",
    "components/mbedtls/mbedtls/include/mbedtls/check_config.h",
    "components/mbedtls/mbedtls/include/mbedtls/sha256.h",
    "components/mbedtls/mbedtls/include/mbedtls/private_access.h",
    "components/mbedtls/mbedtls/include/mbedtls/platform_util.h",
    "components/mbedtls/mbedtls/include/mbedtls/error.h",
    "components/mbedtls/mbedtls/include/mbedtls/platform.h",
    "components/mbedtls/mbedtls/LICENSE",
)
LVGL_INPUTS = (
    "src", "lvgl.h", "lvgl_private.h", "lv_version.h", ".component_hash",
    "idf_component.yml", "LICENCE.txt", "COPYRIGHTS.md",
)


def locked_component(lock, name):
    match = re.search(rf"(?m)^  {re.escape(name)}:\n((?:^    .*\n)*)", lock)
    if not match:
        raise ValueError(f"Missing locked component: {name}")
    version = re.search(r"(?m)^    version: ['\"]?([0-9.]+)['\"]?$", match[1])
    if not version:
        raise ValueError(f"Missing locked version: {name}")
    component_hash = re.search(r"(?m)^    component_hash: ([0-9a-f]{64})$", match[1])
    return version[1], component_hash[1] if component_hash else None


def header_version(path, prefix):
    text = path.read_text(encoding="utf-8")
    parts = []
    for suffix in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"(?m)^#define {prefix}_{suffix}\s+(\d+)\s*$", text)
        if not match:
            raise ValueError(f"Missing {prefix}_{suffix} in {path}")
        parts.append(match[1])
    return ".".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", required=True, type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/native-ui-sources.tar.gz")
    args = parser.parse_args()
    lock = (ROOT / "dependencies.lock").read_text(encoding="utf-8")
    idf_version, _ = locked_component(lock, "idf")
    lvgl_version, lvgl_hash = locked_component(lock, "lvgl/lvgl")
    lvgl = ROOT / "managed_components/lvgl__lvgl"
    if header_version(args.idf_path / SDK_FILES[2], "ESP_IDF_VERSION") != idf_version:
        raise ValueError("SDK source version differs from dependencies.lock")
    if header_version(lvgl / "lv_version.h", "LVGL_VERSION") != lvgl_version:
        raise ValueError("LVGL source version differs from dependencies.lock")
    if not lvgl_hash or (lvgl / ".component_hash").read_text(encoding="ascii").strip() != lvgl_hash:
        raise ValueError("LVGL component marker differs from dependencies.lock")
    manifest = {
        "idfVersion": idf_version, "lvglVersion": lvgl_version,
        "lvglComponentHash": lvgl_hash,
        "dependenciesLockSha256IgnoringLineEndings": hashlib.sha256(lock.encode("utf-8")).hexdigest(),
        "sdkFileSha256": {name: hashlib.sha256((args.idf_path / name).read_bytes()).hexdigest()
                          for name in SDK_FILES},
        "sdkSourceSubsetOnly": True,
        "lvglNativeBuildInputsOnly": True,
    }
    manifest_bytes = (json.dumps(manifest, indent=2) + "\n").encode("utf-8")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, "w:gz") as archive:
        for name in LVGL_INPUTS:
            archive.add(lvgl / name, arcname=f"managed_components/lvgl__lvgl/{name}")
        for name in SDK_FILES:
            archive.add(args.idf_path / name, arcname=f"build/native-ui-idf/{name}")
        info = tarfile.TarInfo("build/native-ui-sources.json")
        info.size = len(manifest_bytes)
        archive.addfile(info, io.BytesIO(manifest_bytes))
    print(f"Packaged LVGL {lvgl_version} native inputs and ESP-IDF {idf_version} source subset: {args.output}")


if __name__ == "__main__":
    main()
