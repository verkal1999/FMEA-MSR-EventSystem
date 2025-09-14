#pragma once
#include <string>
#include <cstdint>

inline bool parseNsAndId(const std::string& full,
                         uint16_t& nsOut,
                         std::string& idStrOut,
                         char& idTypeOut) {
    nsOut = 4; idStrOut.clear(); idTypeOut = '?';

    // Kurzform: “OPCUA.…”
    if (full.rfind("OPCUA.", 0) == 0) {
        idTypeOut = 's';
        idStrOut  = full;
        return true;
    }
    // Langform: ns=<n>;<t>=<id>
    if (full.rfind("ns=", 0) != 0) return false;
    const size_t semi = full.find(';');
    if (semi == std::string::npos) return false;

    try {
        nsOut = static_cast<uint16_t>(std::stoi(full.substr(3, semi - 3)));
    } catch (...) {
        nsOut = 4;
    }
    if (semi + 2 >= full.size()) return false;
    idTypeOut = full[semi + 1];
    if (full[semi + 2] != '=') return false;
    idStrOut = full.substr(semi + 3);
    return true;
}
