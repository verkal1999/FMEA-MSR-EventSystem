#pragma once
#include <vector>
#include <string>

struct IWinnerFilter {
    virtual ~IWinnerFilter() = default;
    virtual std::vector<std::string>
    filter(const std::vector<std::string>& winners,
           const std::string& correlationId,
           const std::string& processNameForAck) = 0;
};