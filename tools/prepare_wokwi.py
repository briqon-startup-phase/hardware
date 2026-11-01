"""Copy a PlatformIO ESP-IDF build into the standard Wokwi build/ directory."""
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = ROOT / ".pio/build/wokwi"
    target = ROOT / "build"
    manifest_path = source / "flasher_args.json"
    if not manifest_path.is_file():
        raise SystemExit("Jalankan `pio run -e wokwi` terlebih dahulu; flasher_args.json belum tersedia.")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    target.mkdir(exist_ok=True)
    flash_files = {}
    renamed = {
        "bootloader/bootloader.bin": "bootloader.bin",
        "partition_table/partition-table.bin": "partitions.bin",
        "briqon_esp.bin": "firmware.bin",
    }
    # Use a self-contained manifest: no paths into local toolchain or .pio folders.
    for offset, name in manifest["flash_files"].items():
        normalized = name.replace("\\", "/")
        original = Path(renamed.get(normalized, normalized))
        if not original.is_absolute():
            original = source / original
        if not original.is_file():
            raise SystemExit(f"Missing image: {original}")
        relative = f"images/{original.name}"
        destination = target / relative
        destination.parent.mkdir(exist_ok=True)
        shutil.copy2(original, destination)
        flash_files[offset] = relative
    manifest["flash_files"] = flash_files
    # Named entries are redundant for the Wokwi loader; update their paths too.
    for entry in manifest.values():
        if isinstance(entry, dict) and "offset" in entry and "file" in entry:
            entry["file"] = flash_files.get(str(entry["offset"]), entry["file"])
    (target / "flasher_args.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    shutil.copy2(source / "firmware.elf", target / "briqon_esp.elf")
    shutil.copy2(ROOT / "sdkconfig.wokwi", target / "sdkconfig.used")
    print(f"Wokwi firmware prepared: {target}")


if __name__ == "__main__":
    main()
