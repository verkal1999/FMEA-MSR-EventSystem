#include <iostream>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <sstream>
#include <type_traits>
#include <pybind11/embed.h>
#include "PythonWorker.h"
// ReactionManager.cpp (bei den Includes)
#include <optional>
#include "ReactionManager.h"
#include "EventBus.h"
#include "Event.h"
#include "Acks.h"
#include "Correlation.h"
#include "CommandForce.h"
#include "CommandForceFactory.h"
#include "PLCMonitor.h"
#include "Plan.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include "MonActionForce.h"
#include "PlanJsonUtils.h"

namespace py = pybind11;
using json = nlohmann::json;

// ---------- Null-Stream für stummgeschaltete Logs ----------------------------
struct RM_NullBuf : std::streambuf { int overflow(int c) override { return c; } };
static RM_NullBuf   rm_nullbuf;
static std::ostream rm_nullout(&rm_nullbuf);

// ---------- Kleine Utils für Logs --------------------------------------------
static std::string truncStr(const std::string& s, size_t n=200) {
    if (s.size() <= n) return s;
    return s.substr(0, n) + "...(" + std::to_string(s.size()) + "B)";
}
static std::string keyStr(const ReactionManager::NodeKey& k) {
    std::ostringstream oss;
    oss << "ns=" << k.ns << ";" << k.type << "=" << k.id;
    return oss.str();
}

// ---------- Helfer in anonymem Namensraum (kollisionsfrei) -------------------
namespace {

    // Vorwärtsdeklaration, damit IntelliSense die Signatur kennt, bevor sie benutzt wird.
    static bool rm_parseNsAndId(const std::string& full,
                                UA_UInt16& nsOut,
                                std::string& idStrOut,
                                char& idTypeOut);

    // robustes Float-Gleichheitkriterium ohne initializer_list
    static bool nearlyEqual(double a, double b,
                            double absTol = 1e-6,
                            double relTol = 1e-6) {
        const double diff  = std::fabs(a - b);
        const double aa    = std::fabs(a);
        const double bb    = std::fabs(b);
        double scale = 1.0;
        if (aa > scale) scale = aa;
        if (bb > scale) scale = bb;
        return diff <= absTol + relTol * scale;
    }

    static nlohmann::json parseMaybeDoubleEncoded(const nlohmann::json& j, const char* /*where*/) {
        if (j.is_string()) {
            try { return nlohmann::json::parse(j.get<std::string>()); } catch (...) {}
        }
        return j;
    }

    static bool coerceRowObject(const nlohmann::json& in, nlohmann::json& out) {
        if (in.is_object()) { out = in; return true; }
        if (in.is_string()) {
            try {
                auto j2 = nlohmann::json::parse(in.get<std::string>());
                if (j2.is_object()) {
                    if (j2.contains("rows") && j2["rows"].is_array() && !j2["rows"].empty() && j2["rows"][0].is_object()) { out=j2["rows"][0]; return true; }
                    out=j2; return true;
                }
                if (j2.is_array() && !j2.empty() && j2[0].is_object()) { out=j2[0]; return true; }
            } catch (...) {}
        }
        return false;
    }

    // Implementierung von rm_parseNsAndId (früher parseNsAndId)
    static bool rm_parseNsAndId(const std::string& full,
                                UA_UInt16& nsOut,
                                std::string& idStrOut,
                                char& idTypeOut)
    {
        nsOut = 4;
        idStrOut.clear();
        idTypeOut = '?';

        // Kurzform
        if (full.rfind("OPCUA.", 0) == 0) {
            idTypeOut = 's';
            idStrOut  = full;
            return true;
        }
        // Langform "ns=<n>;<t>=<id>"
        if (full.rfind("ns=", 0) != 0) return false;

        const size_t semi = full.find(';');
        if (semi == std::string::npos) return false;

        try {
            nsOut = static_cast<UA_UInt16>(std::stoi(full.substr(3, semi - 3)));
        } catch (...) {
            nsOut = 4;
        }

        if (semi + 2 >= full.size()) return false;
        idTypeOut = full[semi + 1];
        if (full[semi + 2] != '=') return false;

        idStrOut = full.substr(semi + 3);
        return true;
    }
} // namespace

// ---------- ReactionManager ---------------------------------------------------
ReactionManager::ReactionManager(PLCMonitor& mon, EventBus& bus) : mon_(mon), bus_(bus) {
    worker_ = std::jthread([this](std::stop_token st){
        for (;;) {
            std::function<void(std::stop_token)> job;
            {
                std::unique_lock<std::mutex> lk(job_mx_);
                job_cv_.wait(lk, [&]{ return st.stop_requested() || !jobs_.empty(); });
                if (st.stop_requested() && jobs_.empty()) break;
                job = std::move(jobs_.front());
                jobs_.pop();
            }
            try { job(st); } catch (const std::exception& e) {
                log(LogLevel::Warn) << "[worker] job failed: " << e.what() << "\n";
            }
        }
    });
}

const char* ReactionManager::toCStr(LogLevel lvl) const {
    switch (lvl) {
        case LogLevel::Error:   return "ERR";
        case LogLevel::Warn:    return "WRN";
        case LogLevel::Info:    return "INF";
        case LogLevel::Debug:   return "DBG";
        case LogLevel::Trace:   return "TRC";
        case LogLevel::Verbose: return "VRB";
    }
    return "?";
}
std::ostream& ReactionManager::log(LogLevel lvl) const {
    if (isEnabled(lvl)) {
        std::cout << "[RM][" << toCStr(lvl) << "] ";
        return std::cout;
    }
    return rm_nullout;
}

