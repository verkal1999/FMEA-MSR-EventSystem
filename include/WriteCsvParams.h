// WriteCsvParams.h
#pragma once
#include <string>
#include <vector>
struct CsvRow {
  std::string corrId;
  std::string eventType;
  long long   durationMs{0};
  long long   durSumMs{0};
};
struct WriteCsvParams {
  std::string outFile;           
  std::vector<CsvRow> rows;      
  bool withHeader{true};         
};
