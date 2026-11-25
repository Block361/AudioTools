#pragma once
#include "aoip/system.hpp"
#include <atomic>
#include <functional>
namespace aoip::detail {
void validate_common(const std::string&, const std::string&, const std::string&,
                     const std::string&);
void heartbeat(const std::atomic<bool>&, const std::string&, const std::function<Json()>&,
               const std::function<void(const Json&)>&, const std::string&,const std::function<void(const std::string&)>& = {});
Json audio_status(const AudioIoStatus&, const std::string&);
} // namespace aoip::detail