bool ReactionManager::parseNodeId(const std::string& full, NodeKey& out) const {
    UA_UInt16 ns = 4; std::string id; char type='?';
    const bool ok = rm_parseNsAndId(full, ns, id, type);
    log(ok ? LogLevel::Debug : LogLevel::Warn)
        << "parseNodeId in='" << full << "' -> "
        << (ok ? "OK " : "FAIL ")
        << "ns=" << ns << " type='" << type << "' id='" << id << "'\n";
    if (!ok) return false;
    out.ns = ns; out.type = type; out.id = id;
    return true;
}

ReactionManager::InventorySnapshot
ReactionManager::buildInventorySnapshot(const std::string& root) {
    log(LogLevel::Info) << "buildInventorySnapshot ENTER root='" << root << "'\n";

    InventorySnapshot s;
    const bool dumped = mon_.dumpPlcInventory(s.rows, root.c_str());
    log(LogLevel::Info) << "dumpPlcInventory=" << (dumped?"OK":"FAIL")
                        << " rows=" << s.rows.size() << "\n";

    size_t boolVars = 0, boolReadOk = 0;
    size_t strVars  = 0, strReadOk  = 0;
    size_t i16Vars  = 0, i16ReadOk  = 0;
    size_t fVars    = 0, fReadOk    = 0;

    for (const auto& ir : s.rows) {
        if (ir.nodeClass != "Variable") continue;

        const bool isBool   = (ir.dtypeOrSig.find("Boolean") != std::string::npos);
        const bool isString = (ir.dtypeOrSig.find("String")  != std::string::npos) ||
                              (ir.dtypeOrSig.find("STRING")  != std::string::npos);
        const bool isI16    = (ir.dtypeOrSig.find("Int16")   != std::string::npos);
        const bool isF64    = (ir.dtypeOrSig.find("Double")  != std::string::npos);
        const bool isF32    = (ir.dtypeOrSig.find("Float")   != std::string::npos);

        ReactionManager::NodeKey k;
        if (!parseNodeId(ir.nodeId, k)) {
            log(LogLevel::Warn) << "[inv] parse FAIL nodeId=" << ir.nodeId << "\n";
            continue;
        }
        if (k.type != 's') {
            log(LogLevel::Trace) << "[inv] skip non-'s' idType for " << keyStr(k) << "\n";
            continue;
        }

        if (isBool) {
            ++boolVars;
            bool v=false;
            const bool ok = mon_.readBoolAt(k.id, k.ns, v);
            log(ok ? LogLevel::Debug : LogLevel::Warn)
                << "[inv] read BOOL " << keyStr(k) << " -> "
                << (ok ? (v?"true":"false") : "<read failed>") << "\n";
            if (ok) { s.bools.emplace(k, v); ++boolReadOk; }
        } else if (isString) {
            ++strVars;
            std::string sv;
            const bool ok = mon_.readStringAt(k.id, k.ns, sv);
            log(ok ? LogLevel::Debug : LogLevel::Warn)
                << "[inv] read STR  " << keyStr(k) << " -> "
                << (ok ? ("\"" + truncStr(sv,120) + "\"") : "<read failed>") << "\n";
            if (ok) { s.strings.emplace(k, std::move(sv)); ++strReadOk; }
        } else if (isI16) {
            ++i16Vars;
            UA_Int16 v = 0;
            const bool ok = mon_.readInt16At(k.id, k.ns, v);
            log(ok ? LogLevel::Debug : LogLevel::Warn)
                << "[inv] read I16  " << keyStr(k) << " -> "
                << (ok ? std::to_string((int)v) : "<read failed>") << "\n";
            if (ok) { s.int16s.emplace(k, static_cast<int16_t>(v)); ++i16ReadOk; }
        } else if (isF64 || isF32) {
            ++fVars;
            bool ok = false;
            double dv = 0.0;
            if (isF64) {
                UA_Double tmp = 0.0;
                ok = mon_.readDoubleAt(k.id, k.ns, tmp);
                dv = tmp;
            } else {
                UA_Float tmp = 0.0f;
                ok = mon_.readFloatAt(k.id, k.ns, tmp);
                dv = static_cast<double>(tmp);
            }
            log(ok ? LogLevel::Debug : LogLevel::Warn)
                << "[inv] read Fxx  " << keyStr(k) << " -> "
                << (ok ? std::to_string(dv) : "<read failed>") << "\n";
            if (ok) { s.floats.emplace(k, dv); ++fReadOk; }
        } else {
            log(LogLevel::Trace) << "[inv] known var but not cached type: "
                                 << ir.dtypeOrSig << " @ " << keyStr(k) << "\n";
        }
    }

    log(LogLevel::Info) << "buildInventorySnapshot"
                        << " BOOL vars=" << boolVars << " cached=" << boolReadOk
                        << " | STR vars="  << strVars  << " cached=" << strReadOk
                        << " | I16 vars="  << i16Vars  << " cached=" << i16ReadOk
                        << " | FP  vars="  << fVars    << " cached=" << fReadOk
                        << "\n";
    log(LogLevel::Info) << "buildInventorySnapshot EXIT\n";
    return s;
}
void ReactionManager::logInventoryVariables(const InventorySnapshot& inv) const {
    log(LogLevel::Info) << "logInventoryVariables ENTER rows=" << inv.rows.size() << "\n";
    std::cout << "[Inventory] Variablen + Typen (mit Cache-Werten, falls vorhanden):\n";
    for (const auto& ir : inv.rows) {
        if (ir.nodeClass != "Variable") continue;

        NodeKey k;
        if (!parseNodeId(ir.nodeId, k)) {
            std::cout << "  " << ir.nodeId << " | " << ir.dtypeOrSig << " | <parse fail>\n";
            continue;
        }

        std::string val = "-";
        if (auto itB = inv.bools.find(k); itB != inv.bools.end()) {
            val = itB->second ? "true" : "false";
        } else if (auto itS = inv.strings.find(k); itS != inv.strings.end()) {
            val = "\"" + truncStr(itS->second, 120) + "\"";
        } else if (auto itI = inv.int16s.find(k); itI != inv.int16s.end()) {
            val = std::to_string(itI->second);
        } else if (auto itF = inv.floats.find(k); itF != inv.floats.end()) {
            val = std::to_string(itF->second);
        }

        std::cout << "  " << keyStr(k) << " | " << ir.dtypeOrSig << " | " << val << "\n";
    }
    log(LogLevel::Info) << "logInventoryVariables EXIT\n\n";
}
std::string ReactionManager::getStringFromCache(const InventorySnapshot& inv,
                                                UA_UInt16 ns, const std::string& id,
                                                bool* found) const
{
    NodeKey k; k.ns = ns; k.type='s'; k.id = id;
    auto it = inv.strings.find(k);
    if (found) *found = (it != inv.strings.end());
    return (it != inv.strings.end()) ? it->second : std::string{};
}

