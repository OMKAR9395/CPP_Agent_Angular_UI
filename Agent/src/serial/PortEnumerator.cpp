#include "serial/PortEnumerator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include <serialport.h>

namespace agent {
namespace {

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string ConsumeLastSerialError() {
  char* msg = sp_last_error_message();
  if (msg == nullptr) {
    return {};
  }
  std::string out(msg);
  sp_free_error_message(msg);
  return out;
}

bool LooksLikePortInUseMessage(const std::string& message) {
  const std::string lower = ToLower(message);
  return lower.find("busy") != std::string::npos || lower.find("in use") != std::string::npos ||
         lower.find("access is denied") != std::string::npos ||
         lower.find("permission denied") != std::string::npos;
}

}  // namespace

std::vector<PortInfo> PortEnumerator::ListPorts(bool probe_busy) const {
  std::vector<PortInfo> result;
  sp_port** ports = nullptr;
  if (sp_list_ports(&ports) != SP_OK || ports == nullptr) {
    return result;
  }

  for (int i = 0; ports[i] != nullptr; ++i) {
    sp_port* port = ports[i];
    const char* name = sp_get_port_name(port);
    if (name == nullptr) {
      continue;
    }

    PortInfo info;
    info.port = name;

    const char* desc = sp_get_port_description(port);
    info.display_name = (desc != nullptr && std::string(desc).size() > 0) ? desc : info.port;
    info.unique_id = ResolveUniqueId(info.port);

    if (probe_busy) {
      ProbePortBusy(info.port, &info.is_busy, &info.notes);
      if (info.notes.empty()) {
        info.notes = "Busy detection is best-effort.";
      }
    }

    result.push_back(std::move(info));
  }

  sp_free_port_list(ports);
  return result;
}

std::optional<PortInfo> PortEnumerator::FindPort(const std::string& port_name, bool probe_busy) const {
  auto ports = ListPorts(probe_busy);
  for (const auto& info : ports) {
    if (info.port == port_name) {
      return info;
    }
  }
  return std::nullopt;
}

std::string PortEnumerator::ResolveUniqueId(const std::string& port_path) const {
#if defined(__linux__)
  namespace fs = std::filesystem;
  const fs::path by_id_dir("/dev/serial/by-id");
  if (!fs::exists(by_id_dir) || !fs::is_directory(by_id_dir)) {
    return {};
  }

  std::error_code ec;
  const fs::path canonical_port = fs::weakly_canonical(fs::path(port_path), ec);
  if (ec) {
    return {};
  }

  for (const auto& entry : fs::directory_iterator(by_id_dir, ec)) {
    if (ec || !entry.is_symlink(ec)) {
      continue;
    }

    const fs::path target = fs::weakly_canonical(entry.path(), ec);
    if (ec) {
      continue;
    }
    if (target == canonical_port) {
      return entry.path().string();
    }
  }
#endif
  return {};
}

bool PortEnumerator::ProbePortBusy(const std::string& port_path, bool* busy, std::string* notes) const {
  if (busy == nullptr || notes == nullptr) {
    return false;
  }
  *busy = false;
  notes->clear();

  sp_port* port = nullptr;
  if (sp_get_port_by_name(port_path.c_str(), &port) != SP_OK || port == nullptr) {
    return false;
  }

  const sp_return open_rc = sp_open(port, SP_MODE_READ_WRITE);
  if (open_rc == SP_OK) {
    sp_close(port);
    sp_free_port(port);
    return true;
  }

  const std::string err = ConsumeLastSerialError();
  if (LooksLikePortInUseMessage(err)) {
    *busy = true;
    *notes = "Port appears to be in use by another application.";
  } else if (!err.empty()) {
    *notes = "Busy probe failed: " + err;
  }

  sp_free_port(port);
  return true;
}

}  // namespace agent
