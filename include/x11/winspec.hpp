#pragma once

#include "common.hpp"
#include "x11/connection.hpp"

POLYBAR_NS

using connection_t = connection;

struct cw_size {
  cw_size(uint16_t w, uint16_t h) : w(w), h(h) {}
  uint16_t w;
  uint16_t h;
};
struct cw_pos {
  cw_pos(int16_t x, int16_t y) : x(x), y(y) {}
  int16_t x;
  int16_t y;
};
struct cw_border {
  cw_border(uint16_t border_width) : border_width(border_width) {}
  uint16_t border_width;
};
struct cw_class {
  cw_class(uint16_t class_) : class_(class_) {}
  uint16_t class_;
};
struct cw_parent {
  cw_parent(xcb_window_t parent) : parent(parent) {}
  xcb_window_t parent;
};
struct cw_depth {
  cw_depth(uint8_t depth) : depth(depth) {}
  uint8_t depth;
};
struct cw_visual {
  cw_visual(xcb_visualid_t visualid) : visualid(visualid) {}
  xcb_visualid_t visualid;
};
struct cw_mask {
  cw_mask(uint32_t mask) : mask(mask) {}
  const uint32_t mask;
};
struct cw_params {
  cw_params(const xcb_params_cw_t* params) : params(params) {}
  const xcb_params_cw_t* params;
};
struct cw_flush {
  cw_flush(bool checked = true) : checked(checked) {}
  bool checked;
};

/**
 * Create X window
 *
 * Example usage:
 * @code cpp
 *   auto win = winspec(conn)
 *     << cw_size(100, 200)
 *     << cw_pos(10, -20)
 *     << cw_border(9)
 *     << cw_class(XCB_WINDOW_CLASS_INPUT_ONLY)
 *     << cw_parent(0x000110a);
 *     << cw_flush(false);
 * @endcode
 */
class winspec {
 public:
  explicit winspec(connection& conn, uint32_t id = XCB_NONE) : m_connection(conn), m_window(id) {}

  explicit operator xcb_window_t() const {
    return m_window;
  }

  explicit operator xcb_rectangle_t() const {
    return {m_x, m_y, m_width, m_height};
  }

  winspec& operator<<(cw_size w) {
    m_width = w.w;
    m_height = w.h;
    return *this;
  }
  winspec& operator<<(cw_pos p) {
    m_x = p.x;
    m_y = p.y;
    return *this;
  }
  winspec& operator<<(cw_border b) {
    m_border = b.border_width;
    return *this;
  }
  winspec& operator<<(cw_class c) {
    m_class = c.class_;
    return *this;
  }
  winspec& operator<<(cw_parent p) {
    m_parent = p.parent;
    return *this;
  }
  winspec& operator<<(cw_depth d) {
    m_depth = d.depth;
    return *this;
  }
  winspec& operator<<(cw_visual v) {
    m_visual = v.visualid;
    return *this;
  }
  winspec& operator<<(cw_mask m) {
    m_mask = m.mask;
    return *this;
  }
  winspec& operator<<(cw_params p) {
    m_params = p.params;
    return *this;
  }

  xcb_window_t operator<<(cw_flush f) {
    if (!m_window) {
      m_window = m_connection.generate_id();
    }

    uint32_t values[16]{0};
    if (m_params != nullptr) {
      xutils::pack_values(m_mask, m_params, values);
    }

    if (f.checked) {
      m_connection.create_window_checked(
          m_depth, m_window, m_parent, m_x, m_y, m_width, m_height, m_border, m_class, m_visual, m_mask, values);
    } else {
      m_connection.create_window(
          m_depth, m_window, m_parent, m_x, m_y, m_width, m_height, m_border, m_class, m_visual, m_mask, values);
    }

    return m_window;
  }

 protected:
  connection& m_connection;
  uint32_t m_window;
  uint32_t m_parent;
  int16_t m_x;
  int16_t m_y;
  uint16_t m_width;
  uint16_t m_height;
  uint8_t m_depth{XCB_COPY_FROM_PARENT};
  uint16_t m_border{XCB_COPY_FROM_PARENT};
  uint16_t m_class{XCB_COPY_FROM_PARENT};
  xcb_visualid_t m_visual;
  uint32_t m_mask;
  const xcb_params_cw_t* m_params{nullptr};
};

POLYBAR_NS_END