std::string ReactionManager::getLastExecutedSkill(const InventorySnapshot& inv) const {
    bool had=false;
    auto s = getStringFromCache(inv, /*ns*/4, "OPCUA.lastExecutedSkill", &had);
    if (had) {
        log(LogLevel::Info) << "[skill] from cache: \"" << truncStr(s,120) << "\"\n";
        return s;
    }
    // Fallback: einmal live lesen (falls du das möchtest)
    std::string live;
    const bool ok = mon_.readStringAt("OPCUA.lastExecutedSkill", 4, live);
    log(ok ? LogLevel::Info : LogLevel::Warn)
        << "[skill] live read " << (ok ? "OK \"" + truncStr(live,120) + "\"" : "FAIL") << "\n";
    return ok ? live : std::string{};
}

void ReactionManager::logBoolInventory(const InventorySnapshot& inv) const {
    log(LogLevel::Info) << "logBoolInventory ENTER count=" << inv.bools.size() << "\n";
    std::cout << "[Inventory] Aktuelle BOOL-Werte:\n";
    for (const auto& it : inv.bools) {
        const auto& k = it.first;
        std::cout << "  " << keyStr(k) << " = " << (it.second ? "true" : "false") << "\n";
    }
    log(LogLevel::Info) << "logBoolInventory EXIT\n\n";
}

