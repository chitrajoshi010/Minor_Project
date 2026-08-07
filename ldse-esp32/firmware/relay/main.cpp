/*
 * LDSE Relay firmware (layer 1).
 *
 * Roles:
 *   - Learns its layer (1) from the gateway's LAYER_INIT on the Puc.
 *   - Forwards the FTSP sync beacon to the node on Prc1 (per-hop offset).
 *   - In the DATA window: listens on Prc1 for the node's data, enqueues it,
 *     then forwards each queued packet to the gateway on the Puc using CAD
 *     listen + exponential backoff.
 *   - Congestion control (Phase 8): if the relay queue exceeds 80% it raises
 *     the data SF to 10 and sets the bypass-to-gateway flag.
 *   - Sleeps in the SLEEP window (energy control, Phase 7).
 *
 * Build & flash:  pio run -e relay && pio upload -e relay
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

// Sync/bootstrap state.
bool g_synced = false;
int32_t g_phaseOffsetMs = 0;
uint32_t g_lastSyncFwdMs = 0;

// Simulation-time counters for verification.
uint32_t g_rxFromNode = 0;
uint32_t g_forwardedToGateway = 0;

void
setup()
{
    Serial.begin(LDSE_SERIAL_BAUD);
    delay(300);
    Serial.println("[LDSE] Relay");

    g_sync.Begin(LDSE_DRIFT_PPM);
    g_energy.Begin();

    g_forwarder.SetGatewayId(LDSE_GATEWAY_ID);
    g_forwarder.SetRelayCapacity(LDSE_RELAY_QUEUE_CAPACITY);

    if (!g_radio.Begin(LDSE_FREQ_PUC_MHZ, LDSE_SF_NORMAL))
    {
        Serial.println("[LDSE] Radio init FAILED");
        while (1)
        {
            delay(1000);
        }
    }
    g_radio.SetChannel(LDSE_FREQ_PUC_MHZ, LDSE_SF_NORMAL);
    Serial.println("[LDSE] Relay listening on Puc");
}

void
OnSync(const LdsePacket& pkt)
{
    uint32_t rxLocalUs = micros();
    bool firstSync = !g_synced;
    g_sync.OnReceiveSync(pkt.timestampUs, rxLocalUs);
    g_sync.ApplyCorrection();
    g_sync.SetHopCount(pkt.hopCount + 1);
    g_synced = true;
    // Align our epoch to the reference: reference phase = local phase + offset.
    g_phaseOffsetMs = -(int32_t)(g_sync.GetOffsetUs() / 1000);
    g_energy.Wake();
    Serial.printf("[REL] SYNC offset=%d us, hops=%u\n",
                  g_sync.GetOffsetUs(),
                  g_sync.GetHopCount());

    // Record the gateway as a parent (layer 0, best path).
    g_routing.UpdateParent(pkt.srcId, pkt.layer, pkt.rssiDbm, pkt.energyPct);

    // Forward the beacon to the node on Prc1 at most once per epoch (the
    // gateway bursts beacons now, so within-epoch repeats are suppressed).
    if (!firstSync && (uint32_t)(millis() - g_lastSyncFwdMs) < LDSE_SYNC_FWD_INTERVAL_MS)
    {
        return;
    }
    g_lastSyncFwdMs = millis();

    // Forward the beacon to the node on Prc1 with a corrected timestamp.
    LdsePacket fwd = pkt;
    fwd.srcId = LDSE_RELAY_ID;
    fwd.originId = LDSE_GATEWAY_ID;
    fwd.hopCount = pkt.hopCount + 1;
    fwd.layer = g_layer;
    fwd.timestampUs = g_sync.LocalTimeUs();
    g_radio.SetChannel(LDSE_FREQ_PRC1_MHZ, LDSE_SF_NORMAL);
    g_radio.Send(fwd);
    g_radio.SetChannel(LDSE_FREQ_PUC_MHZ, LDSE_SF_NORMAL);
    Serial.printf("[REL] SYNC forwarded to node @ %lu us\n", fwd.timestampUs);
}

void
OnLayerInit(const LdsePacket& pkt)
{
    // The gateway advertised layer 1 -> we are layer 1.
    g_layer = pkt.layer;
    g_routing.UpdateParent(pkt.srcId, 0, pkt.rssiDbm, pkt.energyPct);
    Serial.printf("[REL] Layer = %u, parent = gateway(%u)\n", g_layer, pkt.srcId);

    // Forward the init to the node on Prc1 at most once per epoch (the
    // gateway bursts beacons now, so within-epoch repeats are suppressed).
    bool forward = !g_synced || (uint32_t)(millis() - g_lastSyncFwdMs) >= LDSE_SYNC_FWD_INTERVAL_MS;
    if (!forward)
    {
        return;
    }
    g_lastSyncFwdMs = millis();

    // Forward the init to the node on Prc1 so it can become layer 2.
    LdsePacket fwd = pkt;
    fwd.srcId = LDSE_RELAY_ID;
    fwd.originId = LDSE_GATEWAY_ID;
    fwd.hopCount = pkt.hopCount + 1;
    fwd.layer = g_layer + 1; // advertised layer for our children
    g_radio.SetChannel(LDSE_FREQ_PRC1_MHZ, LDSE_SF_NORMAL);
    g_radio.Send(fwd);
    g_radio.SetChannel(LDSE_FREQ_PUC_MHZ, LDSE_SF_NORMAL);
    Serial.println("[REL] LAYER_INIT forwarded to node");
}

void
OnDataFromNode(const LdsePacket& pkt)
{
    g_rxFromNode++;
    if (!g_forwarder.Enqueue(pkt))
    {
        Serial.println("[REL] Queue full: dropped packet");
        return;
    }
    Serial.printf("[REL] Rx from node %u, queue=%u/%u congested=%d\n",
                  pkt.originId,
                  g_forwarder.GetQueueSize(),
                  LDSE_RELAY_QUEUE_CAPACITY,
                  g_forwarder.IsCongested());

    // Hop-by-hop ACK to the node on Prc1 (same channel we received on).
    LdsePacket ack;
    ack.type = MSG_ACK;
    ack.srcId = LDSE_RELAY_ID;
    ack.dstId = pkt.srcId;
    ack.originId = LDSE_RELAY_ID;
    ack.seq = pkt.seq;
    g_radio.Send(ack);
    Serial.printf("[REL] ACK to node %u (seq=%u)\n", pkt.srcId, pkt.seq);
}

void
DrainQueue()
{
    LdsePacket pkt;
    while (g_forwarder.Dequeue(pkt))
    {
        // Congestion control: SF10 under congestion, else SF9.
        uint8_t sf = g_forwarder.GetDataSpreadingFactor();

        // Relay forwards to the gateway on the Puc.
        g_radio.SetChannel(LDSE_FREQ_PUC_MHZ, sf);
        g_radio.Standby();

        if (g_forwarder.TryTransmit(g_radio, pkt, LDSE_GATEWAY_ID))
        {
            g_forwardedToGateway++;
            Serial.printf("[REL] Forwarded to gateway seq=%u sf=%u bypass=%d\n",
                          pkt.seq,
                          sf,
                          g_forwarder.ShouldBypassToGateway());
        }
        else
        {
            Serial.printf("[REL] Forward failed (parent failure) seq=%u\n", pkt.seq);
        }
    }

    if (g_forwarder.RouteRefreshRequested())
    {
        g_forwarder.ClearRouteRefresh();
        Serial.println("[REL] Route refresh requested (re-discover gateway)");
    }
}

void
loop()
{
    if (!g_synced)
    {
        // Cold-start bootstrap: no schedule yet, scan the Puc continuously
        // until the gateway's beacon burst is heard (within one epoch).
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
            g_radio.SetChannel(LDSE_FREQ_PUC_MHZ, LDSE_SF_NORMAL);
        }
        else if (win == WIN_DATA)
        {
            g_energy.Wake();
            // Listen for the node on Prc1.
            g_radio.SetChannel(LDSE_FREQ_PRC1_MHZ, LDSE_SF_NORMAL);
        }
        else // WIN_SLEEP
        {
            g_energy.EnterSleep();
            g_radio.Sleep();
            Serial.printf("[REL] Sleep window: energy=%.3f J battery=%.1f mAh\n",
                          g_energy.GetEnergyConsumedJ(),
                          g_energy.GetBatteryMouth());
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
    else if (win == WIN_DATA)
    {
        // Phase A: collect node data on Prc1.
        uint32_t listenDeadline = nowMs + LDSE_RELAY_LISTEN_MS;
        while (millis() < listenDeadline)
        {
            LdsePacket pkt;
            if (g_radio.Receive(pkt, 20))
            {
                if (pkt.type == MSG_DATA || pkt.type == MSG_FIRE_ALERT)
                {
                    OnDataFromNode(pkt);
                }
            }
        }
        // Phase B: forward the queue to the gateway on the Puc.
        DrainQueue();
    }
}
