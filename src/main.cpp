#include "PLCMonitor.h"
#include <iostream>
#include <thread>
#include <chrono>
#include "EventBus.h"
#include "ReactionManager.h"
#include <atomic>

int main() {
    auto opt = PLCMonitor::TestServerDefaults(
        "./certificates/client_cert.der",
        "./certificates/client_key.der",
        "opc.tcp://localhost:4850"
    );

    PLCMonitor mon(opt);
    if(!mon.connect()) { std::cerr << "[Client] connect() failed\n"; return 1; }
    std::cout << "[Client] connected\n";

    EventBus bus;
    auto taskMgr = std::make_shared<ReactionManager>(mon);
    auto subTM   = bus.subscribe_scoped(EventType::evD2, taskMgr, 4); // <-- Prio 4 + RAII

    // Rising-Edge-Erkennung auf TriggerD2
    std::atomic<bool> d2Prev{false};
    mon.subscribeBool("TriggerD2", 1, 50.0, 10,
        [&](bool b, const UA_DataValue&){
            if (b) {
                if (!d2Prev.exchange(true)) {
                    bus.post({ EventType::evD2, std::chrono::steady_clock::now(), {} });
                }
            } else {
                d2Prev = false;
            }
        });
    std::cout << "[Client] subscribed: ns=1;s=TriggerD2\n";

    // Event-/OPC-Loop
    for(;;) {
        mon.runIterate(50);     // Publish/Notifications pumpen :contentReference[oaicite:3]{index=3}
        mon.processPosted(16);  // deine interne Job-Queue :contentReference[oaicite:4]{index=4}
        bus.process(16);        // Events an Observer verteilen
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