std::vector<ReactionManager::KgExpect>
ReactionManager::normalizeKgResponse(const std::string& srows) {
    log(LogLevel::Info) << "normalizeKgResponse ENTER len=" << srows.size()
                        << " preview=\"" << truncStr(srows) << "\"\n";

    std::vector<KgExpect> out;

    json j;
    try { j = json::parse(srows); }
    catch (const std::exception& e) {
        log(LogLevel::Warn) << "normalizeKgResponse top parse FAIL: " << e.what() << "\n";
        j = json::object();
    }

    j = parseMaybeDoubleEncoded(j, "top");
    log(LogLevel::Debug) << "normalizeKgResponse top type=" << j.type_name() << "\n";

    json rows = json::array();
    if (j.contains("rows") && j["rows"].is_array()) {
        rows = j["rows"];
        log(LogLevel::Debug) << "rows[] found size=" << rows.size() << "\n";
    } else if (j.is_array()) {
        rows = j;
        log(LogLevel::Debug) << "top is array size=" << rows.size() << "\n";
    } else {
        rows = json::array({ j });
        log(LogLevel::Debug) << "single row promoted\n";
    }

    size_t added = 0, seen = 0;
    for (const auto& rowIn : rows) {
        ++seen;
        json row;
        const bool okC = coerceRowObject(rowIn, row);
        log(LogLevel::Trace) << "[row#" << seen << "] coerce=" << (okC?"OK":"FAIL")
                             << " type=" << row.type_name() << "\n";
        if (!okC) continue;

        std::string idFull = row.value("id", "");
        std::string t      = row.value("t", "");
        const bool hasV    = row.contains("v");
        log(LogLevel::Debug) << "[row#" << seen << "] id='" << truncStr(idFull,120)
                             << "' t='" << t << "' v=" << (hasV?row["v"].dump():"<none>") << "\n";

        // id könnte selbst JSON enthalten
        if (!idFull.empty() && idFull.front()=='{') {
            json idj = parseMaybeDoubleEncoded(json(idFull), "row.id");
            log(LogLevel::Trace) << "[row#" << seen << "] id unwrap type=" << idj.type_name() << "\n";
            if (idj.is_object()) {
                if (idj.contains("rows") && idj["rows"].is_array() && !idj["rows"].empty() && idj["rows"][0].is_object())
                    row = idj["rows"][0];
                else
                    row = idj;
                idFull = row.value("id", "");
                t      = row.value("t", t);
            }
        }

        if (idFull.empty() || t.empty() || !row.contains("v")) {
            log(LogLevel::Warn) << "[row#" << seen << "] skip: missing id/t/v\n";
            continue;
        }

        NodeKey key;
        if (!parseNodeId(idFull, key)) {
            log(LogLevel::Warn) << "[row#" << seen << "] skip: parseNodeId FAIL\n";
            continue;
        }

        std::string tl = t;
        std::transform(tl.begin(), tl.end(), tl.begin(), [](unsigned char c){ return std::tolower(c); });

        if (tl == "bool" || tl == "boolean") {
            if (!row["v"].is_boolean()) {
                log(LogLevel::Warn) << "[row#" << seen << "] skip: v not boolean\n";
                continue;
            }
            const bool exp = row["v"].get<bool>();
            KgExpect e; e.key = key; e.kind = KgValKind::Bool; e.expectedBool = exp;
            out.push_back(e);
            ++added;
            log(LogLevel::Debug) << "[row#" << seen << "] added BOOL expect "
                                 << keyStr(key) << " = " << (exp?"true":"false") << "\n";

        } else if (tl == "int" || tl == "int16") {
            // v: Zahl oder String mit Zahl
            int v = 0; bool okV = false;
            if (row["v"].is_number_integer()) { v = row["v"].get<int>(); okV = true; }
            else if (row["v"].is_string()) { try { v = std::stoi(row["v"].get<std::string>()); okV = true; } catch(...){} }
            if (!okV) {
                log(LogLevel::Warn) << "[row#" << seen << "] skip: v not int/int16\n";
                continue;
            }
            KgExpect e; e.key = key; e.kind = KgValKind::Int16; e.expectedI16 = static_cast<int16_t>(v);
            out.push_back(e);
            ++added;
            log(LogLevel::Debug) << "[row#" << seen << "] added I16 expect "
                                 << keyStr(key) << " = " << e.expectedI16 << "\n";

        } else if (tl == "double" || tl == "float" || tl == "number") {
            // v: Zahl oder String mit Zahl
            double dv = 0.0; bool okV = false;
            if (row["v"].is_number()) { dv = row["v"].get<double>(); okV = true; }
            else if (row["v"].is_string()) { try { dv = std::stod(row["v"].get<std::string>()); okV = true; } catch(...){} }
            if (!okV) {
                log(LogLevel::Warn) << "[row#" << seen << "] skip: v not float/double\n";
                continue;
            }
            KgExpect e; e.key = key; e.kind = KgValKind::Float64; e.expectedF64 = dv;
            out.push_back(e);
            ++added;
            log(LogLevel::Debug) << "[row#" << seen << "] added FP expect "
                                 << keyStr(key) << " = " << dv << "\n";
        } else {
            log(LogLevel::Trace) << "[row#" << seen << "] skip: unsupported t='" << t
                                 << "' (supported: bool/int16/float/double/number)\n";
        }
    }

    log(LogLevel::Info) << "normalizeKgResponse EXIT expects=" << added << "\n";
    return out;
}

ReactionManager::ComparisonReport
ReactionManager::compareAgainstCache(const InventorySnapshot& inv,
                                     const std::vector<KgExpect>& ex) {
    log(LogLevel::Info) << "compareAgainstCache ENTER inv.bools=" << inv.bools.size()
                        << " expects=" << ex.size() << "\n";
    ComparisonReport rep;

    size_t idx = 0;
    for (const auto& e : ex) {
        ++idx;
        if (e.kind == KgValKind::Bool) {
            auto it = inv.bools.find(e.key);
            if (it == inv.bools.end()) {
                rep.items.push_back({ e.key, false, "kein Cachewert vorhanden" });
                rep.allOk = false;
                log(LogLevel::Warn) << "[cmp#" << idx << "] " << keyStr(e.key) << " -> NO CACHE\n";
                continue;
            }
            const bool cur = it->second;
            const bool ok  = (cur == e.expectedBool);
            rep.items.push_back({
                e.key, ok,
                std::string("ist=") + (cur ? "true" : "false")
                + ", soll=" + (e.expectedBool ? "true" : "false")
            });
            if (!ok) rep.allOk = false;

            log(LogLevel::Debug) << "[cmp#" << idx << "] " << keyStr(e.key)
                                 << " ist=" << (cur?"true":"false")
                                 << " soll=" << (e.expectedBool?"true":"false")
                                 << " -> " << (ok?"MATCH":"DIFF") << "\n";
        }
        else if (e.kind == KgValKind::Int16) {
            auto it = inv.int16s.find(e.key);
            if (it == inv.int16s.end()) {
                rep.items.push_back({ e.key, false, "kein Cachewert (Int16) vorhanden" });
                rep.allOk = false;
                log(LogLevel::Warn) << "[cmp#" << idx << "] " << keyStr(e.key) << " -> NO CACHE (I16)\n";
                continue;
            }
            const int16_t cur = it->second;
            const bool ok = (cur == e.expectedI16);
            rep.items.push_back({
                e.key, ok,
                std::string("ist=") + std::to_string(cur) +
                ", soll=" + std::to_string(e.expectedI16)
            });
            if (!ok) rep.allOk = false;
            log(LogLevel::Debug) << "[cmp#" << idx << "] " << keyStr(e.key)
                                 << " ist=" << cur
                                 << " soll=" << e.expectedI16
                                 << " -> " << (ok?"MATCH":"DIFF") << "\n";
        }
        else if (e.kind == KgValKind::Float64) {
            auto it = inv.floats.find(e.key);
            if (it == inv.floats.end()) {
                rep.items.push_back({ e.key, false, "kein Cachewert (Float/Double) vorhanden" });
                rep.allOk = false;
                log(LogLevel::Warn) << "[cmp#" << idx << "] " << keyStr(e.key) << " -> NO CACHE (FP)\n";
                continue;
            }
            const double cur = it->second;
            const bool ok = nearlyEqual(cur, e.expectedF64, 1e-6, 1e-6);
            rep.items.push_back({
                e.key, ok,
                std::string("ist=") + std::to_string(cur) +
                ", soll=" + std::to_string(e.expectedF64) +
                (ok ? "" : " (ε≈1e-6)")
            });
            if (!ok) rep.allOk = false;
            log(LogLevel::Debug) << "[cmp#" << idx << "] " << keyStr(e.key)
                                 << " ist=" << cur
                                 << " soll=" << e.expectedF64
                                 << " -> " << (ok?"MATCH":"DIFF") << "\n";
        }
    }

    log(LogLevel::Info) << "compareAgainstCache EXIT allOk=" << (rep.allOk?"true":"false")
                        << " items=" << rep.items.size() << "\n";
    return rep;
}

