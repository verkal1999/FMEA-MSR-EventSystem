#include "PLCMonitor.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <limits>
#include <vector>
#include <atomic>
#include <optional>

static volatile std::sig_atomic_t g_run = 1;
static void onSig(int) { g_run = 0; }
std::atomic<bool> g_snapshotRequested{false};


int main() {
    std::signal(SIGINT,  onSig);
    std::signal(SIGTERM, onSig);
    int pendingBool2 = -1;
    int D2True = 1;
    UA_Int16 prev = std::numeric_limits<UA_Int16>::lowest();
    PLCMonitor::Options opt;
    opt.endpoint       = "opc.tcp://DESKTOP-LNJR8E0:4840";
    opt.username       = "Admin";
    opt.password       = "123456";
    opt.certDerPath    = "client_cert.der";
    opt.keyDerPath     = "client_key.der";
    opt.applicationUri = "urn:DESKTOP-LNJR8E0:Test:opcua-client";
    opt.nsIndex        = 4;
    //opt.nodeIdStr      = "OPCUA.Z1";
    std::optional<std::chrono::steady_clock::time_point> ackOffAt;
    PLCMonitor mon(opt);
    if(!mon.connect()) {
        std::cerr << "Connect failed\n";
        return 1;
    }
    std::cout << "Connected and session ACTIVATED.\n";

    // === NEU: Subscription statt Polling ===
    const double   samplingMs = 0.0;   // Server revidiert auf fastest practical
    const UA_UInt32 queueSz   = 256;
    bool okD2 = mon.subscribeBool("OPCUA.TriggerD2", opt.nsIndex, 0.0, 8,
    [&mon,&opt,&D2True](UA_Boolean b, const UA_DataValue& dv){
        std::cout << "[TriggerD2] = " << (b ? "TRUE" : "FALSE");
        if(dv.hasSourceTimestamp) {
            UA_DateTime ts = dv.sourceTimestamp;
            std::cout << " (ts=" << ts/10000 << " ms)";
        }
        std::cout << std::endl;

        // Wenn TRUE: Snapshot lesen
        if(b) {
            g_snapshotRequested.store(true, std::memory_order_relaxed);
            D2True = 1;
        } else
        {
            D2True = 0;
        }
    });
    // Hauptloop: nur noch iterieren, damit Publish/Callbacks verarbeitet werden
    // --- Phase 1: Normalbetrieb ---
    while (g_run) {
        mon.runIterate(20);   // treibt Subscriptions/Eventloop

        if (pendingBool2 != -1) {
            bool val = (pendingBool2 == 1);
            if (mon.writeBool("OPCUA.bool2", opt.nsIndex, val)) {
                std::cout << "-> bool2 = " << (val ? "TRUE" : "FALSE") << "\n";
                pendingBool2 = -1;

                if (val) { // Bedingung erfüllt: Programm beenden
                    std::cout << "Bedingung erfüllt, beende Programm...\n";
                    g_run = 0;       // Schleife verlassen
                }
            }
        }
        // Ack-Puls NICHT blockierend:
        if (D2True == 1 && !ackOffAt.has_value()) {
            if (mon.writeBool("OPCUA.DiagnoseFinished", opt.nsIndex, true))
                std::cout << "-> DiagnoseFinished = TRUE\n";
            else
                std::cerr << "Write DiagnoseFinished=TRUE FAILED\n";

            // Pulsbreite robust > SPS-Taskzeit (z. B. 250 ms)
            ackOffAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            D2True = 0; // „Puls läuft“
        }

        if (ackOffAt && std::chrono::steady_clock::now() >= *ackOffAt) {
            if (mon.writeBool("OPCUA.DiagnoseFinished", opt.nsIndex, false))
                std::cout << "-> DiagnoseFinished = FALSE (pulse end)\n";
            else
                std::cerr << "Write DiagnoseFinished=FALSE FAILED\n";
            ackOffAt.reset();
        }
        // Snapshot außerhalb des Eventloops ausführen
        if (g_snapshotRequested.exchange(false, std::memory_order_relaxed)) {
            std::vector<std::pair<UA_UInt16,std::string>> nodes = {
                {opt.nsIndex, "OPCUA.Z1"},
                {opt.nsIndex, "OPCUA.Z2"},
                {opt.nsIndex, "OPCUA.bool1"},
                {opt.nsIndex, "OPCUA.bool2"},
                {opt.nsIndex, "OPCUA.Z3"}
            };

            std::vector<PLCMonitor::SnapshotItem> snap;
            if(mon.readSnapshot(nodes, snap, UA_TIMESTAMPSTORETURN_SOURCE, 0.0)) {
                std::cout << "--- SNAPSHOT ---\n";
                for(const auto& it : snap) {
                    std::cout << "[" << it.ns << ":" << it.nodeIdStr << "] ";

                    if(it.dv.hasValue && UA_Variant_isScalar(&it.dv.value)) {
                        auto type = it.dv.value.type;
                        if(type == &UA_TYPES[UA_TYPES_INT16]) {
                            std::cout << *static_cast<UA_Int16*>(it.dv.value.data);
                        } else if(type == &UA_TYPES[UA_TYPES_BOOLEAN]) {
                            std::cout << (*(UA_Boolean*)it.dv.value.data ? "TRUE" : "FALSE");
                        } else {
                            int idx = -1;
                            for(size_t i = 0; i < UA_TYPES_COUNT; ++i) {
                                if(type == &UA_TYPES[i]) { idx = static_cast<int>(i); break; }
                            }
                            std::cout << "(unknown type idx=" << idx << ")";
                        }
                    } else {
                        std::cout << "(no value)";
                    }

                    if(it.dv.hasSourceTimestamp)
                        std::cout << " (ts=" << it.dv.sourceTimestamp/10000 << " ms)";
                    std::cout << "\n";
                }
                for(auto& it : snap) UA_DataValue_clear(&it.dv);
            } else {
                std::cerr << "Snapshot-Read fehlgeschlagen\n";
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // --- Phase 2: Drain-Phase (sauberer Shutdown) ---
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200)) {
        mon.runIterate(20);   // noch eintreffende PublishResponses verarbeiten
    }

    // Aufräumen
    mon.unsubscribe();   // optional, disconnect räumt Subscriptions sowieso ab
    mon.disconnect();
    return 0;
}
