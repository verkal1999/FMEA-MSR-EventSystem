#include <iostream>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <sstream>

#include <pybind11/embed.h>
#include "PythonWorker.h"

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

// Versucht, ein evtl. unvollständig quotiertes params_raw zu reparieren.
// Spezifisch für Zeilen mit k:"jobId", wo "v": MAIN.fbJob" ohne öffnendes
// Anführungszeichen kommen kann.
static std::string rm_fixParamsRawIfNeeded(std::string s) {
    auto find_ws = [](const std::string& str, size_t pos) {
        while (pos < str.size() && (str[pos]==' ' || str[pos]=='\t' || str[pos]=='\r' || str[pos]=='\n'))
            ++pos;
        return pos;
    };

    size_t p = 0;
    while (true) {
        // Suche eine Stelle mit  "k": "jobId"
        p = s.find("\"k\"", p);
        if (p == std::string::npos) break;
        size_t p_colon = s.find(':', p);
        if (p_colon == std::string::npos) break;
        size_t p_val = find_ws(s, p_colon+1);
        if (p_val >= s.size()) break;
        if (s.compare(p_val, 8, "\"jobId\"") != 0) { ++p; continue; }

        // Jetzt ab hier die nächste "v":
        size_t pv = s.find("\"v\"", p_val);
        if (pv == std::string::npos) break;
        size_t pv_colon = s.find(':', pv);
        if (pv_colon == std::string::npos) break;
        size_t after = find_ws(s, pv_colon+1);
        if (after >= s.size()) break;

        // Falls kein öffnendes Anführungszeichen kommt, füge eines ein
        if (s[after] != '"') {
            s.insert(after, 1, '"');
            // Bei den Beispielen ist ein schließendes Anführungszeichen schon vorhanden.
            // Falls nicht, könnte man hier bis zum Ende des Tokens laufen und ein " einfügen.
        }
        // Weiter suchen, um mehrere Vorkommen zu fixen
        p = after + 1;
    }
    return s;
}

} // namespace

// ---------- ReactionManager ---------------------------------------------------
ReactionManager::ReactionManager(PLCMonitor& mon, EventBus& bus)
    : mon_(mon), bus_(bus)
{}

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

