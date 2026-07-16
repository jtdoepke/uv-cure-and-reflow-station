#include "device_info.h"

namespace {
DeviceInfo g_info{};
} // namespace

void ui_set_device_info(const DeviceInfo &info) {
  g_info = info;
}

const DeviceInfo &ui_device_info() {
  return g_info;
}
