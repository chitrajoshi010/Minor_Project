# LDSE ESP32 — Hardware, Wiring & Flashing Notes

Operational notes from bringing up the three-board LDSE bench (2026-08).
Read this before touching the hardware or re-flashing the boards.

---

## 1. Boards & roles

| Role   | Layer | Board            | Board id (PlatformIO)   |
| ------ | ----- | ---------------- | ----------------------- |
| Gateway | 0   | ESP32-WROOM-32   | `esp32dev`              |
| Relay  | 1    | ESP32-S3-N8R2    | `esp32-s3-devkitc-1`    |
| Node   | 2    | ESP32-S3-N16R8   | `esp32-s3-devkitc-1`    |

The gateway is the WROOM-32 substitute for the report's third S3 (mains
powered sink, no deep sleep / TinyML needed). If a third S3 appears, the
gateway env can be re-targeted.

---

## 2. Final SX1278 wiring (per board)

The radio is wired **per board** because the WROOM-32 devkit does not break
out GPIO16/17. NSS, SCK, DIO0 and RST are identical everywhere; only
MOSI/MISO/DIO1 differ.

### Gateway (ESP32-WROOM-32)

| SX1278 | ESP32 GPIO | Notes |
| ------ | ---------- | ----- |
| NSS    | 18         | SPI chip select |
| SCK    | 13         | SPI clock |
| MOSI   | 23         | SPI MOSI |
| MISO   | 19         | SPI MISO |
| DIO0   | 4          | RX/TX interrupt |
| DIO1   | 26         | required for CAD |
| RST    | 14         | reset |
| VCC    | 3.3 V      | |
| GND    | GND        | |

### Relay (S3-N8R2) & Node (S3-N16R8)

| SX1278 | ESP32 GPIO | Notes |
| ------ | ---------- | ----- |
| NSS    | 18         | SPI chip select |
| SCK    | 13         | SPI clock |
| MOSI   | 17         | SPI MOSI |
| MISO   | 21         | SPI MISO |
| DIO0   | 4          | RX/TX interrupt |
| DIO1   | 16         | required for CAD |
| RST    | 14         | reset |
| VCC    | 3.3 V      | |
| GND    | GND        | |

Pins are defined in `lib/ldse/LdseConfig.h` (`LDSE_PIN_*`). The gateway env
overrides `LDSE_PIN_MOSI/MISO/DIO1` via `build_flags` in `platformio.ini`.

### Why these pins (datasheet analysis)

Every radio signal is routed through the ESP32 GPIO matrix, so any free GPIO
can drive SPI. Constraints: pin must exist on the module, must not be wired
to internal flash/PSRAM, must be broken out on the board, and must match the
signal direction (inputs for MISO/DIO0/DIO1).

| Board | Unusable pins |
| ----- | ------------- |
| ESP32-WROOM-32 | GPIO6-11 internal flash; GPIO34-39 input-only; GPIO20/24/28-31 not on module; strapping 0/2/5/12/15; UART0 1/3 |
| ESP32-S3-WROOM-1 (N8R2 & N16R8) | GPIO22-25 not on module; GPIO26-32 flash/PSRAM bus; GPIO35-37 octal PSRAM (N16R8); USB 19/20; UART0 43/44; strapping 0/3/45/46; JTAG 39-42 |

- The free GPIOs present on **all three** boards are exactly
  `{4, 13, 14, 16, 17, 18, 21}` — the 7 pins the radio needs. A fully shared
  wiring would have to use 16/17.
- The WROOM-32 **devkit** additionally does not break out GPIO16/17
  (`U2RXD`/`U2TXD` labels), so the gateway uses 23/19/26 instead.
- On the S3, GPIO16 (`U1TXD`/`U0CTS`/`XTAL_32K_N`) and GPIO17 are free — the
  S3-WROOM-1 module does **not** populate the 32.768 kHz crystal (datasheet
  p.25: `32.768KHz(NC)`), so `XTAL_32K_N` is usable.

### Dangers of the original pinout (why this matters)

- Original `DIO0=26 / DIO1=33` is unusable on the S3 (26-32 = flash/PSRAM bus).
- `NSS=18` collides with the classic-ESP32 VSPI default SCK when SPI pins are
  not set explicitly.
- `MOSI=23 / MISO=19` do not exist (or are USB pins) on the S3 module.
- A shared `DIO1=6` lands on the WROOM-32 internal flash bus.

---

## 3. Known bugs fixed (code)

### 3.1 SPI pins were never set — RadioLib used per-chip defaults

RadioLib's `Module` calls `SPI.begin()` with **no arguments** on begin
(`ArduinoHal.cpp:99`). On Arduino-ESP32 that no-arg call returns early if the
bus is already started (`if(_spi) return;` in `SPI.cpp`), so the fix is to
call `SPI.begin(SCK, MISO, MOSI, NSS)` **first** in `LdseRadio::Begin()`.
Without this, the gateway used classic VSPI defaults (SCK=18) which collided
with NSS=18, and the S3 used FSPI defaults (SCK=12/MOSI=11) — the radio can
never be reached.

### 3.2 `#define` in the header silently overrode `-D` build flags

`LdseConfig.h` defined `LDSE_PIN_MOSI 17` **unconditionally**, so the gateway
build flag `-DLDSE_PIN_MOSI=23` was redefined back to 17. Symptom: the gateway
was driving SPI on the S3 pins (17/21) while the radio was wired to 23/19 →
`LdseRadio begin failed: -2` (RadioLib `CHIP_NOT_FOUND`, version register
read `0x00`).