void ReactionManager::executePlanAndAck(const Plan& plan, bool checksOk) {
    using Clock = std::chrono::steady_clock;
    log(LogLevel::Info) << "executePlanAndAck ENTER corr=" << plan.correlationId
                        << " ops=" << plan.ops.size()
                        << " checksOk=" << (checksOk?"true":"false") << "\n";

    bus_.post(Event{
        EventType::evReactionPlanned, Clock::now(),
        std::any{ ReactionPlannedAck{
            plan.correlationId, plan.resourceId,
            std::string("KG Checks: ") + (checksOk ? "OK" : "FAIL") + " -> DiagnoseFinished-Puls"
        } }
    });
    log(LogLevel::Debug) << "posted evReactionPlanned\n";

    auto cf = CommandForceFactory::create(CommandForceFactory::Kind::UseMonitor, mon_);
    const int rc = cf->execute(plan);
    log(LogLevel::Debug) << "CommandForce rc=" << rc << "\n";

    bus_.post(Event{
        EventType::evReactionDone, Clock::now(),
        std::any{ ReactionDoneAck{ plan.correlationId, rc, rc ? "OK" : "FAIL" } }
    });
    log(LogLevel::Debug) << "posted evReactionDone\n";
    log(LogLevel::Info)  << "executePlanAndAck EXIT\n";
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
            py::object kgi  = cls(
                R"(C:\Users\Alexander Verkhov\OneDrive\Dokumente\MPA\Implementierung_MPA\Test\src\FMEA_KG.ttl)",
                "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/",
                "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/class_",
                "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/op_",
                "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/dp_"
            );
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

Plan ReactionManager::createPlanFromSystemReactionJson(const std::string& corr,
                                                       const std::string& payload)
{
    Plan plan;
    plan.correlationId = corr;
    plan.resourceId    = "Station";

    log(LogLevel::Info)  << "[sysreact] createPlanFromSystemReactionJson ENTER len=" << payload.size() << "\n";
    if (!payload.empty()) {
        auto preview = payload.substr(0, std::min<size_t>(payload.size(), 300));
        log(LogLevel::Debug) << "[sysreact] payload preview: \"" << preview << "\"\n";
    }

    // --- 1) Kopfzeile (IRI) optional ausgeben --------------------------------
    // Format (neu):
    //   <IRI>\n
    //   { "rows": [ ... ] }
    std::string::size_type nl = payload.find('\n');
    if (nl != std::string::npos) {
        std::string head = payload.substr(0, nl);
        // nur „nett“ loggen – nicht weiterverwenden
        if (!head.empty() && head.find("http") != std::string::npos) {
            log(LogLevel::Info) << "[sysreact] sysReact IRI: " << head << "\n";
        }
    }

    // --- 2) JSON-Teil ab erstem '{' herausschneiden + robust parsen ----------
    const std::size_t brace = payload.find('{', (nl == std::string::npos) ? 0 : nl);
    if (brace == std::string::npos) {
        log(LogLevel::Warn) << "[sysreact] no JSON object found in payload\n";
        plan.ops.push_back(Operation{ OpType::PulseBool, "OPCUA.DiagnoseFinished", 4, "true", "", 100 });
        log(LogLevel::Info) << "[sysreact] createPlanFromSystemReactionJson EXIT ops=" << plan.ops.size() << "\n";
        return plan;
    }

    std::string js = payload.substr(brace);

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(js);
        log(LogLevel::Debug) << "[sysreact] JSON parse OK\n";
    } catch (const std::exception& e1) {
        // bekannter Fehler: bei k:"jobId" ist das 'v' manchmal nicht sauber gequotet
        std::string fixed = rm_fixParamsRawIfNeeded(js);
        try {
            j = nlohmann::json::parse(fixed);
            log(LogLevel::Debug) << "[sysreact] JSON parse OK after auto-fix\n";
        } catch (const std::exception& e2) {
            log(LogLevel::Warn) << "[sysreact] JSON parse failed: " << e2.what() << "\n";
            plan.ops.push_back(Operation{ OpType::PulseBool, "OPCUA.DiagnoseFinished", 4, "true", "", 100 });
            log(LogLevel::Info) << "[sysreact] createPlanFromSystemReactionJson EXIT ops=" << plan.ops.size() << "\n";
            return plan;
        }
    }

    // --- 3) rows[] direkt auswerten (neues, flaches Format) ------------------
    // Fallbacks bleiben: falls mal doch „bundles“ oder ein nacktes Array kommt.
    nlohmann::json rows = nlohmann::json::array();

    if (j.is_object() && j.contains("rows") && j["rows"].is_array()) {
        rows = j["rows"];
        log(LogLevel::Info) << "[sysreact] rows count=" << rows.size() << "\n";
    } else if (j.is_object() && j.contains("sysReactions") && j["sysReactions"].is_array()
               && !j["sysReactions"].empty() && j["sysReactions"][0].is_object()
               && j["sysReactions"][0].contains("rows"))
    {
        rows = j["sysReactions"][0]["rows"];
        log(LogLevel::Info) << "[sysreact] bundles->rows count=" << rows.size() << "\n";
    } else if (j.is_array()) {
        rows = j;
        log(LogLevel::Info) << "[sysreact] top is array -> rows count=" << rows.size() << "\n";
    } else {
        log(LogLevel::Warn) << "[sysreact] JSON doesn't contain rows[]\n";
    }

    struct Step {
        std::string jobId;
        std::string methodId;
        std::map<int, UA_Int32> intInputs;
        int timeoutMs = 30000;
    };
    std::map<int, Step> steps;

    auto parse_rows_into_steps = [this, &steps](const nlohmann::json& jrows, const char* origin) {
        for (size_t idx = 0; idx < jrows.size(); ++idx) {
            const auto& r = jrows[idx];
            if (!r.is_object()) {
                log(LogLevel::Warn) << "[sysreact][" << origin << "][row#" << (idx+1)
                                    << "] skip non-object type=" << r.type_name() << "\n";
                continue;
            }
            const int         step = r.value("step", 0);
            const std::string g    = r.value("g", "");
            const std::string k    = r.value("k", "");
            const std::string t    = r.value("t", "");
            const int         i    = r.value("i", 0);

            std::string vStr;
            if (r.contains("v")) vStr = r["v"].is_string() ? r["v"].get<std::string>() : r["v"].dump();

            log(LogLevel::Debug) << "[sysreact][" << origin << "][row#" << (idx+1)
                                 << "] step=" << step << " g=" << g << " k=" << k
                                 << " i=" << i << " t=" << t << " v=" << vStr << "\n";

            if (g == "meta" && k == "jobId") {
                if (r.contains("v") && r["v"].is_string()) steps[step].jobId = r["v"].get<std::string>();
            } else if (g == "method" && k == "methodId") {
                if (r.contains("v") && r["v"].is_string()) steps[step].methodId = r["v"].get<std::string>();
            } else if (g == "meta" && k == "timeoutMs") {
                if (r.contains("v")) {
                    if (r["v"].is_number_integer())        steps[step].timeoutMs = r["v"].get<int>();
                    else if (r["v"].is_string()) { try {   steps[step].timeoutMs = std::stoi(r["v"].get<std::string>()); } catch (...) {} }
                }
            } else if (g == "input") {
                if (t == "int32" && r.contains("v")) {
                    UA_Int32 val = 0;
                    if (r["v"].is_number_integer())        val = static_cast<UA_Int32>(r["v"].get<int64_t>());
                    else if (r["v"].is_string()) { try {   val = static_cast<UA_Int32>(std::stol(r["v"].get<std::string>())); } catch (...) {} }
                    steps[step].intInputs[i] = val;
                }
            }
            // "output" bleibt informativ
        }
    };

    parse_rows_into_steps(rows, "top");

    if (steps.empty()) {
        log(LogLevel::Info) << "[sysreact] steps map is EMPTY after parsing -> nothing to execute\n";
        // trotzdem: DiagnoseFinished zum Schluss
        plan.ops.push_back(Operation{ OpType::PulseBool, "OPCUA.DiagnoseFinished", 4, "true", "", 100 });
        log(LogLevel::Info) << "[sysreact] createPlanFromSystemReactionJson EXIT ops=" << plan.ops.size() << "\n";
        return plan;
    }

    // --- 4) pro Step eine CallMethod-Op erzeugen ------------------------------
    for (auto &kv : steps) {
        const int sidx = kv.first;
        const Step &st = kv.second;

        if (st.jobId.empty() || st.methodId.empty()) {
            log(LogLevel::Warn) << "[sysreact] step " << sidx << " missing jobId/methodId -> skip\n";
            continue;
        }

        Operation op;
        op.type           = OpType::CallMethod;
        op.callObjNodeId  = st.jobId;
        op.callMethNodeId = st.methodId;
        op.timeoutMs      = st.timeoutMs;

        if (auto it = st.intInputs.find(0); it != st.intInputs.end())
            op.arg = std::to_string(it->second);

        log(LogLevel::Debug) << "[sysreact] step " << sidx
                             << " obj="   << op.callObjNodeId
                             << " meth="  << op.callMethNodeId
                             << " x="     << (op.arg.empty() ? "(none)" : op.arg)
                             << " timeoutMs=" << op.timeoutMs << "\n";

        plan.ops.push_back(std::move(op));
    }

    // --- 5) Zwangs-letzter Schritt: DiagnoseFinished pulsen -------------------
    plan.ops.push_back(Operation{ OpType::PulseBool, "OPCUA.DiagnoseFinished", 4, "true", "", 100 });

    log(LogLevel::Info) << "[sysreact] createPlanFromSystemReactionJson EXIT ops=" << plan.ops.size() << "\n";
    return plan;
}





