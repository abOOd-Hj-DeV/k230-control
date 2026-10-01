#pragma once

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace k230 {

// Tiny `--key value` / `--flag` parser; enough for the three executables.
class Cli {
 public:
  Cli(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg.rfind("--", 0) != 0) {
        positional_.push_back(arg);
        continue;
      }
      std::string key = arg.substr(2);
      auto eq = key.find('=');
      if (eq != std::string::npos) {
        values_[key.substr(0, eq)] = key.substr(eq + 1);
      } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        values_[key] = argv[++i];
      } else {
        values_[key] = "";
      }
    }
  }

  bool has(const std::string& key) const { return values_.count(key) != 0; }

  std::string get(const std::string& key, const std::string& fallback = "") const {
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
  }

  long get_int(const std::string& key, long fallback) const {
    auto it = values_.find(key);
    return it == values_.end() ? fallback : std::strtol(it->second.c_str(), nullptr, 10);
  }

  double get_double(const std::string& key, double fallback) const {
    auto it = values_.find(key);
    return it == values_.end() ? fallback : std::strtod(it->second.c_str(), nullptr);
  }

  const std::vector<std::string>& positional() const { return positional_; }

 private:
  std::map<std::string, std::string> values_;
  std::vector<std::string> positional_;
};

}  // namespace k230
