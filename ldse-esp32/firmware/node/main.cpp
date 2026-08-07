/*
 * LDSE End device (node) firmware (layer 2).
 *
 * Roles:
 *   - Learns layer 2 from the relay's forwarded LAYER_INIT on Prc1.
 *   - Synchronizes its clock from the relay-forwarded FTSP beacon.
 *   - In the DATA window: builds a sensor payload, sends it to its parent
 *     (the relay) on Prc1, and waits for an ACK.  Retries with exponential
 *     backoff; on repeated failure it marks the parent failed and requests a
 *     route refresh (re-discovery).
 *   - Sleeps in the SLEEP window (energy control).
 *
 * Build & flash:  pio run -e node && pio upload -e node
 */

#include <Arduino.h>

#include "LdseConfig.h"
#include "LdseEpoch.h"
#include "LdseEnergy.h"
#include "LdseForwarder.h"
#include "LdsePacket.h"
#include "LdseRadio.h"
#include "LdseRouting.h"
#include "LdseSync.h"

using namespace ldse;

LdseRadio g_radio;
LdseSync g_sync;
LdseRouting g_routing;
LdseForwarder g_forwarder;
LdseEnergy g_energy;

uint8_t g_layer = 0;
uint8_t g_lastWindow = WIN_SLEEP;
uint16_t g_seq = 0;
uint32_t g_txSuccess = 0;
uint32_t g_txFail = 0;

// Sync/bootstrap state.
bool g_synced = false;
int32_t g_phaseOffsetMs = 0;

// Simulated sensor reading (temperature, degrees C). Replace with a real
// sensor read if available.
float
ReadSensor()
{
    // A slowly drifting temperature to make the demo readable.
    return 24.0f + 2.0f * sinf(millis() / 30000.0f);
}

void
setup()
{
    Serial.begin(LDSE_SERIAL_BAUD);
    delay(300);
    Serial.println("[LDSE] End device (node)");

    g_sync.Begin(LDSE_DRIFT_PPM);
    g_energy.Begin();
    g_forwarder.SetGatewayId(LDSE_GATEWAY_ID);

    if (!g_radio.Begin(LDSE_FREQ_PRC1_MHZ, LDSE_SF_NORMAL))
    {
        Serial.println("[LDSE] Radio init FAILED");
        while (1)
        {
            delay(1000);
        }
    }
    g_radio.SetChannel(LDSE_FREQ_PRC1_MHZ, LDSE_SF_NORMAL);
    Serial.println("[LDSE] Node listening on Prc1");
}

void
OnSync(const LdsePacket& pkt)
{
    uint32_t rxLocalUs = micros();
    g_sync.OnReceiveSync(pkt.timestampUs, rxLocalUs);
    g_sync.ApplyCorrection();
    g_sync.SetHopCount(pkt.hopCount + 1);
    g_synced = true;
    // Align our epoch to the reference: reference phase = local phase + offset.
    g_phaseOffsetMs = -(int32_t)(g_sync.GetOffsetUs() / 1000);
    g_energy.Wake();
    Serial.printf("[NODE] SYNC offset=%d us, hops=%u\n",
                  g_sync.GetOffsetUs(),
                  g_sync.GetHopCount());

    // The relay is our parent candidate (it forwarded the beacon).
    g_routing.UpdateParent(pkt.srcId, g_layer ? g_layer - 1 : pkt.layer,
                           pkt.rssiDbm, pkt.energyPct);
}

void
OnLayerInit(const LdsePacket& pkt)
{
    // The relay advertised its child layer -> we are that layer.
    g_layer = pkt.layer;
    g_routing.UpdateParent(pkt.srcId, g_layer - 1, pkt.rssiDbm, pkt.energyPct);
    Serial.printf("[NODE] Layer = %u, parent = relay(%u)\n", g_layer, pkt.srcId);
}

