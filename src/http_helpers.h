#pragma once
#include "httplib.h"
#include <string>

inline std::string qp(const httplib::Request& req, const char* key, const char* defv=nullptr) {
  if (auto it = req.get_param_value(key, 0); !it.empty()) return it;
  return defv ? std::string(defv) : std::string();
}
