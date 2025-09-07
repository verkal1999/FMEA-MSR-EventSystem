#pragma once
#include <memory>

// Vorwärtsdeklarationen, um Header leicht zu halten:
class PLCMonitor;
struct IOrderQueue;
struct ICommandForce;

struct CommandForceFactory {
    enum class Kind { UseMonitor /*, SeparateClient*/ };

    static std::unique_ptr<ICommandForce>
    create(Kind k, PLCMonitor& mon, IOrderQueue* oq = nullptr);
};
