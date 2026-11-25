#include "matrix_model.hpp"
#include "ui.hpp"
namespace aoip::desktop {
void MatrixModel::update(Json value) {
  state = std::move(value);
  for (const auto& d : state.at("devices")) {
    const auto id = d.at("id").get<std::string>();
    if (known.insert(id).second) {
      expanded_tx.insert(id);
      expanded_rx.insert(id);
    }
  }
}
std::string folded(const std::string& value){auto text=aoip::ui::wide(value);if(!text.empty())CharLowerBuffW(text.data(),static_cast<DWORD>(text.size()));return aoip::ui::utf8(text);}
std::vector<AxisItem> axis(const Json& state, bool tx, const std::set<std::string>& expanded,const std::string& filter,bool online_only) {
  std::vector<AxisItem> result;
  for (const auto& d : state.at("devices")) {
    const auto& channels = d.at(tx ? "streams" : "inputs");
    if (channels.empty())
      continue;
    const auto id = d.at("id").get<std::string>(), name = d.at("name").get<std::string>();
    const bool online = d.value("online", false);
    if(online_only&&!online)continue;
    const auto needle=folded(filter);const bool device_match=needle.empty()||folded(name+" "+id).find(needle)!=std::string::npos;
    std::vector<AxisItem> children;
    int index = 1;
    for (const auto& channel : channels)
      for (int side = 1; side <= channel.value("channels", 2); ++side) {
        const int number=index++;const auto labels=channel.value("labels",Json::array());const auto label=side<=int(labels.size())?labels[side-1].get<std::string>():"Channel "+std::to_string(number);
        if(device_match||folded(label+" "+std::to_string(number)).find(needle)!=std::string::npos)children.push_back({id,channel.at("id"),label,name,side,false,online,number});
      }
    if(!device_match&&children.empty())continue;
    result.push_back({id,"",name,name,0,true,online});
    if(expanded.contains(id)||(!needle.empty()&&!device_match))result.insert(result.end(),children.begin(),children.end());
  }
  return result;
}
std::vector<AxisItem> MatrixModel::transmitters() const {
  return axis(state, true, expanded_tx,tx_filter,online_only);
}
std::vector<AxisItem> MatrixModel::receivers() const {
  return axis(state, false, expanded_rx,rx_filter,online_only);
}
Json MatrixModel::route_for(const AxisItem& rx) const {
  Json found = nullptr;
  for (const auto& r : state.at("routes"))
    if (r.at("receiver") == rx.device && r.at("input") == rx.endpoint &&
        (!r.contains("receiverChannel") || r.at("receiverChannel") == rx.side)) {
      found = r;
      if (!found.contains("senderChannel"))
        found["senderChannel"] = rx.side;
    }
  return found;
}
std::string MatrixModel::connection_state(const AxisItem& rx) const {
  const auto route = route_for(rx);
  if (route.is_null())
    return "none";
  if (!route.value("active", false))
    return "pending";
  for (const auto& d : state.at("devices"))
    if (d.at("id") == rx.device)
      for (const auto& i : d.at("inputs"))
        if (i.at("id") == rx.endpoint) {
          const auto stats = i.value("stats", Json::object());
          for (const auto& c : stats.value("channelStats", Json::array()))
            if (c.at("channel") == rx.side)
              return c.value("state", "waiting");
          return stats.value("state", "waiting");
        }
  return "waiting";
}
void MatrixModel::expand_all(bool value) {
  expanded_tx.clear();
  expanded_rx.clear();
  if (value)
    for (const auto& id : known) {
      expanded_tx.insert(id);
      expanded_rx.insert(id);
    }
}
Json preview_network() {
  Json state = {{"devices", Json::array()}, {"routes", Json::array()}, {"events", Json::array()}};
  for (int n = 0; n < 3; ++n) {
    Json d = {{"id", "preview-" + std::to_string(n)},
              {"name", n == 0   ? "Stage Rack"
                       : n == 1 ? "FOH Console"
                                : "Playback Workstation"},
              {"online", true},
              {"role", "device"},
              {"host", "192.168.178." + std::to_string(30 + n)},
              {"format", {{"sampleRate", 48000}}},
              {"clock", {{"mode", "software"}, {"synchronized", false}}},
              {"streams", Json::array()},
              {"inputs", Json::array()}};
    for (int i = 1; i <= 2; ++i) {
      d["streams"].push_back({{"id", "stream-" + std::to_string(i)},
                              {"name", "Virtual audio"},
                              {"channels", 2},
                              {"stats", {{"packets", 10000}}}});
      d["inputs"].push_back(
          {{"id", "input-" + std::to_string(i)},
           {"name", "Virtual audio"},
           {"channels", 2},
           {"port", 5003 + i},
           {"stats",
            {{"state", "receiving"}, {"received", 10000}, {"lost", 0}, {"jitterMs", .4}}}});
    }
    state["devices"].push_back(d);
  }
  for (int n = 0; n < 3; ++n)
    for (int i = 1; i <= 2; ++i)
      for (int side = 1; side <= 2; ++side)
        state["routes"].push_back({{"sender", "preview-" + std::to_string(n)},
                                   {"receiver", "preview-" + std::to_string((n + 1) % 3)},
                                   {"stream", "stream-" + std::to_string(i)},
                                   {"input", "input-" + std::to_string(i)},
                                   {"senderChannel", side},
                                   {"receiverChannel", side},
                                   {"active", true}});
  return state;
}
} // namespace aoip::desktop
