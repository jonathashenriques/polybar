#pragma once

#include <X11/Xresource.h>

#include "common.hpp"

POLYBAR_NS

class xresource_manager {
 public:
  explicit xresource_manager();

  string get_string(string name, string fallback = "") const;
  float get_float(string name, float fallback = 0.0f) const;
  int get_int(string name, int fallback = 0) const;

 protected:
  string load_value(string key, string res_type, size_t n) const;

 private:
  char* m_manager = nullptr;
  XrmDatabase m_db;
};

di::injector<const xresource_manager&> configure_xresource_manager();

POLYBAR_NS_END
