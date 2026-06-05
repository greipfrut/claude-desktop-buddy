# 4B audio-working firmware snapshot — 2026-06-05

Prebuilt, **confirmed-working** firmware for the Waveshare ESP32-S3-Touch-LCD-4B
buddy: from-source framework rebuild with working ES8311 audio, octal PSRAM, the
GDMA cache-disable crash fix, and all the audio bug fixes (settings sharing,
prompt-oscillation, I2S auto_clear, codec force-unmute).

`firmware-merged.bin` is a single image (bootloader + partitions + boot_app0 +
app) flashable at offset `0x0`. Use it to restore a known-good state without
rebuilding.

> Built with `ARDUINO_USB_CDC_ON_BOOT=0` (diagnostic — app `Serial` routes to
> UART0/COM4). Functionally identical for the buddy; only changes where serial
> logs appear. The repo's `platformio.ini` flips back to `=1` in a later commit.

## One-shot flash

```powershell
$py = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$et = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
& $py $et --chip esp32s3 --port COM4 --baud 921600 write_flash 0x0 firmware-merged.bin
```

(If COM4 is wrong, check Device Manager or `pio device list`.)

## Or rebuild + flash from source

```powershell
& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b -t upload --upload-port COM4
```
