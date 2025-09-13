#pragma once
#include "CommandForce.h"
#include "EventBus.h"
#include <pybind11/embed.h>
#include "PythonWorker.h"

class KgIngestionForce final : public ICommandForce {
public:
    explicit KgIngestionForce(EventBus& bus) : bus_(bus) {}
    int execute(const Plan& p) override;

private:
    EventBus& bus_;
    // helpers
    static std::string getStr(const UAValueMap& m, int idx) {
        auto it = m.find(idx);
        if (it == m.end() || it->second.index() != 6) return std::string{};
        return std::get<std::string>(it->second);
    }
};
