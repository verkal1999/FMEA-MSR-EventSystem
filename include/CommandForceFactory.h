#pragma once
#include <memory>
#include <functional>
#include <string>
class PLCMonitor;
class EventBus;
struct Plan;

struct IOrderQueue;
struct ICommandForce;
struct IWinnerFilter;          // aus MonActionForce.h bekannt

class MonitoringActionForce;   // fwd

struct CommandForceFactory {
    enum class Kind { UseMonitor /*...*/ };

    static std::unique_ptr<ICommandForce>
    create(Kind k, PLCMonitor& mon, IOrderQueue* oq = nullptr);

    // vereinfacht: kein PlanBuilder mehr
    static std::unique_ptr<IWinnerFilter>
    createWinnerFilter(PLCMonitor& mon, EventBus& bus,
                       std::function<std::string(const std::string&)> fetcher,
                       unsigned defaultTimeoutMs = 30000);
};
