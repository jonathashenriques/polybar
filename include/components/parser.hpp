#pragma once

#include "common.hpp"

POLYBAR_NS

struct bar_settings;
enum class attribute : uint8_t;
enum class mousebtn : uint8_t;

DEFINE_ERROR(unrecognized_token);

class parser {
 public:
  explicit parser(const bar_settings& bar);
  void operator()(string data);
  void codeblock(string data);
  size_t text(string data);

 protected:
  uint32_t parse_color(string s, uint32_t fallback = 0);
  int8_t parse_fontindex(string s);
  attribute parse_attr(const char s);
  mousebtn parse_action_btn(string data);
  string parse_action_cmd(string data);

 private:
  const bar_settings& m_bar;
  vector<int> m_actions;
};

POLYBAR_NS_END