void ReactionManager::executeMethodPlanAndAck(const Plan& plan) {
    using Clock = std::chrono::steady_clock;

    // Ack: PLANNED
    bus_.post(Event{
        EventType::evReactionPlanned, Clock::now(),
        std::any{ ReactionPlannedAck{
            plan.correlationId, plan.resourceId,
            "SystemReaction Plan (CallMethod)"
        } }
    });

    bool allOk = true;

    for (size_t idx = 0; idx < plan.ops.size(); ++idx) {
        const auto& op = plan.ops[idx];

        if (op.type == OpType::CallMethod) {
            // Simple arg: int32 "x" steckt als string in op.arg (falls vorhanden)
            UA_Int32 x = 0;
            bool haveX = false;
            if (!op.arg.empty()) {
                try { x = static_cast<UA_Int32>(std::stol(op.arg)); haveX = true; }
                catch (...) { haveX = false; }
            }

            UA_Int32 yOut = 0;
            const unsigned to = (op.timeoutMs > 0 ? static_cast<unsigned>(op.timeoutMs) : 3000);

            log(LogLevel::Info) << "[exec] CallMethod step#" << idx
                                << " obj='"  << op.callObjNodeId
                                << "' meth='"<< op.callMethNodeId
                                << "' x="    << (haveX ? std::to_string(x) : "(none)")
                                << " timeout=" << to << "ms\n";

            const bool ok = mon_.callJob(op.callObjNodeId, op.callMethNodeId, x, yOut, to);
            allOk = allOk && ok;

            log(LogLevel::Info) << "[exec]   -> " << (ok ? "OK" : "FAIL")
                                << "  yOut=" << yOut << "\n";
        }
        else if (op.type == OpType::PulseBool) {
            // Den Pulse führen wir über CommandForce aus, damit er garantiert identisch
            // zu deinem bisherigen Pfad läuft.
            Plan pulsePlan;
            pulsePlan.correlationId = plan.correlationId;
            pulsePlan.resourceId    = plan.resourceId;
            pulsePlan.ops.push_back(op);

            log(LogLevel::Info) << "[exec] PulseBool step#" << idx
                    << " nodeId='" << op.nodeId << "' ns=" << op.ns
                    << " value=" << op.arg << " timeout=" << op.timeoutMs << "ms\n";

            auto cf = CommandForceFactory::create(CommandForceFactory::Kind::UseMonitor, mon_);
            const int rc = cf->execute(pulsePlan);
            allOk = allOk && (rc != 0);

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
}




void ReactionManager::onMethod(const Event& ev) {
    if (ev.type == EventType::evD2) {
        const auto corr = makeCorrelationId("evD2");
        log(LogLevel::Info) << "onMethod ENTER evD2 corr=" << corr << "\n";

        auto inv = buildInventorySnapshot("PLC1");
        // alt: logBoolInventory(inv);5
        logInventoryVariables(inv); // jetzt ALLES loggen (+ gecachte Werte)

        std::thread([this, corr, inv]() {
            log(LogLevel::Info) << "[thread] corr=" << corr << " START\n";
            std::string srows;
            try {
                log(LogLevel::Debug) << "[thread] calling PythonWorker...\n";
                srows = PythonWorker::instance().call([&]() -> std::string {
                    py::module_ sys = py::module_::import("sys");
                    py::list path = sys.attr("path").cast<py::list>();
                    path.append(R"(C:\Users\Alexander Verkhov\OneDrive\Dokumente\MPA\Implementierung_MPA\Test\src)");

                    py::module_ kg  = py::module_::import("KG_Interface");
                    py::object cls  = kg.attr("KGInterface");
                    py::object kgi  = cls(
                        R"(C:\Users\Alexander Verkhov\OneDrive\Dokumente\MPA\Implementierung_MPA\Test\src\FMEA_KG.ttl)",
                        "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/",
                        "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/class_",
                        "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/op_",
                        "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/dp_"
                    );
                    std::string interruptedSkill = getLastExecutedSkill(inv);
                    if (interruptedSkill.empty()) {
                        // optionaler Fallback-Default, damit KG nicht mit "" arbeitet:
                        interruptedSkill = "UnknownSkill";
                        log(LogLevel::Warn) << "[skill] empty -> using default \"" << interruptedSkill << "\"\n";
                    }
                    auto pyres = kgi.attr("getFailureModeParameters")(interruptedSkill);
                    return py::str(pyres);
                });
                log(LogLevel::Info) << "[thread] PythonWorker OK json_len=" << srows.size()
                                    << " preview=\"" << truncStr(srows) << "\"\n";
            } catch (const std::exception& e) {
                log(LogLevel::Error) << "[thread] KG error: " << e.what() << "\n";
                srows = R"({"rows":[]})";
            }

            auto potCands = normalizeKgPotFM(srows);
            if (!potCands.empty()) {
                std::vector<ComparisonReport> perRep;
                // --- NEU: zuerst potFM-Layout versuchen
                auto winners = selectPotFMByChecks(inv, potCands, &perRep);

                if (winners.empty()) {
                    log(LogLevel::Info) << "[potFM] kein FailureMode passt zu den aktuellen Werten.\n";
                } else if (winners.size() == 1) {
                        const std::string foundFM = winners.front();
                        log(LogLevel::Info) << "[potFM] foundFM = " << foundFM << "\n";

                        // 1) Systemreaction aus KG holen
                        const std::string sysPayload = fetchSystemReactionForFM(foundFM);
                        log(LogLevel::Debug) << "[sysreact] payload len=" << sysPayload.size() << "\n";

                        // 2) Plan daraus bauen
                        Plan sysPlan = createPlanFromSystemReactionJson(corr, sysPayload);
                        log(LogLevel::Info) << "[sysreact] steps=" << sysPlan.ops.size() << "\n";

                        // 3) Ausführen und ACKs schicken
                        executeMethodPlanAndAck(sysPlan);

                        return; // done – kein Fallback nötig
                } else if (winners.size() > 1) {
                        log(LogLevel::Warn) << "[potFM] Mehrdeutige Treffer (" << winners.size()
                                            << ") -> Too many candidates\n";
                        // -> ggf. Fallback tun (z. B. DiagnoseFinished-Puls), wie bisher
                } else {
                        log(LogLevel::Warn) << "[potFM] Kein passender FailureMode -> Fallback\n";
                        // -> ggf. Fallback tun, wie bisher
                }
                    

                // Du kannst hier weiterhin deinen Plan feuern (unabhängig von foundFM),
                // oder optional den Summary-Text anreichern:
                auto plan = buildPlanFromComparison(corr,
                            winners.size()==1
                                ? ComparisonReport{true, {}}  // rein kosmetisch: „OK“
                                : ComparisonReport{false,{}});
                executePlanAndAck(plan, winners.size()==1);

            } else {
                // --- Fallback: altes Format (Einzel-zeilen mit id/t/v wie gehabt)
                auto expects = normalizeKgResponse(srows);
                auto rep     = compareAgainstCache(inv, expects);
                auto plan    = buildPlanFromComparison(corr, rep);
                executePlanAndAck(plan, rep.allOk);
            }
        }).detach();

        log(LogLevel::Info) << "onMethod EXIT evD2 corr=" << corr << " (thread detached)\n";
        return;
    }
}
