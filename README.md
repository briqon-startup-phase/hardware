# BRIQON ESP32-S3 untuk Wokwi

**Versi 0.1.1** memperbaiki laporan reset bus sensor dan retry Wi-Fi dari versi
0.1.0, serta mengganti broker demo yang timeout. Firmware baru sudah dikompilasi;
pengujian ulang di Wokwi masih diperlukan. Lihat `FIXES-0.1.1.md`.

Implementasi PRD_ESP.md: ESP-IDF (C), ESP32-S3 dual core, DS18B20 asli simulator,
Wi-Fi, MQTT dua arah, relay, aturan notifikasi, dan pengaturan di NVS. Tidak ada AI.

**Jalur penggunaan: Wokwi for VS Code dengan firmware ESP-IDF.** Ini bukan proyek
Arduino yang ditempel ke `sketch.ino`. `libraries.txt` tidak diperlukan karena
dependensi dikelola ESP-IDF. Folder ini memuat `diagram.json` dan `wokwi.toml`.

## 1. Menjalankan simulasi

1. Ekstrak ZIP dan buka folder **briqon-wokwi** di VS Code. Folder yang dibuka
   harus langsung berisi `wokwi.toml`.
2. Instal/aktifkan ekstensi **Wokwi Simulator** sesuai akses akun Wokwi Anda.
3. Paket distribusi menyertakan `build/flasher_args.json`, image flash, dan ELF.
   Jika folder `build` belum ada atau kode diubah, lakukan build pada bagian 4.
4. Tekan **F1 → Wokwi: Start Simulator**.
5. Serial Monitor 115200 menampilkan `device=...`, `topics=...`, sampel suhu,
   kemudian `ONLINE ... session=...` setelah Wi-Fi, MQTT, dan SUBACK berhasil.
6. Klik DS18B20, ubah suhu dari 4 menjadi 7 °C. Nilai log dan MQTT harus mengikuti.

