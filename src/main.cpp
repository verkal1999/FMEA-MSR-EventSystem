#include "PLCMonitor.h"
#include "EventBus.h"
#include "ReactionManager.h"
#include "AckLogger.h"
#include "PythonRuntime.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    // Zertifikat/Key/Endpoint passend zu deinem Testserver
    //auto opt = PLCMonitor::TestServerDefaults(
    //    "./certificates/client_cert.der",
    //    "./certificates/client_key.der",
    //    "opc.tcp://localhost:4850"
    //);
    PLCMonitor::Options opt;
    opt.endpoint       = "opc.tcp://DESKTOP-LNJR8E0:4840"; // anpassen falls nötig
    opt.username       = "Admin";
    opt.password       = "123456";
    opt.certDerPath    = R"(..\..\certificates\client_cert.der)";
    opt.keyDerPath     = R"(..\..\certificates\client_key.der)";
    opt.applicationUri = "urn:DESKTOP-LNJR8E0:Test:opcua-client";
    opt.nsIndex        = 4;

    PythonRuntime::ensure_started();

    PLCMonitor mon(opt);
    if (!mon.connect()) {
        std::cerr << "[Client] connect() failed\n";
        return 1;
    }
    std::cout << "[Client] connected\n";

    // EventBus + ReactionManager + AckLogger
    EventBus bus;
    auto rm         = std::make_shared<ReactionManager>(mon, bus);
    auto subD2      = bus.subscribe_scoped(EventType::evD2, rm, 4);
    auto ackLogger  = std::make_shared<AckLogger>();
    auto subPlan    = bus.subscribe_scoped(EventType::evReactionPlanned, ackLogger, 1);
    auto subDone    = bus.subscribe_scoped(EventType::evReactionDone,    ackLogger, 1);

    // ---------- Snapshot (nicht-blockierend warten) ----------
    // Holt alle Variablen (ns=1) bis Tiefe 5, max. 200 Referenzen pro Knoten
    /*auto fut = mon.TestServer_snapshotAllAsync(2 nsFilter, 5, 200);

    using namespace std::chrono_literals;
    while (fut.wait_for(0ms) != std::future_status::ready) {
        mon.runIterate(50);     // OPC UA-Client-State-Machine pumpen
        mon.processPosted(32);  // intern gepostete Arbeiten abarbeiten
        std::this_thread::sleep_for(10ms);
    }   
    

    const auto snapshot = fut.get(); 
    std::cout << "[Snapshot] entries: " << snapshot.size() << "\n";
    for (const auto& e : snapshot) {
        std::cout << e.nodeIdText << "  " << e.browsePath
                  << "  [" << e.dataType << "] = " << e.value
                  << "  (Status: " << e.status << ")\n";
    } */

    // ---------- TriggerD2 Rising-Edge -> Event ----------
    std::atomic<bool> d2Prev{false};
    mon.subscribeBool("OPCUA.TriggerD2", opt.nsIndex, /*samplingMs*/0.0, /*queueSize*/10,
        [&](bool b, const UA_DataValue&) {
            if (b) {
                if (!d2Prev.exchange(true)) {
                    bus.post({ EventType::evD2, std::chrono::steady_clock::now(), {} });
                }
            } else {
                d2Prev = false;
            }
        });
    std::cout << "[Client] subscribed: ns=4;s=TriggerD2\n";

    // ---------- Main-Loop ----------
    for (;;) {
        mon.runIterate(50);     // Notifications / Publishes verarbeiten
        mon.processPosted(16);  // eigene Jobs
        bus.process(16);        // Events an Observer verteilen
        //std::this_thread::sleep_for(10ms);
    }
}