Fix: guard the overridable pins with `#ifndef`:

```c
#ifndef LDSE_PIN_MOSI
#define LDSE_PIN_MOSI 17
#endif
```

The gateway env then correctly compiled with MOSI=23/MISO=19/DIO1=26 and the
version register read `0x12` (genuine SX1278).

### 3.3 Dangling `Module` pointer (crash after successful init)

`LdseRadio::Begin()` created the RadioLib `Module` on the **stack** and stored
its address in the heap `SX1278`:

```cpp
Module mod(LDSE_PIN_NSS, ...);   // stack object
m_radio = new SX1278(&mod);      // stores pointer to freed stack after return
```

RadioLib keeps `mod` for its lifetime. Once `Begin()` returned, `m_radio`
dereferenced freed memory → `Guru Meditation Error: LoadProhibited` at
`EXCVADDR 0x00000000`. This only appeared once the chip actually initialized
(previous builds always failed at `-2` first). Fixed by heap-allocating the
Module:

```cpp
m_mod = new Module(LDSE_PIN_NSS, LDSE_PIN_DIO0, LDSE_PIN_RST, LDSE_PIN_DIO1);
m_radio = new SX1278(m_mod);   // + delete in destructor
```

---

## 4. Flashing from WSL2 (usbipd workflow)

WSL2 cannot see host USB serial ports. Use
[usbipd-win](https://github.com/dorssel/usbipd-win) to attach each board.

### One-time setup (Windows, admin PowerShell)

```powershell
winget install --interactive --exact dorssel.usbipd-win
usbipd list            # find the BUSID of the board's USB-serial (e.g. 2-2)
usbipd bind --busid <BUSID>        # admin; one-time per unique device
```

### Every time you plug a board in (admin PowerShell)

```powershell
usbipd attach --wsl --busid <BUSID>    # admin; needs UAC approval
```

The device then appears in WSL as `/dev/ttyACM0` (or `/dev/ttyUSB0`).

### Give your user access to the port (WSL)

```sh
sudo usermod -aG dialout chitra
```

then reopen the terminal (or `newgrp dialout`).

### Build & flash (WSL)

```sh
cd ~/forest_lora_ns3/ldse-esp32
pio run -e gateway && pio run -e gateway -t upload --upload-port /dev/ttyACM0
pio run -e relay   && pio run -e relay   -t upload --upload-port /dev/ttyACM0
pio run -e node    && pio run -e node    -t upload --upload-port /dev/ttyACM0
```

- PlatformIO 6.x removed `pio upload`; use `pio run -t upload`.
- Always pass `--upload-port` — auto-detection picks the bogus `/dev/ttyS0`.
- Flash one board at a time (unplug the others or the port differs).

### S3 boards: hard reset after upload

After an S3 upload the chip can sit in download mode (`boot:0x0
(DOWNLOAD(USB/UART0))`, "waiting for download"). Hard-reset it to run the app:

```sh
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 --port /dev/ttyACM0 --after hard_reset read_mac
```

### Reading serial from a script (S3 gotcha)

Opening the port with default DTR/RTS asserted puts the S3 into download mode.
Deassert both before reading:

```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=2)
s.dtr = False
s.rts = False
time.sleep(4)
print(s.read(4000).decode('utf-8', 'replace'))
s.close()
```

`pio device monitor` fails under a non-TTY shell (`termios ioctl` error) — run
it from an interactive terminal instead.

---

## 5. The node (S3-N16R8) flash quirk

- The module's flash is a **Winbond W25Q128, quad 16 MB** (`esptool flash_id`:
  manufacturer 0x68, device 0x4018) — **not** octal flash.
- Setting `board_build.flash_size = 16MB` (+ `default_16MB.csv`) makes the
  second-stage bootloader panic inside its SHA-256 image check
  (`bootloader_sha.c:39` / `bootloader_utility.c:500`, crash PC
  `0x403cdb0a` in DIO / `0x403cd9ce` in DOUT) and loop on `RTC_SW_SYS_RST`.
- Workaround: the node env uses the **8 MB layout** (flash_size 8MB, default
  partitions). The app is only ~320 KB, so 8 MB is plenty.
- PSRAM on this module does not respond as quad (`PSRAM ID read error:
  0x00ffffff`). The node firmware does not use PSRAM yet, so `-DBOARD_HAS_PSRAM`
  was removed from the node env to keep the boot log clean.

---

## 6. Bench test (after flashing)

1. Power the boards in order: **gateway → relay → node**.
2. The node only starts transmitting after it hears `LAYER_INIT` + `SYNC`
   (epoch = 10 s), so wait 1-2 epochs.
3. Gateway serial (115200) prints `DATA,epoch,origin,src,hops,seq,rssi,
   origin_layer,sensor,count` CSV rows — expect `hops ≈ 2`
   (node → relay → gateway).
4. To observe congestion control, burst node traffic and watch the relay
   report `sf=10 bypass=1`.

---

## 7. Verified working state (2026-08)

| Board | Boot log evidence |
| ----- | ----------------- |
| Gateway | `[LDSE] Radio on Puc ready`, `[GW] LAYER_INIT layer=1 sent`, `[GW] SYNC sent @ ... us` |
| Relay   | `[LDSE] Relay`, `[LDSE] Relay listening on Puc` |
| Node    | `[LDSE] End device (node)`, `[LDSE] Node listening on Prc1`, `[NODE] No parent yet: waiting for LAYER_INIT/SYNC` |
