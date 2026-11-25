#pragma once
#include "aoip/system.hpp"
#include <set>
namespace aoip::desktop {
struct AxisItem {
  std::string device, endpoint, label, device_name;
  int side = 0;
  bool group = false, online = false;
  int number=0;
};
class MatrixModel {
public:
  Json state = {{"devices", Json::array()}, {"routes", Json::array()}, {"events", Json::array()}};
  std::set<std::string> expanded_tx, expanded_rx, known;
  std::string tx_filter,rx_filter;bool online_only=false;
  void update(Json value);
  std::vector<AxisItem> transmitters() const;
  std::vector<AxisItem> receivers() const;
  Json route_for(const AxisItem& receiver) const;
  std::string connection_state(const AxisItem& receiver) const;
  void expand_all(bool expanded);
};
Json preview_network();
std::string folded(const std::string&);
} // namespace aoip::desktop
