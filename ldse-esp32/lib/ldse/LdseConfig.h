#pragma once

/*
 * LdseConfig.h - global LDSE constants for the ESP32 implementation.
 *
 * Maps the paper parameters (Table 3 / Table 4) to practical values.
 *
 * NOTE: 433.3/433.5/433.7 MHz is the Nepal ISM band (433.05-434.79 MHz),
 * matching the project report's 433 MHz deployment. If you deploy in a
 * different region, change LDSE_FREQ_PUC/PRC to your legal ISM band
 * (e.g. 868.0 / 868.5 MHz in EU, 915.0 / 915.5 MHz in US).
 *
 * GPIO choice: the values below are the S3 defaults (relay/node). The
 * gateway env (WROOM-32) overrides MOSI/MISO/DIO1 via build_flags because
 * its devkit does not break out GPIO16/17. GPIO26-32 are wired to the
 * flash/PSRAM on every ESP32-S3-WROOM module and GPIO33-37 are used by the
 * octal PSRAM on N8R8/N16R8 modules, so they are unusable for the radio on
 * the S3 boards. GPIO6-11 are the internal flash bus on the WROOM-32.
 */

// ---------------- Radio pinout (SX1278 on ESP32 / ESP32-S3) ----------------
//   NSS  -> GPIO 18      DIO0 -> GPIO 4       RST -> GPIO 14
//   SCK  -> GPIO 13      MOSI -> GPIO 17      MISO -> GPIO 21
//   DIO1 -> GPIO 16      (needed for Channel Activity Detection)
//
// Gateway (WROOM-32) overrides: MOSI=23, MISO=19, DIO1=26 (see platformio.ini).
#define LDSE_PIN_NSS 18
#define LDSE_PIN_SCK 13
#ifndef LDSE_PIN_MOSI
#define LDSE_PIN_MOSI 17
#endif
#ifndef LDSE_PIN_MISO
#define LDSE_PIN_MISO 21
#endif
#define LDSE_PIN_DIO0 4
#define LDSE_PIN_RST 14
#ifndef LDSE_PIN_DIO1
#define LDSE_PIN_DIO1 16
#endif

// ---------------- Radio parameters (paper Table 3) ----------------
#define LDSE_BW_KHZ 125.0f
#define LDSE_SF_NORMAL 9
#define LDSE_SF_CONGESTED 10
#define LDSE_CODING_RATE 5 // 4/5 -> RadioLib coding rate value 5
#define LDSE_TX_POWER_DBM 20
#define LDSE_PREAMBLE_SYMBOLS 8
#define LDSE_SYNC_WORD 0x12

// ---------------- Channel plan [MHz] ----------------
// Nepal ISM band 433.05-434.79 MHz (report 3.2.1: 433 MHz operation).
// Puc  = primary uplink / control channel (route discovery, sync)
// Prc1 = data channel relay <-> node
// Prc2 = reserved data channel (used for the congestion bypass path)
#define LDSE_FREQ_PUC_MHZ 433.3f
#define LDSE_FREQ_PRC1_MHZ 433.5f
#define LDSE_FREQ_PRC2_MHZ 433.7f

// ---------------- Node identities ----------------
#define LDSE_GATEWAY_ID 0
#define LDSE_RELAY_ID 1
#define LDSE_NODE_ID 2

// ---------------- Epoch timing [ms] ----------------
// SYNC / DATA / SLEEP windows (paper Section 2.2.4).
// 10 s epoch keeps the demo observable on a serial monitor.
#define LDSE_EPOCH_MS 10000
#define LDSE_SYNC_MS 2000
#define LDSE_DATA_MS 6000
#define LDSE_SLEEP_MS (LDSE_EPOCH_MS - LDSE_SYNC_MS - LDSE_DATA_MS)

// Gateway broadcasts LAYER_INIT + SYNC every period during the SYNC window,
// so unsynced children cold-start within one epoch regardless of boot phase.
#define LDSE_SYNC_BEACON_PERIOD_MS 250

// Minimum gap between relay-forwards of the sync beacon to the node
// (the gateway now bursts beacons, so within-epoch repeats are suppressed).
#define LDSE_SYNC_FWD_INTERVAL_MS (LDSE_EPOCH_MS - LDSE_SYNC_MS)

// Time inside the DATA window when the node transmits (relay listens before).
#define LDSE_NODE_TX_OFFSET_MS 200
// Relay listens on Prc1 for the node for this long, then forwards on Puc.
#define LDSE_RELAY_LISTEN_MS 2500

// ---------------- FTSP (paper Section 2.2.4, Phase 6) ----------------
#define LDSE_DRIFT_PPM 45.0f
#define LDSE_SYNC_PERIOD_EPOCHS 1

// ---------------- Forwarding / congestion (Phase 8) ----------------
#define LDSE_RELAY_QUEUE_CAPACITY 10
#define LDSE_CONGESTION_THRESHOLD 0.8f
#define LDSE_BACKOFF_SLOT_MS 2
#define LDSE_MAX_ACK_RETRIES 3
#define LDSE_ACK_TIMEOUT_MS 1500

// ---------------- Energy model (paper Table 3 / Figures 8-10) ----------------
#define LDSE_ACTIVE_POWER_W 0.396f
#define LDSE_SLEEP_POWER_W 0.00000333f // 0.9 uA * 3.7 V
#define LDSE_BATTERY_CAPACITY_MAH 2000.0f
#define LDSE_BATTERY_VOLTAGE_V 3.7f

// ---------------- Serial ----------------
#define LDSE_SERIAL_BAUD 115200
