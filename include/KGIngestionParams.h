#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct KgIngestionParams {
    // Meta
    std::string corr;
    std::string process;
    std::string summary;
    std::string resourceId;

    // Zeit/Name
    std::string ts;               // "YYYY-MM-DD_HH-mm-ss"
    std::string individualName;   // corr + "_" + ts

    // Snapshot (bereits vorbereitet)
    std::string snapshotWrapped;  // "==InventorySnapshot==" + json + "==InventorySnapshot=="
    std::string lastSkill;        // aus Snapshot (OPCUA.lastExecutedSkill)
    std::string lastProcess;      // aus Snapshot (OPCUA.lastExecutedProcess)

    // Listen (füllt der FailureRecorder aus Events)
    std::vector<std::string> sysReactions;    // IRIs der ausgeführten System-Reactions
    std::vector<std::string> monReactions;    // IRIs der ausgeführten Monitoring-Actions
    std::vector<std::string> failureModes;    // optional

    nlohmann::json toJson() const {
        using nlohmann::json;
        json j = {
            {"corr", corr}, {"process", process}, {"summary", summary}, {"resourceId", resourceId},
            {"ts", ts}, {"individualName", individualName},
            {"snapshot", snapshotWrapped}, {"lastSkill", lastSkill}, {"lastProcess", lastProcess}
        };
        if (!sysReactions.empty()) j["sysReactions"] = sysReactions;
        if (!monReactions.empty()) j["monReactions"] = monReactions;
        if (!failureModes.empty()) j["failureModes"] = failureModes;
        return j;
    }
    /*nlohmann::json toJson() const {
        using nlohmann::json;
        return json{
            {"corr", corr}, {"process", process}, {"summary", summary}, {"resourceId", resourceId},
            {"ts", ts}, {"individualName", individualName},
            {"snapshot", snapshotWrapped}, {"lastSkill", lastSkill}, {"lastProcess", lastProcess},
            {"sysReactions", sysReactions}, {"monReactions", monReactions}, {"failureModes", failureModes}
        };
    } */
};