void
SendData()
{
    if (!g_routing.HasParent())
    {
        Serial.println("[NODE] No parent yet: waiting for LAYER_INIT/SYNC");
        return;
    }

    uint8_t parent = g_routing.SelectBestParent();
    uint8_t sf = g_forwarder.GetDataSpreadingFactor();

    LdsePacket pkt;
    pkt.type = MSG_DATA;
    pkt.srcId = LDSE_NODE_ID;
    pkt.dstId = parent;
    pkt.originId = LDSE_NODE_ID;
    pkt.hopCount = 1;
    pkt.layer = g_layer;
    pkt.seq = g_seq++;
    float value = ReadSensor();
    memcpy(pkt.payload, &value, sizeof(float));
    pkt.payloadLen = sizeof(float);
    pkt.energyPct = static_cast<uint8_t>(g_energy.GetBatteryMouth() /
                                         LDSE_BATTERY_CAPACITY_MAH * 100.0f);

    g_radio.SetChannel(LDSE_FREQ_PRC1_MHZ, sf);
    g_radio.Standby();

    // Try transmit with CAD listen + exponential backoff.
    if (!g_forwarder.TryTransmit(g_radio, pkt, parent))
    {
        g_txFail++;
        Serial.println("[NODE] TX failed after retries: parent failure");
        g_routing.RemoveParent(parent); // route refresh: re-discover
        return;
    }

    // Wait for the ACK from the parent.
    uint32_t deadline = millis() + LDSE_ACK_TIMEOUT_MS;
    bool acked = false;
    while (millis() < deadline)
    {
        LdsePacket ack;
        if (g_radio.Receive(ack, 20) && ack.type == MSG_ACK && ack.seq == pkt.seq)
        {
            acked = true;
            break;
        }
    }
    if (acked)
    {
        g_txSuccess++;
        Serial.printf("[NODE] DATA seq=%u acked, T=%.2f C, sf=%u\n",
                      pkt.seq, value, sf);
    }
    else
    {
        g_txFail++;
        Serial.println("[NODE] No ACK: retry next epoch, route refresh");
        g_forwarder.OnParentFailure(parent);
        g_routing.RemoveParent(parent);
    }
}

void
loop()
{
    if (!g_synced)
    {
        // Cold-start bootstrap: no schedule yet, scan Prc1 continuously until
        // the relay's forwarded beacon is heard (within one epoch).
        LdsePacket pkt;
        if (g_radio.Receive(pkt, 20))
        {
            if (pkt.type == MSG_SYNC)
            {
                OnSync(pkt);
            }
            else if (pkt.type == MSG_LAYER_INIT)
            {
                OnLayerInit(pkt);
            }
        }
        return;
    }

    uint32_t nowMs = millis();
    uint8_t win = LdseEpoch::GetWindow(nowMs, g_phaseOffsetMs);

    if (win != g_lastWindow)
    {
        if (win == WIN_SYNC)
        {
            g_energy.Wake();
            g_radio.SetChannel(LDSE_FREQ_PRC1_MHZ, LDSE_SF_NORMAL);
        }
        else if (win == WIN_DATA)
        {
            g_energy.Wake();
            g_radio.SetChannel(LDSE_FREQ_PRC1_MHZ, LDSE_SF_NORMAL);
            // Send shortly after the DATA window opens (relay is listening).
            delay(LDSE_NODE_TX_OFFSET_MS);
            SendData();
        }
        else // WIN_SLEEP
        {
            g_energy.EnterSleep();
            g_radio.Sleep();
            Serial.printf("[NODE] Sleep: energy=%.3f J battery=%.1f mAh txOK=%u txFail=%u\n",
                          g_energy.GetEnergyConsumedJ(),
                          g_energy.GetBatteryMouth(),
                          g_txSuccess,
                          g_txFail);
        }
        g_lastWindow = win;
    }

    if (win == WIN_SYNC)
    {
        LdsePacket pkt;
        if (g_radio.Receive(pkt, 20))
        {
            if (pkt.type == MSG_SYNC)
            {
                OnSync(pkt);
            }
            else if (pkt.type == MSG_LAYER_INIT)
            {
                OnLayerInit(pkt);
            }
        }
    }
}
