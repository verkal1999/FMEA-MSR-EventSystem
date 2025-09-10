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

#include "PLCMonitor.h"   // für PLCMonitor::InventoryRow
#include "Plan.h"         // für Plan

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

    // ---- kleine Hilfstypen ---------------------------------------------------
    struct NodeKey {
        uint16_t ns = 4;
        char     type = 's';    // 's'|'i'|'g'|'b'
        std::string id;         // z.B. "OPCUA.bool1"
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
        std::unordered_map<NodeKey, bool, NodeKeyHash> bools; // später ints/strings ergänzen
    };

    enum class KgValKind { Bool, Number, String };
    struct KgExpect {
        NodeKey   key;
        KgValKind kind{KgValKind::Bool};
        bool      expectedBool{false}; // vorerst nur Bool
    };

    struct ComparisonItem {
        NodeKey key;
        bool    ok{false};
        std::string detail; // "ist=true, soll=false" etc.
    };
    struct ComparisonReport {
        bool allOk{true};
        std::vector<ComparisonItem> items;
    };

private:
    PLCMonitor& mon_;
    EventBus&   bus_;

    // --- Correlation Guard
    std::mutex corrMx_;
    std::unordered_set<std::string> corrDone_;
    bool markDone_(const std::string& id) {
        std::lock_guard<std::mutex> lk(corrMx_);
        return corrDone_.insert(id).second;
    }
    bool isDone_(const std::string& id) {
        std::lock_guard<std::mutex> lk(corrMx_);
        return corrDone_.count(id) != 0;
    }

    // --- Logger intern
    std::atomic<int> logLevel_{ static_cast<int>(LogLevel::Info) };
    bool isEnabled(LogLevel lvl) const {
        return static_cast<int>(lvl) <= logLevel_.load(std::memory_order_relaxed);
    }
    const char* toCStr(LogLevel lvl) const;
    std::ostream& log(LogLevel lvl) const; // liefert std::cout (mit Prefix) oder Null-Stream

    // 1) Inventar + Cache
    InventorySnapshot buildInventorySnapshot(const std::string& root);

    // 2) KG normalisieren
    std::vector<KgExpect> normalizeKgResponse(const std::string& srows);

    // 3) Vergleich KG vs. Inventar-Cache
    ComparisonReport compareAgainstCache(const InventorySnapshot& inv,
                                         const std::vector<KgExpect>& ex);

    // 4) Plan bauen & ausführen
    Plan buildPlanFromComparison(const std::string& corr,
                                 const ComparisonReport& rep);
    void executePlanAndAck(const Plan& plan, bool checksOk);

    // 5) Logging/Dumps
    void logBoolInventory(const InventorySnapshot& inv) const;

    // 6) NodeId-Parser auf Member-Typ
    bool parseNodeId(const std::string& full, NodeKey& out) const;
};
