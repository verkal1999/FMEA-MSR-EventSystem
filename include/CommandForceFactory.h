#pragma once
#include <memory>
#include <functional>
#include <string>
class PLCMonitor;
class EventBus;
struct Plan;

struct IOrderQueue;
struct ICommandForce;
struct IWinnerFilter;

class MonitoringActionForce;

struct CommandForceFactory {
    enum class Kind { UseMonitor, KGIngest };   // <— NEU

    static std::unique_ptr<ICommandForce>
    create(Kind k, PLCMonitor& mon, IOrderQueue* oq = nullptr);

    using Fetcher = std::function<std::string(const std::string&)>;
    static std::unique_ptr<IWinnerFilter>
    createWinnerFilter(PLCMonitor& mon, EventBus& bus, Fetcher fetcher, unsigned defaultTimeoutMs);

    static std::unique_ptr<IWinnerFilter>
    createSystemReactionFilter(PLCMonitor& mon, EventBus& bus, Fetcher fetcher, unsigned defaultTimeoutMs);
};