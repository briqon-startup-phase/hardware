# Status validasi — 1 November 2026 (Asia/Jakarta)

Paket saat ini **0.1.1**. Log pengguna dari 0.1.0 membuktikan boot, task startup,
output awal OFF, dan Wi-Fi mendapat IP; sensor gagal presence/reset dan koneksi
broker timeout. Perbaikan dan cara mencoba ulang ada di `FIXES-0.1.1.md`.

## Sudah dijalankan

| Pemeriksaan | Hasil |
| --- | --- |
| Compile/link ESP32-S3, ESP-IDF 5.4.0, GCC 14.2.0 | **PASS** |
| Build final sesudah penyesuaian API/queue MQTT | **PASS**, exit code 0 |
| Ukuran RAM statis menurut linker | 92.240 / 327.680 byte (28,1%) |
| Ukuran aplikasi menurut linker | 932.880 / 3.145.728 byte (29,7% partisi aplikasi) |
| Core + parser C aktual, cJSON 1.7.18 bawaan IDF, GCC host -Wall -Wextra -Werror | **PASS: 73 checks** |
| Aturan episode, dwell, hysteresis, fault 3 sampel, config validation, deadline | Termasuk 73 checks di atas |
| JSON/type/key/range/duplikat key, parser depth, embedded NUL, fragmen/timeout | Termasuk 73 checks di atas |
| Adapter GPIO sensor aktual + model bus host, CRC, data/fault/recovery/yield | **PASS: 20 sensor checks** |
| Probe TCP host (5 detik per endpoint, tanpa payload aplikasi) | Mosquitto 1883/8080 timeout; EMQX 1883/8083 reachable |
| Diagram dengan Wokwi CLI **0.29.1**, commit 0aa185144b92 | **PASS: 0 errors, 0 warnings, 2 info** |
| Manifest lengkap, target esp32s3, flash 8 MB, semua file image ada, ELF magic | **PASS** |
| Sintaks Python dan pembentukan envelope command oleh client | **PASS** |

Dua info linter menyebut board S3 dan DS18B20 sebagai undocumented parts pada
metadata registry. Tipe komponen dan seluruh nama pin dikenali; tidak ada error.
Nama 3V3 pada diagram telah disesuaikan menjadi 3V3.1/3V3.2 sesuai board S3.
Log build final, test host, dan lint tersedia di `evidence/`.

Versi komponen dengan hash hasil resolusi ada pada `dependencies.lock`.
MQTT dan cJSON menggunakan salinan yang dibundel ESP-IDF 5.4.0; DS18B20 0.3.0
dan onewire_bus 1.0.3 memakai managed components terpin untuk hardware profile.
Demo 0.1.1 menggunakan backend GPIO yang membaca sensor dan CRC aslinya.

## Belum dijalankan

**Firmware perbaikan 0.1.1 belum diuji ulang di Wokwi dan belum mencapai simulation MVP exit.**
Percobaan menjalankan Wokwi CLI dihentikan oleh peninjauan izin otomatis sebelum
eksekusi: simulasi dapat mengirim firmware/proyek nonpublik ke layanan Wokwi.
Persetujuan pengiriman artefak tersebut belum tersedia. Environment juga tidak
memiliki `WOKWI_CLI_TOKEN`; akses simulator perlu disediakan melalui Wokwi for
VS Code atau akun/token pengguna. Lint diagram tidak menjalankan firmware.

| Tes PRD | Status dan langkah berikutnya |
| --- | --- |
| T-01 | Build + diagram PASS; boot, output awal, dan genuine sensor smoke test belum dijalankan |
| T-02 / T-03 | Wi-Fi, SUBACK, MQTT, perubahan sensor dan latency perlu uji simulator |
| T-04 | Logika episode PASS pada host; verifikasi event MQTT di simulator belum |
| T-05 | Relay/contact/LED dan result round trip belum; gunakan client on/off |
| T-06 | Parser/deadline PASS pada host; dedup, conflict, session/retain secara end-to-end belum |
| T-07 | Validasi config PASS pada host; NVS persistence dan restart belum |
| T-08 / T-09 | Outage/recovery/loss policy belum; tombol GPIO7 tersedia untuk uji software disconnect |
| T-10 | Fault rule PASS pada host; genuine sensor + hook GPIO6 belum |
| T-11 | Fragment/malformed/batas parser PASS pada host; runtime overload/busy belum |
| T-12 | Boot OFF dan identitas sesi diimplementasikan, belum diuji reboot simulator |
| T-13 | Soak 60 menit, stack/heap runtime dan overload 10 command/detik belum |
| T-14 | Dashboard tidak termasuk proyek firmware ini; perlu integrasi platform |
| T-15 | Hardware/TLS/polaritas/ACL/kebijakan outlet belum diverifikasi pada board fisik |

Target 250 ms / 3 s / 5 s / 60 s di PRD tetap target yang perlu diukur, bukan hasil
yang disimpulkan dari kompilasi. Angka RAM di atas tidak mencakup seluruh alokasi
dinamis saat Wi-Fi/TLS berjalan.

## Menjalankan skenario sensor

Setelah firmware dibangun, gunakan akses Wokwi CLI Anda sendiri:

```powershell
wokwi-cli . --scenario tests/sensor-rules.yaml --timeout 90000 --serial-log-file sensor-rules.log
```

Skenario memeriksa boot GPIO5 HIGH/OFF, pembacaan sensor 4 → 7 °C, open/clear suhu,
fault/recovery sensor, dan output tetap OFF. Ini belum menguji MQTT atau klaim
tidak ada event duplikat di subscriber; gunakan client untuk verifikasi itu.

Untuk T-06, replay JSON command yang identik saat deadline masih valid, ubah ON
menjadi OFF dengan command_id yang sama, lalu uji salah device/boot/session dan
deadline. Untuk retained delivery, simpan command pada namespace disposable
**sebelum subscription**; bersihkan pesan retained setelah uji. Publish retained
ke subscriber aktif tidak selalu diteruskan dengan retain flag oleh MQTT 3.1.1.

Untuk tiap uji simulator, simpan boot/session, input, waktu pemulihan layanan,
output serial/MQTT, observasi indikator, dan hasil sebenarnya.
