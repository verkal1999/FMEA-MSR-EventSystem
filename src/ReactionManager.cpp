#include "ReactionManager.h"
#include "Event.h"
#include "PLCMonitor.h"
#include <iostream>

void ReactionManager::onMethod(const Event& ev) {
    switch (ev.type) {
        case EventType::evD2: {
            std::cout << "[ReactionManager] evD2 empfangen -> plane Aktion\n";
            std::cout << "[ReactionManager] onMethod: " << static_cast<int>(ev.type) << "\n";
            // Beispielaktion: in 2 s DiagnoseFinished TRUE schreiben
            // mon_.post(...) existiert bereits in deinem PLCMonitor (Job-Queue). :contentReference[oaicite:2]{index=2}
            mon_.post([&m=mon_]{
                m.writeBool("DiagnoseFinished", /*ns*/1, true);
                std::cout << "[ReactionManager] DiagnoseFinished=TRUE geschrieben\n";
            });
            break;
        }
        // weitere Events hier
        default: break;
    }
}
