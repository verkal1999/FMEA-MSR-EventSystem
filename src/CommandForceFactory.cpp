#include "CommandForceFactory.h"
#include "CommandForce.h"

std::unique_ptr<ICommandForce>
CommandForceFactory::create(Kind k, PLCMonitor& mon, IOrderQueue* oq) {
    (void)k; // aktuell nur eine Implementierung
    return std::make_unique<CommandForce>(mon, oq);
}
