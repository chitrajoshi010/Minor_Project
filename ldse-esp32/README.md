# LDSE ESP32 Implementation

Practical ESP32 firmware implementation of the **LDSE** (Layering-based Data
Synchronization and Energy-efficient) protocol from the research project's
ns-3 simulation.  Three roles: **gateway**, **relay**, and **end node**.

This code is intentionally kept **outside** `ns-3.48/` and mirrors the
simulation model 1:1 so the same design can be demonstrated on real hardware
(e.g. ESP32 DevKit + SX1278 LoRa modules).

---

## Board roles

The three roles map to three available boards as follows (see the mid-term
report: 2 forest sensor nodes + 1 gateway):

| Role   | Layer | Board            | Reason |
| ------ | ----- | ---------------- | ------ |
| Gateway | 0   | ESP32-WROOM-32   | Mains-powered sink (LoRa RX + Wi-Fi); no TinyML / deep-sleep needed. |
| Relay  | 1    | ESP32-S3-N8R2    | Forest node + forwarder; S3 as specified in the report. |
| Node   | 2    | ESP32-S3-N16R8   | Deepest sensor node; most flash/PSRAM for the future acoustic payload. |

> **Node flash quirk:** the S3-N16R8 module ships a 16 MB Winbond W25Q128, but
> setting `flash_size = 16MB` (+ `default_16MB.csv`) crashes the second-stage
> bootloader (panic in `bootloader_sha`, `RTC_SW_SYS_RST` loop). The node env
> therefore uses the 8 MB layout — more than enough for the ~320 KB app.
> PSRAM is not enabled on the node yet; enabling it prints a harmless
> `PSRAM ID read error` on this module.

> The report's design assumes three ESP32-S3 boards; the WROOM-32 substitutes
> for the gateway role, which is the least S3-dependent (mains powered, no
> on-node ML, no ~7 uA sleep requirement). If a third S3 becomes available,
> the gateway env can simply be built for it.

---

## Project layout

```
ldse-esp32/
├── platformio.ini          # 3 environments: gateway, relay, node
├── src/main.cpp            # role dispatcher (selects firmware/ via -DLDSE_ROLE)
├── lib/ldse/               # shared protocol library (all roles)
│   ├── LdseConfig.h        # pinout, radio, channels, epoch, timing
│   ├── LdsePacket.h        # compact frame format (magic/type/ids/hops/ts)
│   ├── LdseRadio.{h,cc}    # RadioLib SX1278 wrapper (RX/TX/CAD/sleep)
│   ├── LdseSync.{h,cc}     # FTSP-style clock sync (45 ppm drift model)
│   ├── LdseRouting.{h,cc}  # IRE routing table (parent selection)
│   ├── LdseForwarder.{h,cc}# queue, congestion SF10/bypass, CAD+backoff
│   ├── LdseEpoch.h         # SYNC / DATA / SLEEP window scheduler
│   └── LdseEnergy.{h,cc}   # software energy model (active vs sleep)
└── firmware/
    ├── gateway/main.cpp    # layer 0: sink, sync source, data logger
    ├── relay/main.cpp      # layer 1: forwarder + congestion control
    └── node/main.cpp       # layer 2: sensor uplink + ACK retry
```

> `src/main.cpp` is a thin include-dispatcher: it pulls in
> `firmware/{gateway,relay,node}/main.cpp` based on the `-DLDSE_ROLE` build
> flag (PlatformIO 6.x no longer supports per-env `src_dir`).

## Build & flash

