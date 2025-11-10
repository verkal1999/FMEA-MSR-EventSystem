// ==============================
// File: Correlation.h
// Zweck: Erzeugt eindeutige Correlation-IDs für Tracing über Teilkomponenten.
//        Format: "<prefix>-<nanoseconds>-<counter>".
// ==============================
#pragma once
#include <atomic>
#include <string>
#include <chrono>

inline std::string makeCorrelationId(const char* prefix) {
    static std::atomic<unsigned long long> ctr{1}; // atomarer Zähler für Eindeutigkeit
    const auto n  = ctr.fetch_add(1);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
    // Zusammenbau der ID (ohne Allokationen optimiert wäre möglich, aber hier genügt std::string)
    return std::string(prefix) + "-" + std::to_string(ns) + "-" + std::to_string(n);
}
