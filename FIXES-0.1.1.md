# Perbaikan 0.1.1

## Memakai paket ini di VS Code

1. Hentikan simulator yang sedang berjalan.
2. Ekstrak `briqon-wokwi-v0.1.1.zip` ke **folder baru**, lalu buka folder
   `briqon-wokwi` hasil ekstraksi lewat File → Open Folder. Ini mencegah firmware
   0.1.0 dari folder lama tetap dipakai.
3. Tekan F1 → Wokwi: Start Simulator. Firmware sudah dibangun; tidak perlu build
   ulang untuk mencoba paket ini.
4. Pastikan log boot menampilkan **`BRIQON firmware=0.1.1`** dan log sensor
   **`backend=gpio-onewire gpio=4`**.
5. Indikator keberhasilan uji berikutnya adalah `temperature=4.000 C`, diikuti
   `MQTT connected`, `SUBACK`, dan `ONLINE`. Ini target verifikasi, bukan log
   hasil Wokwi yang sudah dikumpulkan.

## Diagnosis dari log pengguna

- `Wi-Fi IP ready` dan IP 10.13.37.2: asosiasi dan DHCP berhasil.
- `reset bus error` / `ESP_ERR_NOT_FOUND`: driver 1-Wire/RMT tidak menemukan
  presence pulse sensor. Log saja belum membedakan wiring yang berubah dari
  masalah timing/edge capture pada simulator.
- `select() timeout` sebelum MQTT connected: pembukaan koneksi TCP ke broker gagal.
  Nama komponen `esp-tls` pada log juga dipakai jalur transport TCP; ini tidak
  berarti konfigurasi demo memakai TLS atau ada masalah sertifikat.
- `sta is connecting` / `sta is connected, disconnect before connecting`: firmware
  sebelumnya memanggil connect lagi saat koneksi/DHCP masih berlangsung.
- RF calibration fallback di awal tidak mencegah perangkat mendapat IP pada log ini.

## Perubahan

**Sensor demo:** mengganti backend RMT dengan GPIO open-drain untuk fixture satu
DS18B20. Kode mengirim reset/presence, Skip ROM, Convert T, lalu membaca sembilan
byte scratchpad termasuk CRC. Angka tidak dibuat oleh firmware. Data kosong,
CRC gagal, timeout, atau di luar jangkauan tetap invalid/null. Waktu konversi
800 ms memakai vTaskDelay; kontrol tetap bisa dijadwalkan. Hardware profile
tetap memakai komponen Espressif/RMT terpin. Ini penyesuaian adapter simulator
terhadap pilihan driver awal PRD dan masih membutuhkan genuine Wokwi smoke test.

**MQTT:** broker default firmware dan client menjadi `broker.emqx.io:1883`.
Probe TCP dari komputer build: test.mosquitto.org port 1883 dan 8080 timeout
dengan batas 5 detik; broker.emqx.io port 1883 dan 8083 dapat dijangkau.
Hasil ini berlaku untuk jalur host pada waktu pemeriksaan, bukan jaminan
konektivitas dari Wokwi. Tidak ada kode/firmware/data sensor yang dikirim saat
probe; hanya koneksi TCP. MQTT transport timeout diperpanjang menjadi 10 detik
dan anggaran satu attempt menjadi 20 detik.

**Wi-Fi:** event asosiasi dipisah dari IP readiness. Supervisor menunggu satu
attempt/DHCP selesai atau timeout 15 detik sebelum mengulang connect.

## Validasi dan tindak lanjut

- Build ESP32-S3 PASS; 73 pemeriksaan core/parser PASS.
- Tambahan 20 pemeriksaan sensor PASS dengan adapter GPIO aktual dan model bus
  host: nilai positif/negatif, batas suhu, CRC, no presence, bus short, fault hook,
  conversion timeout, recovery, dan conversion wait di luar critical section.
- Model host menguji protokol firmware dan tidak menggantikan simulator Wokwi.
- Firmware 0.1.1 belum diuji di Wokwi. Ulangi dengan diagram paket ini tanpa
  perubahan, lalu periksa dua indikator pada langkah 5.

Jika broker masih timeout, dari terminal PowerShell komputer yang menjalankan
VS Code gunakan:

```powershell
Test-NetConnection broker.emqx.io -Port 1883
```

`TcpTestSucceeded: False` menunjuk kendala broker/jalur jaringan komputer yang
perlu diperiksa (VPN/firewall/jaringan), bukan kesimpulan bahwa Wi-Fi simulasi
gagal. `True` membuktikan TCP host saja; jika simulator tetap gagal, lihat Output
Wokwi/gateway dan log firmware. Wokwi VS Code mendokumentasikan gateway bundled:
[konfigurasi jaringan Wokwi](https://docs.wokwi.com/vscode/project-config#iot-gateway-esp32-wifi).

Client yang disertakan sudah memakai EMQX. Jika memakai MQTT Explorer, ubah host
ke **broker.emqx.io**, port **1883**, agar subscriber memakai broker yang sama.

Referensi protokol sensor:
[datasheet DS18B20](https://www.analog.com/media/en/technical-documentation/data-sheets/ds18b20.pdf).
Endpoint publik:
[EMQX](https://www.emqx.com/en/mqtt/public-mqtt5-broker).