Plan ReactionManager::buildPlanFromComparison(const std::string& corr,
                                              const ComparisonReport& rep) {
    log(LogLevel::Info) << "buildPlanFromComparison ENTER corr=" << corr
                        << " checksOk=" << (rep.allOk?"true":"false")
                        << " items=" << rep.items.size() << "\n";

    Plan plan;
    plan.correlationId = corr;
    plan.resourceId    = "Station";
    {
    Operation pulse;
    pulse.type     = OpType::PulseBool;
    pulse.nodeId   = "OPCUA.DiagnoseFinished";
    pulse.ns       = 4;
    pulse.arg      = "true";      // "true" => Puls HIGH/LOW Sequenz wird intern gemacht
    pulse.timeoutMs= 100;         // Pulsbreite
    plan.ops.push_back(pulse);
    }

    log(LogLevel::Info) << "buildPlanFromComparison EXIT ops=" << plan.ops.size() << "\n";
    return plan;
}

std::vector<ReactionManager::KgCandidate>
ReactionManager::normalizeKgPotFM(const std::string& srows) {
    log(LogLevel::Info) << "normalizeKgPotFM ENTER len=" << srows.size()
                        << " preview=\"" << truncStr(srows) << "\"\n";

    std::vector<KgCandidate> out;

    json j;
    try { j = json::parse(srows); }
    catch (const std::exception& e) {
        log(LogLevel::Warn) << "normalizeKgPotFM top parse FAIL: " << e.what() << "\n";
        return out;
    }

    // rows[] extrahieren (analog zu normalizeKgResponse)
    json rows = json::array();
    if (j.contains("rows") && j["rows"].is_array()) {
        rows = j["rows"];
        log(LogLevel::Debug) << "normalizeKgPotFM rows[] size=" << rows.size() << "\n";
    } else if (j.is_array()) {
        rows = j;
        log(LogLevel::Debug) << "normalizeKgPotFM top is array size=" << rows.size() << "\n";
    } else if (j.is_object()) {
        rows = json::array({ j });
        log(LogLevel::Debug) << "normalizeKgPotFM single row promoted\n";
    } else {
        log(LogLevel::Warn) << "normalizeKgPotFM unsupported top type=" << j.type_name() << "\n";
        return out;
    }

    size_t idx = 0;
    for (const auto& row : rows) {
        ++idx;
        if (!row.is_object()) {
            log(LogLevel::Warn) << "[potFM#" << idx << "] skip: not object, type=" << row.type_name() << "\n";
            continue;
        }

        const bool hasPot = row.contains("potFM")   && row["potFM"].is_string();
        const bool hasPar = row.contains("FMParam") && row["FMParam"].is_string();
        if (!hasPot || !hasPar) {
            log(LogLevel::Warn) << "[potFM#" << idx << "] skip: missing potFM/FMParam string\n";
            continue;
        }

        const std::string pot  = row["potFM"].get<std::string>();
        const std::string fmp  = row["FMParam"].get<std::string>();
        log(LogLevel::Debug) << "[potFM#" << idx << "] potFM='" << truncStr(pot,160)
                             << "' FMParam.len=" << fmp.size() << "\n";

        // FMParam (String) kann selbst JSON sein -> Wiederverwendung der vorhandenen
        // normalizeKgResponse, die genau die "id/t/v"-Zeilen extrahiert.
        auto expects = normalizeKgResponse(fmp);

        if (expects.empty()) {
            log(LogLevel::Warn) << "[potFM#" << idx << "] no expects parsed from FMParam\n";
        } else {
            log(LogLevel::Debug) << "[potFM#" << idx << "] expects=" << expects.size() << "\n";
        }

        out.push_back(KgCandidate{ pot, std::move(expects) });
    }

    log(LogLevel::Info) << "normalizeKgPotFM EXIT candidates=" << out.size() << "\n";
    return out;
}
std::vector<std::string>
ReactionManager::selectPotFMByChecks(const InventorySnapshot& inv,
                                     const std::vector<KgCandidate>& cands,
                                     std::vector<ComparisonReport>* perReports) {
    log(LogLevel::Info) << "selectPotFMByChecks ENTER cands=" << cands.size() << "\n";

    std::vector<std::string> winners;
    if (perReports) perReports->clear();

    size_t idx = 0;
    for (const auto& c : cands) {
        ++idx;
        log(LogLevel::Debug) << "[sel#" << idx << "] potFM='" << truncStr(c.potFM,160)
                             << "' expects=" << c.expects.size() << "\n";

        auto rep = compareAgainstCache(inv, c.expects);
        if (perReports) perReports->push_back(rep);

        if (rep.allOk) {
            winners.push_back(c.potFM);
            log(LogLevel::Debug) << "[sel#" << idx << "] -> MATCH\n";
        } else {
            log(LogLevel::Trace) << "[sel#" << idx << "] -> no match (some DIFF)\n";
        }
    }

    log(LogLevel::Info) << "selectPotFMByChecks EXIT winners=" << winners.size() << "\n";
    return winners;
}

