"""Compile actual firmware core + parser with host GCC and ESP-IDF's cJSON."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cjson-dir", type=Path)
    args = parser.parse_args()
    idf = Path(os.environ.get("IDF_PATH", "__missing_idf__"))
    cjson = args.cjson_dir or idf / "components/json/cJSON"
    if not (cjson / "cJSON.c").is_file():
        raise SystemExit("Set IDF_PATH or use --cjson-dir pointing to cJSON 1.7.18 source.")
    build = ROOT / "tests/.host-build"
    build.mkdir(exist_ok=True)
    exe = build / ("test_core.exe" if os.name == "nt" else "test_core")
    subprocess.run([
        os.environ.get("CC", "gcc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
        f"-I{ROOT / 'components/briqon_core/include'}", f"-I{ROOT / 'main'}", f"-I{cjson}",
        str(ROOT / "components/briqon_core/core.c"), str(ROOT / "main/contract.c"),
        str(cjson / "cJSON.c"), str(ROOT / "tests/test_core.c"), "-lm", "-o", str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)
    sensor_exe = build / ("test_sensor.exe" if os.name == "nt" else "test_sensor")
    subprocess.run([
        os.environ.get("CC", "gcc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
        f"-I{ROOT / 'tests/stubs'}", f"-I{ROOT / 'components/briqon_core/include'}",
        f"-I{ROOT / 'components/sensor_adapter/include'}",
        str(ROOT / "components/briqon_core/ds18b20_frame.c"),
        str(ROOT / "components/sensor_adapter/sensor_adapter.c"),
        str(ROOT / "tests/test_sensor_gpio.c"), "-lm", "-o", str(sensor_exe)
    ], check=True)
    subprocess.run([str(sensor_exe)], check=True)


if __name__ == "__main__":
    main()
