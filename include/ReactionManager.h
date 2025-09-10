#pragma once
#include "ReactiveObserver.h"
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>
#include <atomic>
#include <ostream>
#include <nlohmann/json.hpp>

#include "PLCMonitor.h"
#include "Plan.h"

class Event;
class EventBus;

class ReactionManager : public ReactiveObserver {
public:
    enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4, Verbose = 5 };

    ReactionManager(PLCMonitor& mon, EventBus& bus);
    void onMethod(const Event& ev) override;

    // Logging-API
    void setLogLevel(LogLevel lvl) { logLevel_.store(static_cast<int>(lvl), std::memory_order_relaxed); }
    LogLevel getLogLevel() const   { return static_cast<LogLevel>(logLevel_.load(std::memory_order_relaxed)); }

    // ---- Hilfstypen ----------------------------------------------------------
    struct NodeKey {
        uint16_t    ns   = 4;
        char        type = 's';          // 's'|'i'|'g'|'b'
        std::string id;                  // e.g. "OPCUA.bool1"
        bool operator==(const NodeKey& o) const {
            return ns == o.ns && type == o.type && id == o.id;
        }
    };
    struct NodeKeyHash {
        size_t operator()(const NodeKey& k) const noexcept {
            return std::hash<uint16_t>{}(k.ns)
                 ^ (std::hash<char>{}(k.type) << 1)
                 ^ (std::hash<std::string>{}(k.id) << 2);
        }
    };

    struct InventorySnapshot {
        std::vector<PLCMonitor::InventoryRow> rows;
        std::unordered_map<NodeKey, bool,        NodeKeyHash> bools;
        std::unordered_map<NodeKey, std::string, NodeKeyHash> strings;
        std::unordered_map<NodeKey, int16_t,     NodeKeyHash> int16s;   // NEU
        std::unordered_map<NodeKey, double,      NodeKeyHash> floats;   // NEU (Float & Double)
    };

    enum class KgValKind { Bool, Int16, Float64, String };

    struct KgExpect {
        NodeKey   key;
        KgValKind kind{KgValKind::Bool};
        // erwartete Werte (nur der zum kind passende wird genutzt)
        bool      expectedBool{false};
        int16_t   expectedI16{0};
        double    expectedF64{0.0};
        std::string expectedStr; // derzeit ungenutzt im Vergleich
    };

    struct ComparisonItem {
        NodeKey key;
        bool    ok{false};
        std::string detail;
    };
    struct ComparisonReport {
        bool allOk{true};
        std::vector<ComparisonItem> items;
    };

    // KG liefert (optional) potFM + FMParam
    struct KgCandidate {
        std::string potFM;
        std::vector<KgExpect> expects;
    };

    bool isEnabled(LogLevel lvl) const {
    return static_cast<int>(lvl) <= logLevel_.load(std::memory_order_relaxed);
    };

private:
    PLCMonitor& mon_;
    EventBus&   bus_;

    // --- Correlation Guard
    std::mutex corrMx_;
    std::unordered_set<std::string> corrDone_;
    bool markDone_(const std::string& id);
    bool isDone_(const std::string& id);

    // --- Logger intern
    std::atomic<int> logLevel_{ static_cast<int>(LogLevel::Info) };
    const char*    toCStr(LogLevel lvl) const;
    std::ostream&  log(LogLevel lvl) const;

    // 1) Inventar + Cache
    InventorySnapshot buildInventorySnapshot(const std::string& root);

    // 2) KG normalisieren
    std::vector<KgExpect>     normalizeKgResponse(const std::string& srows);
    std::vector<KgCandidate>  normalizeKgPotFM   (const std::string& srows);

    // 3) Vergleich KG vs. Inventar-Cache
    ComparisonReport compareAgainstCache(const InventorySnapshot& inv,
                                         const std::vector<KgExpect>& ex);
    std::vector<std::string> selectPotFMByChecks(const InventorySnapshot& inv,
                                                 const std::vector<KgCandidate>& cands,
                                                 std::vector<ComparisonReport>* perCandidateReports = nullptr);

    // 4) Plan bauen & ausführen
    Plan  buildPlanFromComparison(const std::string& corr, const ComparisonReport& rep);
    void  executePlanAndAck(const Plan& plan, bool checksOk);

    // 5) Logging/Dumps
    void  logBoolInventory(const InventorySnapshot& inv) const; // bleibt
    void  logInventoryVariables(const InventorySnapshot& inv) const; // alle Variablen + Typ + (cached value)

    // 6) NodeId/Cache-Helfer
    bool  parseNodeId(const std::string& full, NodeKey& out) const;

    // String-Cache helper (für lastExecutedSkill)
    std::string getStringFromCache(const InventorySnapshot& inv,
                                   UA_UInt16 ns, const std::string& id, bool* found = nullptr) const;
    std::string getLastExecutedSkill(const InventorySnapshot& inv) const;

    // --- Systemreaction (aus KG) -> Plan/Execution ---
    std::string fetchSystemReactionForFM(const std::string& fmIri); // Python call
    Plan        createPlanFromSystemReactionJson(const std::string& corrId,
                                                const std::string& payloadJson);
    void        executeMethodPlanAndAck(const Plan& plan);
};


