#pragma once
#include <atomic>
#include <string>
#include <chrono>

inline std::string makeCorrelationId(const char* prefix) {
    static std::atomic<unsigned long long> ctr{1};
    const auto n  = ctr.fetch_add(1);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
    return std::string(prefix) + "-" + std::to_string(ns) + "-" + std::to_string(n);
}
