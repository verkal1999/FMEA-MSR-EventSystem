#include "CommandForceFactory.h"
#include "CommandForce.h"
#include "MonActionForce.h"
#include "PLCMonitor.h"
#include "EventBus.h"

std::unique_ptr<ICommandForce>
CommandForceFactory::create(Kind k, PLCMonitor& mon, IOrderQueue* oq) {
    (void)k;
    return std::make_unique<CommandForce>(mon, oq);
}

std::unique_ptr<IWinnerFilter>
CommandForceFactory::createWinnerFilter(PLCMonitor& mon, EventBus& bus,
                                        std::function<std::string(const std::string&)> fetcher,
                                        unsigned defaultTimeoutMs) {
    return std::make_unique<MonitoringActionForce>(mon, bus,
                                                   std::move(fetcher),
                                                   defaultTimeoutMs);
}