std::string ReactionManager::fetchSystemReactionForFM(const std::string& fmIri) {
    log(LogLevel::Info) << "[sysreact] fetchSystemReactionForFM ENTER fmIri=" << fmIri << "\n";
    std::string payload;

    try {
        payload = PythonWorker::instance().call([&]() -> std::string {
            py::module_ sys = py::module_::import("sys");
            py::list path = sys.attr("path").cast<py::list>();
            path.append(R"(C:\Users\Alexander Verkhov\OneDrive\Dokumente\MPA\Implementierung_MPA\Test\src)");

            py::module_ kg  = py::module_::import("KG_Interface");
            py::object cls  = kg.attr("KGInterface");
            py::object kgi  = cls();
            py::object res = kgi.attr("getSystemreactionForFailureMode")(fmIri.c_str());
            return std::string(py::str(res));
        });

        auto preview = payload.substr(0, std::min<size_t>(payload.size(), 300));
        log(LogLevel::Info)  << "[sysreact] payload len=" << payload.size() << " preview=\"" << preview << "\"\n";
        log(LogLevel::Debug) << "[sysreact] full payload:\n" << payload << "\n";
    } catch (const std::exception& e) {
        log(LogLevel::Warn) << "[sysreact] Python call failed: " << e.what() << "\n";
    }

    log(LogLevel::Info) << "[sysreact] fetchSystemReactionForFM EXIT\n";
    return payload;
}

void ReactionManager::createCommandForceForPlanAndAck(const Plan& plan,
                                                      bool checksOk,
                                                      const std::string& processNameForFail)
{
    using Clock = std::chrono::steady_clock;

    const bool hasCall =
        std::any_of(plan.ops.begin(), plan.ops.end(),
                    [](const Operation& op){ return op.type == OpType::CallMethod; });

    log(LogLevel::Info) << "createCommandForceForPlanAndAck ENTER corr=" << plan.correlationId
                        << " ops=" << plan.ops.size()
                        << " checksOk=" << (checksOk?"true":"false")
                        << " hasCall=" << (hasCall?"true":"false")
                        << "\n";

    // Ack: PLANNED
    bus_.post(Event{
        EventType::evReactionPlanned, Clock::now(),
        std::any{ ReactionPlannedAck{
            plan.correlationId, plan.resourceId,
            hasCall
              ? std::string("SystemReaction Plan (CallMethod)")
              : (std::string("KG Checks: ") + (checksOk ? "OK" : "FAIL") + " -> DiagnoseFinished-Puls")
        } }
    });
    log(LogLevel::Debug) << "posted evReactionPlanned\n";

    // Falls gar keine System-Reaktion (CallMethod) im Plan ist -> Prozessfehler melden
    if (!hasCall) {
        bus_.post(Event{
            EventType::evProcessFail, Clock::now(),
            std::any{ ProcessFailAck{
                plan.correlationId,
                processNameForFail,
                "No system reaction defined for this failure."
            } }
        });
        log(LogLevel::Warn) << "[exec] no CallMethod in plan -> posted evProcessFail\n";
    }

    bool allOk = true;

    for (size_t idx = 0; idx < plan.ops.size(); ++idx) {
        const auto& op = plan.ops[idx];

        if (op.type == OpType::CallMethod) {
            const unsigned to = (op.timeoutMs > 0 ? static_cast<unsigned>(op.timeoutMs) : 30000);

            log(LogLevel::Info) << "[exec] CallMethod step#" << idx
                                << " obj='"  << op.callObjNodeId
                                << "' meth='"<< op.callMethNodeId
                                << "' inputs=" << uaMapToJson(op.inputs).dump()
                                << " timeout=" << to << "ms\n";

            // 1) OPC UA Call (typisiert, mehrere Outputs möglich)
            UAValueMap got;
            const bool ok = mon_.callMethodTyped(op.callObjNodeId, op.callMethNodeId,
                                                 op.inputs, got, to);

            // 2) Soll/Ist-Vergleich (nur Keys aus expOuts müssen matchen)
            bool match = true;
            if (!op.expOuts.empty()) {
                for (const auto& [k, vexp] : op.expOuts) {
                    auto it = got.find(k);
                    if (it == got.end() || !::equalUA(vexp, it->second)) { match = false; break; }
                }
                log(LogLevel::Info) << "[exec]   outputs: expected=" << uaMapToJson(op.expOuts).dump()
                                    << " got=" << uaMapToJson(got).dump()
                                    << " -> " << (match ? "MATCH" : "DIFF") << "\n";
            } else {
                log(LogLevel::Info) << "[exec]   outputs: (no expected values in KG), got size="
                                    << got.size() << "\n";
            }

            // 3) evProcessFail bei Output-Differenz (mit Prozessname!)
            if (!match) {
                bus_.post(Event{
                    EventType::evProcessFail, Clock::now(),
                    std::any{ ProcessFailAck{
                        plan.correlationId,
                        processNameForFail,
                        std::string("Output mismatch at '") + op.callMethNodeId + "'"
                    } }
                });
                log(LogLevel::Debug) << "posted evProcessFail (output mismatch)\n";
            }

            allOk = allOk && ok && match;
            log(LogLevel::Info) << "[exec]   -> call=" << (ok ? "OK" : "FAIL")
                                << " combined=" << (ok && match ? "OK" : "FAIL") << "\n";
        }
        else if (op.type == OpType::PulseBool) {
            // Puls-Op über CommandForce ausführen (dein bestehender Weg)
            Plan pulsePlan;
            pulsePlan.correlationId = plan.correlationId;
            pulsePlan.resourceId    = plan.resourceId;
            pulsePlan.ops.push_back(op);

            log(LogLevel::Info) << "[exec] PulseBool step#" << idx
                                << " nodeId='" << op.nodeId
                                << "' ns=" << op.ns
                                << " value=" << op.arg
                                << " timeout=" << op.timeoutMs << "ms\n";

            auto cf = CommandForceFactory::create(CommandForceFactory::Kind::UseMonitor, mon_);
            const int rc = cf->execute(pulsePlan);            // Pulse/Writes etc.
            allOk = allOk && (rc != 0);                       // 1=OK/0=FAIL
            log(LogLevel::Info) << "[exec]   -> " << (rc ? "OK" : "FAIL") << "\n";
        }
        else {
            log(LogLevel::Debug) << "[exec] skip unsupported op type at idx=" << idx << "\n";
        }
    }

    // Ack: DONE
    bus_.post(Event{
        EventType::evReactionDone, Clock::now(),
        std::any{ ReactionDoneAck{
            plan.correlationId,
            allOk ? 1 : 0,
            allOk ? "OK" : "FAIL"
        } }
    });
    log(LogLevel::Debug) << "posted evReactionDone\n";
    log(LogLevel::Info)  << "createCommandForceForPlanAndAck EXIT\n";
}