Board pada diagram adalah `board-esp32-s3-devkitc-1`, cocok dengan target
`esp32s3`. Panduan loader ESP-IDF resmi:
[Wokwi ESP-IDF](https://docs.wokwi.com/vscode/esp-idf).

Profil demo memakai `Wokwi-GUEST` tanpa password dan
`mqtt://broker.emqx.io:1883`. Ini mode plaintext untuk data simulasi yang
disposable. [Broker publik EMQX](https://www.emqx.com/en/mqtt/public-mqtt5-broker) dapat diakses orang lain;
gunakan namespace/device ID yang berbeda untuk simulator yang berjalan bersamaan.
Tidak ada kredensial outlet yang disertakan.

## 2. Mengamati MQTT dan mengirim ON/OFF

Jalankan terminal kedua dari folder proyek. Contoh Windows PowerShell:

```powershell
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r tools\requirements.txt
```

Salin ID dari `device=...` di Serial Monitor. Ganti `ID_DARI_SERIAL` di bawah ini.
ID default diprovisikan sekali ke NVS menggunakan suffix acak dan MAC. Reboot
mempertahankan ID; simulasi dengan flash baru dapat memiliki ID baru. Anda dapat
menetapkan ID sendiri lewat `menuconfig → BRIQON → Device ID`.

```powershell
# Menampilkan telemetri, state, heartbeat, notifikasi, dan hasil perintah (30 detik)
.\.venv\Scripts\python.exe tools\mqtt_client.py --device ID_DARI_SERIAL watch

# Nyalakan LED melalui relay, lalu matikan
.\.venv\Scripts\python.exe tools\mqtt_client.py --device ID_DARI_SERIAL on
.\.venv\Scripts\python.exe tools\mqtt_client.py --device ID_DARI_SERIAL off

# Minta status
.\.venv\Scripts\python.exe tools\mqtt_client.py --device ID_DARI_SERIAL status

# Ubah interval publikasi menjadi 3 detik (persisten)
.\.venv\Scripts\python.exe tools\mqtt_client.py --device ID_DARI_SERIAL config --values '{"telemetry_interval_ms":3000}'
```

Client menunggu state/heartbeat **live**, mengambil boot/session/uptime terkini,
lalu mengirim command dengan deadline 15 detik dan ID unik. Statusnya `accepted`
kemudian `applied`, `rejected`, atau `failed`. Timeout berarti hasil belum diketahui;
periksa state. Gunakan `--seconds 120` untuk mengamati lebih lama.

MQTT Explorer / mosquitto_sub juga dapat berlangganan
`briqon/v1/demo/ID_DARI_SERIAL/#`. Jangan kirim hanya teks `ON`; perangkat menerima
envelope JSON lengkap seperti PRD:

```json
{
  "schema_version": 1,
  "device_id": "ID_DARI_SERIAL",
  "target_boot_id": "SALIN_DARI_STATE_LIVE",
  "target_session_id": "SALIN_DARI_STATE_LIVE",
  "command_id": "cmd-uji-001",
  "expires_at_uptime_ms": 45000,
  "action": "set_output",
  "params": {"channel": "relay-1", "state": "ON"}
}
```

Ganti deadline dengan uptime perangkat saat ini ditambah 15000 ms. Batas penerimaan
adalah **lebih besar** dari uptime saat diterima, maksimal 30000 ms ke depan.
Publish ke `/commands`, QoS 1, **retain OFF**.

| Topic suffix | Fungsi | QoS / retain |
| --- | --- | --- |
| `/telemetry` | Suhu, quality, sample sequence/waktu/umur, output | 0 / tidak |
| `/heartbeat` | Progress, heap, stack, antrean, counter | 0 / tidak |
| `/availability` | Online dan Last Will offline | 1 / ya |
| `/state` | Snapshot suhu, output, config, kondisi aktif | 1 / ya |
| `/notifications` | Transisi episode suhu/sensor | 1 / tidak |
| `/commands` | Input perintah | 1 / tidak |
| `/command-results` | Hasil berkorelasi command_id | 1 / tidak |

## 3. Rangkaian dan demo

| Bagian | Sambungan |
| --- | --- |
| DS18B20 | DQ → GPIO4; VCC → 3V3; GND bersama; pull-up 4,7 kΩ |
| Relay | IN → GPIO5; VCC → 5V; GND bersama; pull-up input 10 kΩ |
| Kontak relay | COM → 3V3; NO → resistor 330 Ω → anoda LED; katoda → GND; NC kosong |
| Tombol merah | GPIO6 → GND saat ditekan; pull-up internal; fault sensor |
| Tombol biru | GPIO7 → GND saat ditekan; pull-up internal; Wi-Fi disconnect |

Profil diagram menggunakan relay `transistor=npn` dengan **GPIO LOW untuk kontak
COM–NO**, mengikuti bagian Operation pada
[referensi relay Wokwi](https://docs.wokwi.com/parts/wokwi-relay-module).
`CONFIG_BRIQON_RELAY_ACTIVE_LOW=y` harus sesuai diagram. LED OFF saat boot dan ON
sesudah perintah ON; ini masih perlu dikonfirmasi pada simulator sebagai smoke test.
Laporan output selalu `gpio_reported`, karena tidak ada sensor feedback kontak.

Urutan demo:

1. Suhu 4 → 7 °C: lihat nilai MQTT yang sesuai.
2. Set 10 °C lebih dari 5 detik: satu `temperature_high/opened`.
   Pertahankan tinggi: tidak ada pengulangan opened. Set 5 °C lebih dari 5 detik:
   satu `cleared`. Suhu di antara ambang mempertahankan episode aktif.
3. Kirim ON dan OFF: amati LED dan terminal `applied` dengan command ID sama.
4. Tahan tombol merah: tiga sampel gagal membuka `sensor_fault`; lepaskan:
   tiga sampel valid menutupnya. Nilai gagal menjadi JSON null, bukan angka buatan.
5. Nyalakan output, tahan tombol biru lebih dari 5 detik: relay kembali OFF dan
   nomor sampel lokal terus maju. Lepaskan: koneksi pulih dengan session baru,
   tanpa mengulang ON. Tombol ini menguji software disconnect, bukan RF sungguhan.
6. Ubah konfigurasi, tekan reset board: konfigurasi di NVS bertahan, output tetap
   boot OFF, boot/session berganti. Memulai flash simulasi baru berbeda dari reboot.

Ambang 8/6 °C dan dwell 5 detik hanya nilai demo. Aturan suhu tidak mengubah relay.

## 4. Build ulang

Versi terpin: **PlatformIO Core 6.1.18**, **espressif32 6.10.0**,
**ESP-IDF 5.4.0**, **DS18B20 0.3.0**, **onewire_bus 1.0.3**.
`dependencies.lock` mencatat resolusi komponen yang sesungguhnya.

### Pilihan A — PlatformIO (tetap menggunakan ESP-IDF, bukan Arduino)

Instal PlatformIO extension atau gunakan Python 3.11:

```powershell
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install platformio==6.1.18
.\.venv\Scripts\python.exe -m platformio run -e wokwi
.\.venv\Scripts\python.exe tools\prepare_wokwi.py
```

Script terakhir menyalin bootloader, tabel partisi, aplikasi, manifest, dan ELF
ke `build/`, dengan path relatif. Jalankan lagi setiap selesai mengubah kode.
Toolchain diunduh pada build pertama dan memerlukan internet.

### Pilihan B — ESP-IDF 5.4.0 / tag v5.4

Dari terminal ESP-IDF yang telah diinisialisasi, pada checkout baru:

```powershell
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wokwi" set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wokwi" build
```

Build ini menghasilkan file yang dirujuk `wokwi.toml` secara langsung. Jangan
campur artefak build PlatformIO dan idf.py di satu folder `build` yang sedang aktif.

### Mengubah broker / ID / profil

Dengan ESP-IDF gunakan `idf.py menuconfig → BRIQON`. Untuk PlatformIO:
`pio run -e wokwi -t menuconfig`. Perubahan harus dibangun ulang.
Untuk TLS demo dengan Mosquitto pilih URI `mqtts://test.mosquitto.org:8886` dan
tambahkan `--broker test.mosquitto.org --tls --port 8886` pada client. Endpoint ini
alternatif terpisah dari EMQX; keterjangkauan dan validasi sertifikat tetap perlu diuji.

Profil hardware dibuat terpisah:

```powershell
idf.py -B build-hardware -D SDKCONFIG=sdkconfig.hardware -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.hardware" set-target esp32s3
idf.py -B build-hardware -D SDKCONFIG=sdkconfig.hardware menuconfig
idf.py -B build-hardware -D SDKCONFIG=sdkconfig.hardware build
```

Isi AP, `mqtts://` broker, username/password, environment, device ID, dan periksa
polaritas hardware. Hardware profile menunggu waktu SNTP untuk TLS, menggunakan
CA bundle/hostname verification, menolak plaintext/kredensial kosong, menonaktifkan
fault hooks, dan default remote control OFF. Broker harus memakai sertifikat
dari CA dalam bundle; CA privat perlu ditambahkan sebelum penggunaan.
Simpan konfigurasi rahasia lokal, jangan commit. Profil ini belum lolos uji board
fisik, ACL, maupun kebijakan peralatan outlet (T-15).

## 5. Struktur kode dan batas operasional

```text
main/app_main.c                  task, Wi-Fi/MQTT, NVS, output, publisher
main/contract.c                  parser ketat dan perakitan fragmen MQTT
components/briqon_core/          config, deadline, aturan episode, decode scratchpad dan CRC
components/sensor_adapter/       1-Wire GPIO demo; driver Espressif/RMT hardware; fault hook demo
diagram.json, wokwi.toml         fixture dan loader
tools/mqtt_client.py             subscriber dan pengirim perintah fresh
tests/                          pengujian host dan skenario Wokwi
```

Akuisisi dipin core 1 (priority 4), kontrol core 1 (priority 6), publisher dan
supervisor tanpa affinity (priority 3). Profil Wokwi menggunakan 1-Wire GPIO
untuk membaca sensor asli dan memvalidasi CRC scratchpad. Profil hardware tetap
memakai driver managed Espressif/RMT. Konversi 800 ms pada demo menunggu dengan
yield; bagian critical hanya slot bus di bawah 1 ms.
Hanya task kontrol menulis relay setelah inisialisasi boot. Callback MQTT hanya
merakit/copy pesan dan memperbarui lifecycle. Setiap task mendaftarkan dan
mereset watchdog-nya sendiri. Log core ID tidak membuktikan performa paralel hardware.

Antrean command 8, overflow-rejection 4; event 64 total (32 rejection, 32
notification); cache 64 command dengan pending accepted/terminal results. Payload
maksimal 2048 byte, kedalaman JSON maksimal 8, timeout fragmen 2 detik.
Validasi key/type ketat; identifier ASCII terbatas, escape Unicode ditolak.
Duplikat dinormalisasi menjadi struktur command, sehingga urutan key/whitespace
tidak memicu eksekusi ulang. ID sama dengan isi berbeda ditolak. Cache hanya
mengevict hasil yang telah masuk outbox, dan perintah memakai sesi MQTT clean.

Koneksi putus tidak menghentikan sampling. Hanya snapshot sampel terbaru disimpan;
tidak ada backfill. Publisher membatasi outbox 16 KiB dan mendahulukan hasil
command. Queue/cache/outbox tidak menjanjikan delivery durable melewati reboot.
Jika command queue dan overflow queue sekaligus penuh, atau rejection queue penuh,
counter `dropped_results` naik; paket yang belum diterima aplikasi tidak diberi
status accepted. Pesan terlalu besar/tidak dapat dirakit hanya dihitung karena
command ID belum dapat dipercaya. Result yang sudah accepted tetap dicache sampai
bisa masuk outbox. State membantu rekonsiliasi ketika notifikasi drop.

`observed_at` adalah waktu pembuatan pesan; `sample_observed_at`,
`sample_uptime_ms`, `sample_seq`, dan `sample_age_ms` menunjukkan waktu/identitas
**akuisisi**. Saat SNTP belum sinkron, waktu UTC null dengan `time_quality`
unsynchronized. Penerbitan ulang tidak mengubah waktu akuisisi. Dedup titik grafik
dengan device/boot/sample_seq. Telemetry yang lebih cepat dari sampling dapat
membawa sampel yang sama.

Retained command **delivery** ditolak. Pada MQTT 3.1.1, broker umumnya menghapus
retain flag ketika meneruskan publish ke subscriber yang sudah aktif; perangkat
tidak bisa mendeteksi niat retained dari delivery tersebut. Pengirim wajib retain
OFF; uji retained dilakukan dengan pesan yang sudah tersimpan sebelum subscription.
Backend tetap bertanggung jawab atas autentikasi pengguna, ACL, freshness,
penyimpanan data, dashboard, dan pengiriman notifikasi eksternal.

## 6. Validasi

```powershell
# Setelah environment ESP-IDF aktif (tersedia GCC host di PATH)
python tests\run_host_tests.py
# Atau tentukan folder source cJSON secara eksplisit
python tests\run_host_tests.py --cjson-dir PATH_KE_CJSON
```

Lihat `VALIDATION.md` untuk bukti yang sudah dijalankan dan status T-01–T-15.
Skenario `tests/sensor-rules.yaml` dapat dijalankan dengan Wokwi CLI dan akses/token
Anda sendiri; [petunjuk resmi](https://docs.wokwi.com/wokwi-ci/automation-scenarios).
Pengujian host dan build tidak menggantikan Wokwi, soak 60 menit, integrasi dashboard,
atau validasi hardware.
