#include "MonActionForce.h"
#include "PlanJsonUtils.h"
#include "PLCMonitor.h"
#include "EventBus.h"
#include "Event.h"
#include "Acks.h"
#include "Plan.h"
#include "common_types.h"   // UAValue, UAValueMap, tagOf, equalUA
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <chrono>

MonitoringActionForce::MonitoringActionForce(PLCMonitor& mon, EventBus& bus,
                                             Fetcher fetch,
                                             unsigned defaultTimeoutMs)
: mon_(mon), bus_(bus), fetch_(std::move(fetch)), defTimeoutMs_(defaultTimeoutMs) {}

// JSON -> Plan (nur CallMethod, KEIN DiagnoseFinished-Puls)
Plan MonitoringActionForce::buildPlanFromPayload(const std::string& corr,
                                                 const std::string& payload)
{
    Plan plan; plan.correlationId = corr; plan.resourceId = "Station";

    // IRI-Kopf überspringen, ab erstem '{' parsen
    std::string::size_type nl = payload.find('\n');
    const std::size_t brace   = payload.find('{', (nl == std::string::npos) ? 0 : nl);
    if (brace == std::string::npos) return plan;
    std::string js = payload.substr(brace);

    nlohmann::json j;
    try { j = nlohmann::json::parse(js); }
    catch (...) {
        std::string fixed = fixParamsRawIfNeeded(js);
        try { j = nlohmann::json::parse(fixed); }
        catch (...) { return plan; }
    }

    nlohmann::json rows = nlohmann::json::array();
    if (j.is_object() && j.contains("rows") && j["rows"].is_array()) {
        rows = j["rows"];
    } else if (j.is_array()) {
        rows = j;
    } else if (j.is_object() && j.contains("monReactions") && j["monReactions"].is_array()
               && !j["monReactions"].empty() && j["monReactions"][0].is_object()
               && j["monReactions"][0].contains("rows")) {
        rows = j["monReactions"][0]["rows"];
    }

    std::map<int, Operation> opsByStep;
    auto getOp = [&](int step) -> Operation& {
        auto& op = opsByStep[step];
        op.type = OpType::CallMethod;
        return op;
    };

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (!r.is_object()) continue;

        const int         step = r.value("step", 0);
        const std::string g    = r.value("g", "");
        const std::string k    = r.value("k", "");
        const std::string t    = r.value("t", "");
        const int         idx  = r.value("i", 0);

        if (g == "meta"   && k == "jobId")    { if (r.contains("v") && r["v"].is_string()) getOp(step).callObjNodeId = r["v"].get<std::string>(); }
        else if (g=="method" && k=="methodId"){ if (r.contains("v") && r["v"].is_string()) getOp(step).callMethNodeId = r["v"].get<std::string>(); }
        else if (g=="meta"   && k=="timeoutMs") {
            if (r.contains("v")) {
                if (r["v"].is_number_integer())        getOp(step).timeoutMs = r["v"].get<int>();
                else if (r["v"].is_string()) { try {   getOp(step).timeoutMs = std::stoi(r["v"].get<std::string>()); } catch (...) {} }
            }
        } else if (g == "input")  { if (r.contains("v")) assignTyped(getOp(step).inputs,  idx, t, r["v"]); }
          else if (g == "output") { if (r.contains("v")) assignTyped(getOp(step).expOuts, idx, t, r["v"]); }
    }

    for (auto &kv : opsByStep) {
        Operation& op = kv.second;
        if (op.callObjNodeId.empty() || op.callMethNodeId.empty()) continue;
        plan.ops.push_back(std::move(op));
    }
    return plan;
}

std::vector<std::string>
MonitoringActionForce::filter(const std::vector<std::string>& winners,
                              const std::string& corr,
                              const std::string& processNameForAck)
{
    using Clock = std::chrono::steady_clock;

    // Ack: PLANNED
    bus_.post(Event{
        EventType::evReactionPlanned, Clock::now(),
        std::any{ ReactionPlannedAck{ corr, "Station", "MonitoringAction Filter (CallMethod)" } }
    });

    std::vector<std::string> kept;
    kept.reserve(winners.size());

    for (const auto& fm : winners) {
        // 1) MonAction aus KG holen
        const std::string payload = fetch_(fm);
        if (payload.empty()) { kept.push_back(fm); continue; }

        // 2) Plan bauen (nur CallMethod)
        Plan monPlan = buildPlanFromPayload(corr, payload);

        // 3) ausführen + Outputs prüfen
        bool allOk = true;
        for (size_t i = 0; i < monPlan.ops.size(); ++i) {
            const auto& op = monPlan.ops[i];
            if (op.type != OpType::CallMethod) continue;

            const unsigned to = (op.timeoutMs > 0) ? (unsigned)op.timeoutMs : defTimeoutMs_;

            std::cout << "[MonAct] step#" << i
                      << " obj='"  << op.callObjNodeId
                      << "' meth='"<< op.callMethNodeId
                      << "' inputs=" << uaMapToJson(op.inputs).dump()
                      << " timeout=" << to << "ms\n";

            UAValueMap got;
            const bool callOk = mon_.callMethodTyped(op.callObjNodeId, op.callMethNodeId,
                                                     op.inputs, got, to);

            bool match = true;
            if (!op.expOuts.empty()) {
                for (const auto& [k, vexp] : op.expOuts) {
                    auto it = got.find(k);
                    if (it == got.end() || !::equalUA(vexp, it->second)) { match = false; break; }
                }
                std::cout << "[MonAct]   exp=" << uaMapToJson(op.expOuts).dump()
                          << " got=" << uaMapToJson(got).dump()
                          << " -> " << (match ? "MATCH" : "DIFF") << "\n";
            }

            allOk = allOk && callOk && match;
        }

        if (allOk) kept.push_back(fm);
        else {
            bus_.post(Event{
                EventType::evProcessFail, Clock::now(),
                std::any{ ProcessFailAck{
                    corr, processNameForAck,
                    std::string("MonitoringAction mismatch for FM: ") + fm
                } }
            });
        }
    }

    // Ack: DONE
    bus_.post(Event{
        EventType::evReactionDone, Clock::now(),
        std::any{ ReactionDoneAck{
            corr,
            kept.empty() ? 0 : 1,
            kept.empty() ? "NO CANDIDATE PASSED" : "OK"
        } }
    });

    return kept;
}