//== Monitoring-Actions
std::string ReactionManager::fetchMonitoringActionForFM(const std::string& fmIri) {
    log(LogLevel::Info) << "[monact] fetchMonitoringActionForFM ENTER fmIri=" << fmIri << "\n";
    std::string payload;

    try {
        payload = PythonWorker::instance().call([&]() -> std::string {
            py::module_ sys = py::module_::import("sys");
            py::list path   = sys.attr("path").cast<py::list>();
            path.append(R"(C:\Users\Alexander Verkhov\OneDrive\Dokumente\MPA\Implementierung_MPA\Test\src)");

            py::module_ kg  = py::module_::import("KG_Interface");
            py::object cls  = kg.attr("KGInterface");
            py::object kgi  = cls(); // __init__ ohne Parameter
            py::object res  = kgi.attr("getMonitoringActionForFailureMode")(fmIri.c_str());
            return std::string(py::str(res));
        });

        auto preview = payload.substr(0, std::min<size_t>(payload.size(), 300));
        log(LogLevel::Info)  << "[monact] payload len=" << payload.size() << " preview=\"" << preview << "\"\n";
        log(LogLevel::Debug) << "[monact] full payload:\n" << payload << "\n";
    } catch (const std::exception& e) {
        log(LogLevel::Warn) << "[monact] Python call failed: " << e.what() << "\n";
    }

    log(LogLevel::Info) << "[monact] fetchMonitoringActionForFM EXIT\n";
    return payload;
}


