#pragma once
#include "Plan.h"

// Vorwärtsdeklarationen, um Header schlank zu halten:
class PLCMonitor;

struct IOrderQueue {
    virtual ~IOrderQueue() = default;
    virtual bool blockResource(const std::string& /*resId*/) { return true; }
    virtual bool reroute(const std::string& /*resId*/, const std::string& /*criteriaJson*/) { return true; }
    virtual bool unblockResource(const std::string& /*resId*/) { return true; }
};

struct ICommandForce {
    virtual ~ICommandForce() = default;
    // synchroner Ablauf; Rückgabe 1 = OK, 0 = Fehler
    virtual int execute(const Plan& p) = 0;
};

class CommandForce : public ICommandForce {
public:
    explicit CommandForce(PLCMonitor& mon, IOrderQueue* oq = nullptr);
    int execute(const Plan& p) override;

private:
    PLCMonitor&  mon_;
    IOrderQueue* oq_;
};
