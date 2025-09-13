#include "CommandForceFactory.h"
#include "CommandForce.h"
#include "MonActionForce.h"
#include "PLCMonitor.h"
#include "EventBus.h"
#include "SystemReactionForce.h"
#include "KgIngestionForce.h"  

std::unique_ptr<ICommandForce>
CommandForceFactory::create(Kind k, PLCMonitor& mon, IOrderQueue* oq) {
    switch (k) {
        case Kind::UseMonitor:
            return std::make_unique<CommandForce>(mon, oq);
        case Kind::KGIngest:
            // KG-Ingestion benötigt keinen PLCMonitor-Zugriff – wir geben nur den Bus weiter.
            // Hier kurzer Hack: EventBus ist global/Singleton? Falls nicht, passe Signatur an.
            // Für die schnelle Integration nehmen wir an, dass ein globaler Bus existiert
            // oder du eine Überladung mit EventBus& ergänzt.
            extern EventBus* gEventBus;      // (falls du keinen globalen Bus willst: erstelle eine 2. create-Überladung)
            return std::make_unique<KgIngestionForce>(*gEventBus);
    }
    return std::make_unique<CommandForce>(mon, oq);
}

std::unique_ptr<IWinnerFilter>
CommandForceFactory::createWinnerFilter(PLCMonitor& mon, EventBus& bus,
                                        std::function<std::string(const std::string&)> fetcher,
                                        unsigned defaultTimeoutMs) {
    return std::make_unique<MonitoringActionForce>(mon, bus, std::move(fetcher), defaultTimeoutMs);
}

std::unique_ptr<IWinnerFilter>
CommandForceFactory::createSystemReactionFilter(PLCMonitor& mon, EventBus& bus,
                                                Fetcher fetcher, unsigned defaultTimeoutMs) {
    return std::make_unique<SystemReactionForce>(mon, bus, std::move(fetcher), defaultTimeoutMs);
}