void ReactionManager::onEvent(const Event& ev) {
    // Wir reagieren auf D1/D2/D3; andere Events ignorieren wir hier früh.
    const char* evName = nullptr;
    switch (ev.type) {
        case EventType::evD1: evName = "evD1"; break;
        case EventType::evD2: evName = "evD2"; break;
        case EventType::evD3: evName = "evD3"; break;
        default: return;
    }

    const auto corr = makeCorrelationId(evName);
    log(LogLevel::Info) << "onEvent ENTER " << evName << " corr=" << corr << "\n";

    // 1) Inventarsnapshot aufnehmen (inkl. Cache-Füllung/Logs) + Prozessname cachen
    auto inv = buildInventorySnapshot("PLC1");
    logInventoryVariables(inv);
    const std::string processName =
        getStringFromCache(inv, /*ns*/4, "OPCUA.lastExecutedProcess");

    // Für evD1 / evD3 aktuell nur Info-Log und raus (Worker nur für evD2)
    if (ev.type == EventType::evD1 || ev.type == EventType::evD3) {
        std::cout << "[RM][INF] received " << evName << " corr=" << corr << "\n";
        log(LogLevel::Info) << "onEvent EXIT " << evName << " corr=" << corr << " (no worker)\n";
        return;
    }

    // === Nur für evD2: Job joinbar über Worker-Thread ausführen (kein detach!) ===
    {
        std::lock_guard<std::mutex> lk(job_mx_);
        jobs_.push([this, corr, inv, evName, processName](std::stop_token st) mutable {
            log(LogLevel::Info) << "[worker] corr=" << corr << " (" << evName << ") START\n";

            // --- TIMER -----------------------------------------------------------
            using clock = std::chrono::steady_clock;
            const auto t0    = clock::now();
            auto       tlast = t0;
            auto lap = [&](const char* label) {
                const auto now   = clock::now();
                const auto step  = std::chrono::duration_cast<std::chrono::milliseconds>(now - tlast).count();
                const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
                log(LogLevel::Info) << "[timer] corr=" << corr
                                    << " " << label
                                    << " step="  << step  << " ms"
                                    << " total=" << total << " ms\n";
                tlast = now;
            };
            lap("job-start");
            // ---------------------------------------------------------------------

            // --- Python 1/3: potenzielle FailureModes ---------------------------
            std::string srows;
            try {
                log(LogLevel::Debug) << "[worker] calling PythonWorker.getFailureModeParameters\n";
                srows = PythonWorker::instance().call([&]() -> std::string {
                    namespace py = pybind11;
                    py::module_ sys  = py::module::import("sys");
                    py::list    path = sys.attr("path").cast<py::list>();
                    path.append(R"(C:\Users\Alexander Verkhov\OneDrive\Dokumente\MPA\Implementierung_MPA\Test\src)");

                    py::module_ kg  = py::module_::import("KG_Interface");
                    py::object cls  = kg.attr("KGInterface");
                    py::object kgi  = cls();
                    std::string interruptedSkill = getLastExecutedSkill(inv);
                    if (interruptedSkill.empty()) interruptedSkill = "MAIN.fbJob"; // Fallback
                    py::object pyres = kgi.attr("getFailureModeParameters")(interruptedSkill.c_str());
                    return std::string(py::str(pyres));
                });
                log(LogLevel::Info) << "[worker] KG.getFailureModeParameters OK json_len=" << srows.size()
                                    << " preview=\"" << truncStr(srows) << "\"\n";
            } catch (const std::exception& e) {
                log(LogLevel::Error) << "[worker] KG error: " << e.what() << "\n";
                srows = R"({"rows":[]})";
            }
            lap("kg-params-ready");
            if (st.stop_requested()) { log(LogLevel::Warn) << "[worker] stop requested -> abort corr=" << corr << "\n"; return; }

            // --- Kandidaten parsen & Checks -------------------------------------
            auto potCands = normalizeKgPotFM(srows);
            std::vector<ComparisonReport> perRep;
            std::vector<std::string> winners;
            if (!potCands.empty()) {
                winners = selectPotFMByChecks(inv, potCands, &perRep);
            }
            lap("potFM-selected");

            // --- NEU: MonitoringActions je Winner via CommandForce ----------------
            if (!winners.empty()) {
                auto wf = CommandForceFactory::createWinnerFilter(
                    mon_, bus_,
                    // Fetcher: deine bestehende Methode
                    [this](const std::string& fm){ return this->fetchMonitoringActionForFM(fm); },
                    /*defaultTimeoutMs=*/30000
                );
                winners = wf->filter(winners, corr, processName);
                lap("monact-evaluated");
            }

            // --- Winner-Case: genau einer -> Systemreaktion holen/ausführen ------
            if (winners.size() == 1) {
                auto wfSys = CommandForceFactory::createSystemReactionFilter(
                    mon_, bus_,
                    // Du kannst deinen bestehenden Python-Aufruf weiterverwenden:
                    [this](const std::string& fmIri){ return fetchSystemReactionForFM(fmIri); }, // existiert schon
                    /*defaultTimeoutMs=*/30000
                );
                winners = wfSys->filter(winners, corr, processName);   // führt Plan aus + Acks
                // optional: je nach Semantik bei Fehler fall-backen
                return;
            }

            // --- Kein Winner oder Mehrdeutigkeit: Fallback -----------------------
            if (!potCands.empty()) {
                if (winners.empty()) {
                    log(LogLevel::Info) << "[potFM] keine Kandidaten übrig nach MonAct -> Fallback\n";
                } else {
                    log(LogLevel::Warn) << "[potFM] mehrdeutige Kandidaten (" << winners.size()
                                        << ") nach MonAct -> Fallback\n";
                }
                auto plan = buildPlanFromComparison(
                    corr, winners.size()==1 ? ComparisonReport{true,{}} : ComparisonReport{false,{}});
                lap("fallback-plan-built");

                createCommandForceForPlanAndAck(plan, winners.size()==1, processName);
                lap("plan-executed");
                log(LogLevel::Info) << "[worker] corr=" << corr << " END (fallback potFM)\n";
                return;
            }

             //--- Altes Format (id/t/v Rows) – Fallback ---------------------------
            /*auto expects = normalizeKgResponse(srows);
            auto rep     = compareAgainstCache(inv, expects);
            auto plan    = buildPlanFromComparison(corr, rep);
            auto cf = CommandForceFactory::create(CommandForceFactory::Kind::UseMonitor, mon_);
            cf->execute(plan);
            
            using Clock = std::chrono::steady_clock;
            bus_.post(Event{ EventType::evReactionPlanned, Clock::now(),
                std::any{ ReactionPlannedAck{ plan.correlationId, plan.resourceId,
                                            "KG Checks Fallback -> DiagnoseFinished-Puls" } } });
            bus_.post(Event{ EventType::evReactionDone, Clock::now(),
                std::any{ ReactionDoneAck{ plan.correlationId, 1, "OK" } } });*/

            log(LogLevel::Info) << "[worker] corr=" << corr << " END\n";
        });
        job_cv_.notify_one();
    }

    log(LogLevel::Info) << "onEvent EXIT " << evName << " corr=" << corr << " (worker enqueued)\n";
}




