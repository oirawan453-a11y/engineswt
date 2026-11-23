# SwitchCE

Cheat engine sederhana untuk Nintendo Switch dengan Atmosphère. Cara kerjanya sama seperti Cheat Engine di PC: cari nilai, ubah nilainya di game, saring hasil, lalu edit atau kunci nilainya.

## Syarat

- Switch dengan CFW Atmosphère
- devkitPro dengan paket `switch-dev` untuk compile (atau pakai GitHub Actions, lihat bawah)

## Compile

```
export DEVKITPRO=/opt/devkitpro
make
```

Hasilnya `SwitchCE.nro`. Salin ke `sdmc:/switch/SwitchCE/SwitchCE.nro`.

**Tanpa install devkitPro:** upload folder ini ke repo GitHub. Workflow di `.github/workflows/build.yml` akan compile otomatis. File `.nro` bisa diambil di tab **Actions**, lalu buka run terakhir dan lihat bagian **Artifacts**.

## Cara pakai

1. Buka game, lalu tekan HOME.
2. Buka Homebrew Menu. Sebaiknya lewat title override (tahan R saat membuka game) supaya memorinya lebih besar dan hasil scan bisa lebih banyak.
3. Jalankan SwitchCE. Game akan terdeteksi otomatis.
4. Pilih **Tipe data**. Kebanyakan angka di game (uang, item) pakai `u32`, sedangkan darah atau stamina sering pakai `float`.
5. **Scan baru**: masukkan nilai yang terlihat sekarang, misalnya 500.
6. Kembali ke game dan ubah nilainya, misalnya belanja sampai uang tinggal 350.
7. Buka SwitchCE lagi, pilih **Scan lanjutan**, lalu **Sama dengan nilai...** dan masukkan 350.
8. Ulangi langkah 6 dan 7 sampai hasilnya tinggal sedikit.
9. Di **Lihat hasil**:
   - `A` untuk mengedit nilai
   - `Y` untuk freeze (nilai dikunci)
   - `X` untuk menyimpan sebagai cheat

Kalau nilainya tidak terlihat di layar, misalnya bar darah, pakai mode **Naik**, **Turun**, **Berubah**, atau **Tidak berubah**.

## Cheat yang disimpan

Cheat ditulis ke `sdmc:/atmosphere/contents/<TitleID>/cheats/<BuildID>.txt` dalam format Atmosphère dan langsung aktif. Cheat ini juga bisa dinyalakan atau dimatikan lewat EdiZon atau Breeze.

Catatan: alamat di region `heap` bisa berubah setelah game direstart. Kalau cheat tidak bekerja lagi setelah restart, scan ulang. Supaya alamatnya stabil, perlu pencarian pointer. Fitur itu belum ada di versi ini.

## Lisensi

`source/dmntcht.c`, `include/dmntcht.h`, dan `include/service_guard.h` berasal dari Atmosphère-NX/EdiZon-SE (GPLv2). Karena itu proyek ini juga GPLv2.