> Full operational details (pin analysis, WSL2/usbipd flashing, S3 download-mode
> gotchas, the node's 16 MB flash quirk, and the code bugs fixed) are in
> [`docs/HARDWARE_AND_FLASHING.md`](docs/HARDWARE_AND_FLASHING.md).

Requires [PlatformIO](https://platformio.org/).  Each environment is already
configured for its board (see `platformio.ini`): `gateway` → ESP32-WROOM-32,
`relay` → ESP32-S3-N8R2, `node` → ESP32-S3-N16R8.

```sh
pio run -e gateway && pio run -e gateway -t upload
pio run -e relay   && pio run -e relay   -t upload
pio run -e node    && pio run -e node    -t upload
```

> PlatformIO Core 6.x removed the standalone `pio upload` alias; uploads use
> `pio run -t upload`.

> **WSL2 USB note:** WSL2 cannot see host USB serial ports. Install
> [usbipd-win](https://github.com/dorssel/usbipd-win), then (admin PowerShell):
> `usbipd bind --busid <BUSID>` once, and `usbipd attach --wsl --busid <BUSID>`
> each time the board is plugged in. The port then appears in WSL (e.g.
> `/dev/ttyACM0`). Because PlatformIO auto-detection may pick the bogus
> `/dev/ttyS0`, pass the port explicitly:
> `pio run -e gateway -t upload --upload-port /dev/ttyACM0`.
> If you get "Permission denied", add yourself to the `dialout` group:
> `sudo usermod -aG dialout $USER`, then reopen the terminal.

Watch the serial output at **115200 baud**:

```sh
pio device monitor -e gateway
```

> **S3 USB note:** if `Serial` output does not appear on your S3 devkit, its
> USB port may be wired to the native USB (GPIO19/20) instead of a
> USB-UART bridge. In that case add `-DARDUINO_USB_CDC_ON_BOOT=1` to the
> `relay`/`node` `build_flags` in `platformio.ini` and replug the USB cable.

## Wiring (SX1278 <-> ESP32 / ESP32-S3)

The pinout is **per-board**: the gateway (WROOM-32 devkit) does not break out
GPIO16/17, so it uses different MOSI/MISO/DIO1 than the S3 boards. NSS, SCK,
DIO0 and RST are identical everywhere.

**Gateway — ESP32-WROOM-32:**

| SX1278 | ESP32 | Notes |
| ------ | ----- | ----- |
| NSS    | GPIO 18 | SPI chip select |
| SCK    | GPIO 13 | SPI clock |
| MOSI   | GPIO 23 | SPI MOSI |
| MISO   | GPIO 19 | SPI MISO |
| DIO0   | GPIO 4  | RX/TX interrupt |
| DIO1   | GPIO 26 | **required for CAD** |
| RST    | GPIO 14 | reset |
| VCC    | 3.3 V   | |
| GND    | GND     | |

**Relay (ESP32-S3-N8R2) & Node (ESP32-S3-N16R8):**

| SX1278 | ESP32 | Notes |
| ------ | ----- | ----- |
| NSS    | GPIO 18 | SPI chip select |
| SCK    | GPIO 13 | SPI clock |
| MOSI   | GPIO 17 | SPI MOSI |
| MISO   | GPIO 21 | SPI MISO |
| DIO0   | GPIO 4  | RX/TX interrupt |
| DIO1   | GPIO 16 | **required for CAD** |
| RST    | GPIO 14 | reset |
| VCC    | 3.3 V   | |
| GND    | GND     | |

Pins are defined in `lib/ldse/LdseConfig.h` (`LDSE_PIN_*`); the gateway env
overrides `LDSE_PIN_MOSI/MISO/DIO1` in `platformio.ini`.

> **SPI pin note:** the firmware calls `SPI.begin(LDSE_PIN_SCK, LDSE_PIN_MISO,
> LDSE_PIN_MOSI, LDSE_PIN_NSS)` before constructing the radio, because
> RadioLib's `Module` calls `SPI.begin()` with no arguments, which would
> otherwise silently fall back to the Arduino board-default SPI pins — and
> those differ between the classic ESP32 (VSPI: 18/19/23/5) and the ESP32-S3
> (FSPI: 12/13/11/10).  With the explicit `SPI.begin()` the radio keeps the
> wired pins on every board.

## Pin analysis (why this pinout)

Every radio signal is routed through the ESP32 GPIO matrix, so any free GPIO
can be used for SPI.  The constraints are only: the pin must exist on the
module, must not be consumed by the internal flash/PSRAM, must be broken out
on the board, and must have the right direction (inputs for MISO/DIO0/DIO1,
outputs for the rest).

| Board | Unusable pins (from datasheets) |
| ----- | ------------------------------- |
| ESP32-WROOM-32 | GPIO6-11 internal flash; GPIO34-39 input-only; GPIO20/24/28-31 not on module; strapping 0/2/5/12/15; UART0 1/3 |
| ESP32-S3-WROOM-1 (N8R2 & N16R8) | GPIO22-25 not on module; GPIO26-32 flash/PSRAM bus; GPIO35-37 octal PSRAM (N16R8); USB 19/20; UART0 43/44; strapping 0/3/45/46; JTAG 39-42 |

The gateway devkit additionally does not break out GPIO16/17 (`U2RXD`/`U2TXD`
alternate functions), so it uses 23/19/26 for MOSI/MISO/DIO1. On the S3 boards
GPIO16/17 are free (`U1TXD`/`U0CTS`/`XTAL_32K_N`; the 32.768 kHz crystal is
not populated, datasheet p.25 "NC"), so they are used for DIO1/MOSI.

| Radio signal | Gateway | Relay/Node | Why |
| ------------ | ------- | ---------- | --- |
| NSS   | GPIO18 | GPIO18 | free on all boards |
| SCK   | GPIO13 | GPIO13 | free on all boards |
| MOSI  | GPIO23 | GPIO17 | gateway: 17 absent on devkit; S3: free |
| MISO  | GPIO19 | GPIO21 | gateway: 19; S3: 19 is USB_D-, so 21 |
| DIO0  | GPIO4  | GPIO4  | interrupt-capable on all boards |
| DIO1  | GPIO26 | GPIO16 | gateway: 16 absent on devkit; S3: free |
| RST   | GPIO14 | GPIO14 | free on all boards |

Earlier revisions were not board-safe and are why this analysis matters:

- Original `DIO0=26 / DIO1=33` is unusable on the S3 (flash/PSRAM bus).
- `NSS=18` collided with the classic VSPI default SCK when SPI pins were not
  set explicitly.
- `MOSI=23 / MISO=19` do not exist (or are USB pins) on the ESP32-S3.
- A shared `DIO1=6` would land on the WROOM-32 internal flash bus.

> **Radio note:** the default frequencies in `LdseConfig.h` (433.3 / 433.5 /
> 433.7 MHz) match the project report's Nepal deployment (433 MHz ISM band).
> For EU/US deployment change `LDSE_FREQ_*` to a legal ISM band (e.g.
> 868.x / 915.x MHz).  You are responsible for operating within your local
> radio regulations.

## Protocol mapping (simulation -> firmware)

| ns-3 phase | Simulation | Firmware |
| ---------- | ---------- | -------- |
| 1.5 layering | LdseLayering assigns layer = distance to sink | `MSG_LAYER_INIT` beacon (gateway -> relay -> node) |
| 2 MAC | LoraChannelManager, 3 channels | `SetChannel` switching Puc / Prc1 / Prc2 |
| 3 routing | IRE routing table, parent selection | `LdseRouting` alpha/beta score |
| 4 data | DATA window uplink, hop-by-hop ACK | `MSG_DATA` + `MSG_ACK`, ACK timeout & retry |
| 5 fire alert | high-priority packet | `MSG_FIRE_ALERT` type defined |
| 6 sync | FTSP, 45 ppm, per-hop re-stamp | `LdseSync`, `timestampUs` re-stamped by relay |
| 7 epoch/energy | SYNC/DATA/SLEEP, energy model | `LdseEpoch`, `LdseEnergy` sleep on SLEEP window |
| 8 congestion | SF9->SF10 + bypass when queue >= 80% | `LdseForwarder::IsCongested`, `GetDataSpreadingFactor` |
| 8 CAD | channel-busy listen + exponential backoff | `LdseRadio::ChannelFree` (RadioLib `scanChannel`) + backoff |

## Frame format

`LdsePacket.h` defines the LoRa payload (16-byte header + up to 48-byte
payload):

```
magic 'L' | version | type | src | dst | origin | hop | layer | ts(4) | seq(2) | energy | rssi | payload
```

The `origin`/`hop` fields let the gateway log the end-to-end source even when
data is forwarded by the relay — matching the ns-3 `LdseTag` design.

## Testing on the bench

1. Flash one gateway, one relay, one node.
2. Power them in order: gateway first, then relay, then node.
3. The node will only start transmitting **after** it hears `LAYER_INIT` and
   `SYNC` (a new epoch begins every 10 s).
4. Watch the gateway serial log: it prints `DATA,...` CSV rows with the
   origin node id, hop count, RSSI, sensor value and a running packet count.
5. To observe congestion control, burst a lot of node traffic (or shorten
   `LDSE_DATA_MS`) and watch the relay report `sf=10 bypass=1`.

## Limitations / notes

- The energy model (`LdseEnergy`) is a *software estimate* for observability —
  it does not put the ESP32 into deep sleep.  `LdseRadio::Sleep` does put the
  SX1278 into its low-power sleep mode inside the SLEEP window.
- CAD uses RadioLib's blocking `scanChannel()` (SX1278 CAD), which requires
  **DIO1** wired to a GPIO.
- Radio timing is not perfectly realtime-precise on the Arduino loop; the
  epoch scheduler tolerates window-boundary jitter on the order of a few ms.
