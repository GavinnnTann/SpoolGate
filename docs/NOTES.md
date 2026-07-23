# Build environment

**Build via PowerShell, not Git Bash / any MSYS-based shell.** The pioarduino
platform's toolchain installer (`idf_tools.py`) checks the `MSYSTEM` environment
variable and hard-refuses to run under it:

```
ERROR: MSys/Mingw is not supported. Please follow the getting started guide of the
documentation to set up a supported environment
```

Git Bash sets `MSYSTEM=MINGW64` unconditionally. `cmd.exe` and PowerShell don't set
it at all, so either works — PowerShell is what's been verified. This only bites on
a first-time toolchain install (or after clearing `~/.platformio/packages`); once
the toolchain is cached, subsequent `pio run` calls succeed from any shell since
`idf_tools.py` isn't invoked again.

# Board and USB serial

The board is an **ESP32-S3-DevKitC-1** (`board = esp32-s3-devkitc-1`, 8 MB flash).
It replaced an ESP32-S3-Zero (`board = lolin_s3_mini`) that ran too hot to touch
under continuous AP+STA+NAT load. Same ESP32-S3 chip family, so the firmware is
unchanged.

Flash and monitor through the port labelled **"UART"**, not "USB". The UART port
routes through an onboard USB-to-UART bridge that maps to a stable COM port and
drives the DTR/RTS auto-reset circuit, so `pio run -t upload` enters and leaves the
bootloader automatically. Because of that, `Serial` is hardware UART0 and there is
**no** `ARDUINO_USB_CDC_ON_BOOT` build flag. (The S3-Zero had only a native
USB-Serial/JTAG port, which needed CDC-on-boot and re-enumerated to a different COM
port on every reset — none of that applies to the DevKitC's UART port. If you plug
into the DevKitC's native "USB" port instead, you get no serial output until
CDC-on-boot is added back.)

# Platform pin

See the comment block at the top of `platformio.ini` for the full story: the
official `platformio/espressif32` registry platform is still on arduino-esp32 core
2.0.17 regardless of its own wrapper version number (verified against registry
platform 7.0.1). `WiFi.AP.enableNAPT()` requires core 3.x, which today is only
available via the pioarduino fork's GitHub release zip. Don't swap the platform
line without re-confirming this — it fails silently at compile time and only
breaks at the link step.

# NAPT resource limits

NAPT needs a larger IP forwarding/NAT table and more lwIP TCP PCBs than the
default Arduino sdkconfig provides. On the Arduino framework these come from
Espressif's prebuilt `framework-arduinoespressif32-libs` IDF libraries, so they
can't be overridden via `build_flags` or `sdkconfig.defaults` — the framework
isn't built from IDF source in this setup. In practice this hasn't mattered for a
single-client (printer-only) NAT: the defaults are sufficient. If more clients are
ever added and NAT starts dropping connections under load, the fix is switching
`framework = espidf` and building lwIP from source with a custom `sdkconfig`,
which is a much bigger change — not attempted here since the project only needs
one NAT'd client.
