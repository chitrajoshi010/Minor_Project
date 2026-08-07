/*
 * LDSE Gateway firmware (layer 0).
 *
 * Roles:
 *   - Announces layer 1 on the Puc during the SYNC window.
 *   - Sends the FTSP sync beacon (timestamp = reference clock).
 *   - Receives sensor data on the Puc during the DATA window (from the relay,
 *     which re-transmits node data on the Puc) and prints CSV to serial.
 *
 * The gateway stays awake the whole time (it is the network sink, assumed to
 * be mains powered like the paper's RAK7258 gateway).
 *
 * Build & flash:  pio run -e gateway && pio upload -e gateway
 */

#include <Arduino.h>

#include "LdseConfig.h"
#include "LdseEpoch.h"
#include "LdsePacket.h"
#include "LdseRadio.h"
#include "LdseSync.h"

using namespace ldse;

LdseRadio g_radio;
LdseSync g_sync;
uint16_t g_seq = 0;
uint8_t g_lastWindow = WIN_SLEEP;
uint32_t g_nextBeaconMs = 0;

// Data-rate accounting.
uint32_t g_dataPackets = 0;

void
setup()
{
    Serial.begin(LDSE_SERIAL_BAUD);
    delay(300);
    Serial.println("[LDSE] Gateway (layer 0)");

    g_sync.Begin(0.0f); // gateway is the reference clock
    g_sync.SetHopCount(0);

    if (!g_radio.Begin(LDSE_FREQ_PUC_MHZ, LDSE_SF_NORMAL))
    {
        Serial.println("[LDSE] Radio init FAILED");
        while (1)
        {
            delay(1000);
        }
    }
    g_radio.SetChannel(LDSE_FREQ_PUC_MHZ, LDSE_SF_NORMAL);
    Serial.println("[LDSE] Radio on Puc ready");
}

void
BroadcastLayerInit()
{
    LdsePacket pkt;
    pkt.type = MSG_LAYER_INIT;
    pkt.srcId = LDSE_GATEWAY_ID;
    pkt.dstId = LDSE_BROADCAST;
    pkt.originId = LDSE_GATEWAY_ID;
    pkt.hopCount = 0;
    pkt.layer = 1; // advertised layer for direct children
    pkt.seq = g_seq++;
    g_radio.Send(pkt);
    Serial.println("[GW] LAYER_INIT layer=1 sent");
}

void
SendSync()
{
    LdsePacket pkt;
    pkt.type = MSG_SYNC;
    pkt.srcId = LDSE_GATEWAY_ID;
    pkt.dstId = LDSE_BROADCAST;
    pkt.originId = LDSE_GATEWAY_ID;
    pkt.hopCount = 0;
    pkt.layer = 0;
    pkt.timestampUs = g_sync.LocalTimeUs(); // reference timestamp
    pkt.seq = g_seq++;
    g_radio.Send(pkt);
    Serial.printf("[GW] SYNC sent @ %lu us\n", pkt.timestampUs);
}

void
SendAck(const LdsePacket& rx)
{
    LdsePacket ack;
    ack.type = MSG_ACK;
    ack.srcId = LDSE_GATEWAY_ID;
    ack.dstId = rx.originId;
    ack.originId = LDSE_GATEWAY_ID;
    ack.seq = rx.seq;
    g_radio.Send(ack);
}

void
loop()
{
    uint32_t nowMs = millis();
    uint8_t win = LdseEpoch::GetWindow(nowMs);

    if (win != g_lastWindow)
    {
        if (win == WIN_SYNC)
        {
            g_radio.SetChannel(LDSE_FREQ_PUC_MHZ, LDSE_SF_NORMAL);
            g_nextBeaconMs = nowMs;
        }
        else if (win == WIN_DATA)
        {
            g_radio.SetChannel(LDSE_FREQ_PUC_MHZ, LDSE_SF_NORMAL);
        }
        g_lastWindow = win;
    }

    // Beacon burst across the whole SYNC window so unsynced children can
    // cold-start and re-sync regardless of their boot phase.
    if (win == WIN_SYNC && (int32_t)(nowMs - g_nextBeaconMs) >= 0)
    {
        BroadcastLayerInit();
        SendSync();
        g_nextBeaconMs = nowMs + LDSE_SYNC_BEACON_PERIOD_MS;
    }

    // The gateway listens on the Puc for the whole epoch.
    LdsePacket pkt;
    if (g_radio.Receive(pkt, 10))
    {
        if (pkt.type == MSG_DATA || pkt.type == MSG_FIRE_ALERT)
        {
            g_dataPackets++;
            float sensor = 0.0f;
            if (pkt.payloadLen == sizeof(float))
            {
                memcpy(&sensor, pkt.payload, sizeof(float));
            }
            // CSV: epoch_ms,origin,src,hops,seq,rssi,origin_layer,payload,count
            Serial.printf("DATA,%lu,%u,%u,%u,%u,%d,%u,%.2f,%u\n",
                          nowMs % LDSE_EPOCH_MS,
                          pkt.originId,
                          pkt.srcId,
                          pkt.hopCount,
                          pkt.seq,
                          pkt.rssiDbm,
                          pkt.layer,
                          sensor,
                          g_dataPackets);
            SendAck(pkt);
        }
    }
}
