# 4B audio + battery working firmware snapshot — 2026-06-05

Prebuilt, **confirmed-working** firmware for the Waveshare ESP32-S3-Touch-LCD-4B
buddy. Cumulative state at the end of the audio + battery work, before the
480×480 UI redesign (Phase 2):

- From-source framework rebuild: working **ES8311 audio**, octal PSRAM, GDMA
  cache-disable crash fix, all audio bug fixes.
- **AXP2101 battery** via XPowersLib: real voltage / charging / USB, voltage-curve
  percent, real PMIC power-off, real BLE `bat{}` status.
- UI is still the **240×320 layout centered** on the panel (480×480 native is the
  next phase).

`firmware-merged.bin` is a single image (bootloader + partitions + boot_app0 +
app) flashable at offset `0x0`.

> Built with `ARDUINO_USB_CDC_ON_BOOT=0` (diagnostic — app `Serial` → UART0/COM4).
> Phase 2's final task flips it back to `1`.
>
> NOTE: the board's PH2.0 battery connector polarity is **reversed** vs common
> LiPo packs — the cell's +/- must be swapped for the AXP2101 to detect it.

## One-shot flash

```powershell
$py = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$et = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
& $py $et --chip esp32s3 --port COM4 --baud 921600 write_flash 0x0 firmware-merged.bin
```

## Or rebuild + flash from source

```powershell
& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b -t upload --upload-port COM4
```
