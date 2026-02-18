#pragma once

#include <optional>
#include <string>
#include <vector>

namespace agent {

struct PortInfo {
  std::string port;
  std::string display_name;
  std::string unique_id;
  bool is_busy = false;
  std::string notes;
};

class PortEnumerator {
 public:
  std::vector<PortInfo> ListPorts(bool probe_busy = true) const;
  std::optional<PortInfo> FindPort(const std::string& port_name, bool probe_busy = true) const;

 private:
  std::string ResolveUniqueId(const std::string& port_path) const;
  bool ProbePortBusy(const std::string& port_path, bool* busy, std::string* notes) const;
};

}  // namespace agent
