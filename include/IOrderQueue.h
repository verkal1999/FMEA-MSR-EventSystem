#pragma once
#include "Plan.h"
struct IOrderQueue {
    virtual ~IOrderQueue() = default;
    virtual bool blockResource(const std::string& /*resId*/) { return true; }
    virtual bool reroute(const std::string& /*resId*/, const std::string& /*criteriaJson*/) { return true; }
    virtual bool unblockResource(const std::string& /*resId*/) { return true; }
};