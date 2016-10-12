#pragma once

#include <xcb/xcb_icccm.h>
#include <mutex>

#include "common.hpp"
#include "components/config.hpp"
#include "components/logger.hpp"
#include "components/parser.hpp"
#include "components/types.hpp"
#include "components/x11/connection.hpp"
#include "components/x11/draw.hpp"
#include "components/x11/fontmanager.hpp"
#include "components/x11/randr.hpp"
#include "components/x11/tray.hpp"
#include "components/x11/types.hpp"
#include "components/x11/window.hpp"
#include "components/x11/xlib.hpp"
#include "components/x11/xutils.hpp"
#include "utils/bspwm.hpp"
#include "utils/i3.hpp"
#include "utils/math.hpp"
#include "utils/string.hpp"
#include "utils/threading.hpp"

LEMONBUDDY_NS

namespace bar_signals {
  delegate::Signal1<string> action_click;
};

class bar : public xpp::event::sink<evt::button_press, evt::expose> {
 public:
  /**
   * Construct bar
   */
  explicit bar(connection& conn, const config& config, const logger& logger,
      unique_ptr<fontmanager> fontmanager)
      : m_connection(conn)
      , m_conf(config)
      , m_log(logger)
      , m_fontmanager(forward<decltype(fontmanager)>(fontmanager)) {}

  /**
   * Cleanup signal handlers and destroy the bar window
   */
  ~bar() {
    std::lock_guard<threading_util::spin_lock> lck(m_lock);
    parser_signals::alignment_change.disconnect(this, &bar::on_alignment_change);
    parser_signals::attribute_set.disconnect(this, &bar::on_attribute_set);
    parser_signals::attribute_unset.disconnect(this, &bar::on_attribute_unset);
    parser_signals::attribute_toggle.disconnect(this, &bar::on_attribute_toggle);
    parser_signals::action_block_open.disconnect(this, &bar::on_action_block_open);
    parser_signals::action_block_close.disconnect(this, &bar::on_action_block_close);
    parser_signals::color_change.disconnect(this, &bar::on_color_change);
    parser_signals::font_change.disconnect(this, &bar::on_font_change);
    parser_signals::pixel_offset.disconnect(this, &bar::on_pixel_offset);
    parser_signals::ascii_text_write.disconnect(this, &bar::draw_character);
    parser_signals::unicode_text_write.disconnect(this, &bar::draw_character);
    if (m_tray.align != alignment::NONE)
      tray_signals::report_slotcount.disconnect(this, &bar::on_tray_report);
    if (m_sinkattached)
      m_connection.detach_sink(this, 1);
    m_window.destroy();
  }

  /**
   * Configure injection module
   */
  template <typename T = unique_ptr<bar>>
  static di::injector<T> configure() {
    // clang-format off
    return di::make_injector(
        connection::configure(),
        config::configure(),
        logger::configure(),
        fontmanager::configure());
    // clang-format on
  }

  /**
   * Create required components
   *
   * This is done outside the constructor due to boost::di noexcept
   */
  void bootstrap(bool nodraw = false) {  //{{{
    m_screen = m_connection.screen();
    m_visual = m_connection.visual_type(m_screen, 32).get();
    auto monitors = randr_util::get_monitors(m_connection, m_connection.screen()->root);
    auto bs = m_conf.bar_section();

    // Look for the defined monitor {{{

    if (monitors.empty())
      throw application_error("No monitors found");

    auto monitor_name = m_conf.get<string>(bs, "monitor", "");
    if (monitor_name.empty())
      monitor_name = monitors[0]->name;

    for (auto&& monitor : monitors) {
      if (monitor_name.compare(monitor->name) == 0) {
        m_bar.monitor = std::move(monitor);
        break;
      }
    }

    if (m_bar.monitor)
      m_log.trace("bar: Found matching monitor %s (%ix%i+%i+%i)", m_bar.monitor->name,
          m_bar.monitor->w, m_bar.monitor->h, m_bar.monitor->x, m_bar.monitor->y);
    else
      throw application_error("Could not find monitor: " + monitor_name);

    // }}}
    // Set bar colors {{{

    m_bar.background = color::parse(m_conf.get<string>(bs, "background", m_bar.background.hex()));
    m_bar.foreground = color::parse(m_conf.get<string>(bs, "foreground", m_bar.foreground.hex()));
    m_bar.linecolor = color::parse(m_conf.get<string>(bs, "linecolor", m_bar.linecolor.hex()));

    // }}}
    // Set border values {{{

    auto bsize = m_conf.get<int>(bs, "border-size", 0);
    auto bcolor = m_conf.get<string>(bs, "border-color", "");

    m_borders.emplace(border::TOP, border_settings{});
    m_borders[border::TOP].size = m_conf.get<int>(bs, "border-top", bsize);
    m_borders[border::TOP].color = color::parse(m_conf.get<string>(bs, "border-top-color", bcolor));

    m_borders.emplace(border::BOTTOM, border_settings{});
    m_borders[border::BOTTOM].size = m_conf.get<int>(bs, "border-bottom", bsize);
    m_borders[border::BOTTOM].color =
        color::parse(m_conf.get<string>(bs, "border-bottom-color", bcolor));

    m_borders.emplace(border::LEFT, border_settings{});
    m_borders[border::LEFT].size = m_conf.get<int>(bs, "border-left", bsize);
    m_borders[border::LEFT].color =
        color::parse(m_conf.get<string>(bs, "border-left-color", bcolor));

    m_borders.emplace(border::RIGHT, border_settings{});
    m_borders[border::RIGHT].size = m_conf.get<int>(bs, "border-right", bsize);
    m_borders[border::RIGHT].color =
        color::parse(m_conf.get<string>(bs, "border-right-color", bcolor));

    // }}}
    // Set size and position {{{

    GET_CONFIG_VALUE(bs, m_bar.dock, "dock");
    GET_CONFIG_VALUE(bs, m_bar.bottom, "bottom");
    GET_CONFIG_VALUE(bs, m_bar.spacing, "spacing");
    GET_CONFIG_VALUE(bs, m_bar.lineheight, "lineheight");
    GET_CONFIG_VALUE(bs, m_bar.offset_x, "offset-x");
    GET_CONFIG_VALUE(bs, m_bar.offset_y, "offset-y");
    GET_CONFIG_VALUE(bs, m_bar.padding_left, "padding-left");
    GET_CONFIG_VALUE(bs, m_bar.padding_right, "padding-right");
    GET_CONFIG_VALUE(bs, m_bar.module_margin_left, "module-margin-left");
    GET_CONFIG_VALUE(bs, m_bar.module_margin_right, "module-margin-right");

    auto w = m_conf.get<string>(bs, "width", "100%");
    auto h = m_conf.get<string>(bs, "height", "24");

    m_bar.width = std::atoi(w.c_str());
    if (w.find("%") != string::npos)
      m_bar.width = m_bar.monitor->w * (m_bar.width / 100.0) + 0.5f;

    m_bar.height = std::atoi(h.c_str());
    if (h.find("%") != string::npos)
      m_bar.height = m_bar.monitor->h * (m_bar.height / 100.0) + 0.5f;

    // apply offsets
    m_bar.width -= m_bar.offset_x * 2;
    m_bar.x = m_bar.offset_x + m_bar.monitor->x;
    m_bar.y = m_bar.offset_y + m_bar.monitor->y;

    // apply borders
    m_bar.height += m_borders[border::TOP].size;
    m_bar.height += m_borders[border::BOTTOM].size;

    if (m_bar.bottom)
      m_bar.y = m_bar.monitor->y + m_bar.monitor->h - m_bar.height - m_bar.offset_y;

    if (m_bar.width <= 0 || m_bar.width > m_bar.monitor->w)
      throw application_error("Resulting bar width is out of bounds");
    if (m_bar.height <= 0 || m_bar.height > m_bar.monitor->h)
      throw application_error("Resulting bar height is out of bounds");

    m_bar.width = math_util::cap<int>(m_bar.width, 0, m_bar.monitor->w);
    m_bar.height = math_util::cap<int>(m_bar.height, 0, m_bar.monitor->h);

    m_bar.vertical_mid =
        (m_bar.height + m_borders[border::TOP].size - m_borders[border::BOTTOM].size) / 2;

    m_log.trace("bar: Resulting bar geom %ix%i+%i+%i", m_bar.width, m_bar.height, m_bar.x, m_bar.y);

    // }}}
    // Set the WM_NAME value {{{

    m_bar.wmname = "lemonbuddy-" + bs.substr(4) + "_" + m_bar.monitor->name;
    m_bar.wmname = m_conf.get<string>(bs, "wm-name", m_bar.wmname);
    m_bar.wmname = string_util::replace(m_bar.wmname, " ", "-");

    // }}}
    // Set misc parameters {{{

    m_bar.separator = string_util::trim(m_conf.get<string>(bs, "separator", ""), '"');

    // }}}
    // Checking nodraw {{{

    if (nodraw) {
      m_log.trace("bar: Abort bootstrap routine (reason: nodraw)");
      return;
    }

    // }}}
    // Setup graphic components and create the window {{{

    m_log.trace("bar: Create colormap");
    {
      m_connection.create_colormap_checked(
          XCB_COLORMAP_ALLOC_NONE, m_colormap, m_screen->root, m_visual->visual_id);
    }

    m_log.trace("bar: Create window %s", m_connection.id(m_window));
    {
      uint32_t mask = 0;
      xcb_params_cw_t params;
      // clang-format off
      XCB_AUX_ADD_PARAM(&mask, &params, back_pixel, m_bar.background.value());
      XCB_AUX_ADD_PARAM(&mask, &params, border_pixel, m_bar.background.value());
      XCB_AUX_ADD_PARAM(&mask, &params, colormap, m_colormap);
      XCB_AUX_ADD_PARAM(&mask, &params, override_redirect, m_bar.dock);
      XCB_AUX_ADD_PARAM(&mask, &params, event_mask, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS);
      // clang-format on
      m_window.create_checked(m_bar.x, m_bar.y, m_bar.width, m_bar.height, mask, &params);
    }

    m_log.trace("bar: Set WM_NAME");
    {
      xcb_icccm_set_wm_name(
          m_connection, m_window, XCB_ATOM_STRING, 8, m_bar.wmname.length(), m_bar.wmname.c_str());
      xcb_icccm_set_wm_class(m_connection, m_window, 21, "lemonbuddy\0Lemonbuddy");
    }

    m_log.trace("bar: Set _NET_WM_WINDOW_TYPE");
    {
      // const uint32_t win_types[2] = {_NET_WM_WINDOW_TYPE_DOCK, _NET_WM_WINDOW_TYPE_NORMAL};
      const uint32_t win_types[1] = {_NET_WM_WINDOW_TYPE_DOCK};
      m_connection.change_property_checked(
          XCB_PROP_MODE_REPLACE, m_window, _NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM, 32, 1, win_types);
    }

    m_log.trace("bar: Set _NET_WM_STATE");
    {
      if (m_bar.width == m_bar.monitor->w) {
        const uint32_t win_states[3] = {
            _NET_WM_STATE_STICKY, _NET_WM_STATE_SKIP_TASKBAR, _NET_WM_STATE_MAXIMIZED_VERT};
        m_connection.change_property_checked(
            XCB_PROP_MODE_REPLACE, m_window, _NET_WM_STATE, XCB_ATOM_ATOM, 32, 3, win_states);
      } else {
        const uint32_t win_states[2] = {_NET_WM_STATE_STICKY, _NET_WM_STATE_SKIP_TASKBAR};
        m_connection.change_property_checked(
            XCB_PROP_MODE_REPLACE, m_window, _NET_WM_STATE, XCB_ATOM_ATOM, 32, 2, win_states);
      }
    }

    m_log.trace("bar: Set _NET_WM_STRUT");
    {
      // clang-format off
      uint32_t none{0};
      uint32_t value_list[4]{
        static_cast<uint32_t>(m_bar.x), // left
        none, // right
        m_bar.bottom ? none : m_bar.height + m_bar.offset_y, // top
        m_bar.bottom ? m_bar.height + m_bar.offset_y : none, // bottom
      };
      // clang-format on
      m_connection.change_property_checked(
          XCB_PROP_MODE_REPLACE, m_window, _NET_WM_STRUT, XCB_ATOM_CARDINAL, 32, 4, value_list);
    }

    m_log.trace("bar: Set _NET_WM_STRUT_PARTIAL");
    {
      // clang-format off
      uint32_t none{0};
      const uint32_t value_list[12]{
        static_cast<uint32_t>(m_bar.x), // left
        none, // right
        m_bar.bottom ? none : m_bar.height + m_bar.offset_y, // top
        m_bar.bottom ? m_bar.height + m_bar.offset_y : none, // bottom
        none, // left_start_y
        none, // left_end_y
        none, // right_start_y
        none, // right_end_y
        m_bar.bottom ? none : m_bar.x, // top_start_x
        m_bar.bottom ? none : static_cast<uint32_t>(m_bar.x + m_bar.width), // top_end_x
        m_bar.bottom ? m_bar.x : none, // bottom_start_x
        m_bar.bottom ? static_cast<uint32_t>(m_bar.x + m_bar.width) : none, // bottom_end_x
      };
      // clang-format on
      m_connection.change_property_checked(XCB_PROP_MODE_REPLACE, m_window, _NET_WM_STRUT_PARTIAL,
          XCB_ATOM_CARDINAL, 32, 12, value_list);
    }

    m_log.trace("bar: Set _NET_WM_DESKTOP");
    {
      const uint32_t value_list[1]{-1u};
      m_connection.change_property_checked(
          XCB_PROP_MODE_REPLACE, m_window, _NET_WM_DESKTOP, XCB_ATOM_CARDINAL, 32, 1, value_list);
    }

    m_log.trace("bar: Set _NET_WM_PID");
    {
      const uint32_t value_list[1]{uint32_t(getpid())};
      m_connection.change_property_checked(
          XCB_PROP_MODE_REPLACE, m_window, _NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, value_list);
    }

    m_log.trace("bar: Create pixmap");
    {
      m_connection.create_pixmap_checked(
          m_visual->visual_id == m_screen->root_visual ? XCB_COPY_FROM_PARENT : 32, m_pixmap,
          m_window, m_bar.width, m_bar.height);
    }

    m_log.trace("bar: Map window");
    {
      m_connection.flush();
      m_connection.map_window_checked(m_window);
    }

    // }}}
    // Restack window and put it above defined WM's root {{{

    try {
      auto wm_restack = m_conf.get<string>(bs, "wm-restack");
      auto restacked = false;

      if (wm_restack == "bspwm") {
        restacked = bspwm_util::restack_above_root(m_connection, m_bar.monitor, m_window);

      } else if (wm_restack == "i3" && m_bar.dock) {
        restacked = i3_util::restack_above_root(m_connection, m_bar.monitor, m_window);

      } else if (wm_restack == "i3" && !m_bar.dock) {
        m_log.warn("Ignoring restack of i3 window (not needed when dock = false)");
        wm_restack.clear();

      } else {
        m_log.warn("Ignoring unsupported wm-restack option '%s'", wm_restack);
        wm_restack.clear();
      }

      if (restacked) {
        m_log.info("Successfully restacked bar window");
      } else if (!wm_restack.empty()) {
        m_log.err("Failed to restack bar window");
      }
    } catch (const key_error& err) {
    }

    // }}}
    // Create graphic contexts {{{

    m_log.trace("bar: Create graphic contexts");
    {
      // clang-format off
      vector<uint32_t> colors {
        m_bar.background.value(),
        m_bar.foreground.value(),
        m_bar.linecolor.value(),
        m_bar.linecolor.value(),
        m_borders[border::TOP].color.value(),
        m_borders[border::BOTTOM].color.value(),
        m_borders[border::LEFT].color.value(),
        m_borders[border::RIGHT].color.value(),
      };
      // clang-format on

      for (int i = 1; i <= 8; i++) {
        uint32_t mask = 0;
        uint32_t value_list[32];
        xcb_params_gc_t params;
        XCB_AUX_ADD_PARAM(&mask, &params, foreground, colors[i - 1]);
        XCB_AUX_ADD_PARAM(&mask, &params, graphics_exposures, 0);
        xutils::pack_values(mask, &params, value_list);
        m_gcontexts.emplace(gc(i), gcontext{m_connection, m_connection.generate_id()});
        m_connection.create_gc_checked(m_gcontexts.at(gc(i)), m_pixmap, mask, value_list);
      }
    }

    // }}}
    // Load fonts {{{

    auto fonts_loaded = false;
    auto fontindex = 0;
    auto fonts = m_conf.get_list<string>(bs, "font");

    for (auto f : fonts) {
      fontindex++;
      vector<string> fd = string_util::split(f, ';');
      string pattern{fd[0]};
      int offset{0};

      if (fd.size() > 1)
        offset = std::stoi(fd[1], 0, 10);

      if (m_fontmanager->load(pattern, fontindex, offset))
        fonts_loaded = true;
      else
        m_log.warn("Unable to load font '%s'", fd[0]);
    }

    if (!fonts_loaded) {
      m_log.warn("Loading fallback font");

      if (!m_fontmanager->load("fixed"))
        throw application_error("Unable to load fonts");
    }

    m_fontmanager->allocate_color(m_bar.foreground);

    // }}}
    // Set tray settings {{{

    try {
      auto tray_position = m_conf.get<string>(bs, "tray-position");

      if (tray_position == "left")
        m_tray.align = alignment::LEFT;
      else if (tray_position == "right")
        m_tray.align = alignment::RIGHT;
      else
        m_tray.align = alignment::NONE;
    } catch (const key_error& err) {
      m_tray.align = alignment::NONE;
    }

    if (m_tray.align != alignment::NONE) {
      m_tray.background = m_bar.background.value();
      m_tray.height = m_bar.height;
      m_tray.height -= m_borders.at(border::BOTTOM).size;
      m_tray.height -= m_borders.at(border::TOP).size;

      if (m_tray.height % 2 != 0) {
        m_tray.height--;
      }

      if (m_tray.height > 24) {
        m_tray.spacing = (m_tray.height - 24) / 2;
        m_tray.height = 24;
      }

      m_tray.width = m_tray.height;
      m_tray.orig_y = m_bar.y + m_borders.at(border::TOP).size;

      if (m_tray.align == alignment::RIGHT)
        m_tray.orig_x = m_bar.x + m_bar.width - m_borders.at(border::RIGHT).size;
      else
        m_tray.orig_x = m_bar.x + m_borders.at(border::LEFT).size;
    }

    // }}}
    // Connect signal handlers {{{

    parser_signals::alignment_change.connect(this, &bar::on_alignment_change);
    parser_signals::attribute_set.connect(this, &bar::on_attribute_set);
    parser_signals::attribute_unset.connect(this, &bar::on_attribute_unset);
    parser_signals::attribute_toggle.connect(this, &bar::on_attribute_toggle);
    parser_signals::action_block_open.connect(this, &bar::on_action_block_open);
    parser_signals::action_block_close.connect(this, &bar::on_action_block_close);
    parser_signals::color_change.connect(this, &bar::on_color_change);
    parser_signals::font_change.connect(this, &bar::on_font_change);
    parser_signals::pixel_offset.connect(this, &bar::on_pixel_offset);
    parser_signals::ascii_text_write.connect(this, &bar::draw_character);
    parser_signals::unicode_text_write.connect(this, &bar::draw_character);

    if (m_tray.align != alignment::NONE)
      tray_signals::report_slotcount.connect(this, &bar::on_tray_report);

    // }}}

    m_connection.attach_sink(this, 1);
    m_sinkattached = true;

    m_connection.flush();
  }  //}}}

  /**
   * Parse input string and redraw the bar window
   *
   * @param data Input string
   * @param force Unless true, do not parse unchanged data
   */
  void parse(string data, bool force = false) {  //{{{
    std::lock_guard<threading_util::spin_lock> lck(m_lock);
    {
      if (data == m_prevdata && !force)
        return;

      m_prevdata = data;

      // TODO: move to fontmanager
      m_xftdraw = XftDrawCreate(xlib::get_display(), m_pixmap, xlib::get_visual(), m_colormap);

      m_bar.align = alignment::LEFT;
      m_xpos = m_borders[border::LEFT].size;
      m_attributes = 0;

#if DEBUG and DRAW_CLICKABLE_AREA_HINTS
      for (auto&& action : m_actions) {
        m_connection.destroy_window(action.clickable_area);
      }
#endif

      m_actions.clear();

      draw_background();

      if (m_tray.align == alignment::LEFT && m_tray.slots)
        m_xpos += ((m_tray.width + m_tray.spacing) * m_tray.slots) + m_tray.spacing;

      try {
        parser parser(m_bar);
        parser(data);
      } catch (const unrecognized_token& err) {
        m_log.err("Unrecognized syntax token '%s'", err.what());
      }

      if (m_tray.align == alignment::RIGHT && m_tray.slots)
        draw_shift(m_xpos, ((m_tray.width + m_tray.spacing) * m_tray.slots) + m_tray.spacing);

      draw_border(border::ALL);

      flush();

      XftDrawDestroy(m_xftdraw);
    }
  }  //}}}

  /**
   * Copy the contents of the pixmap's onto the bar window
   */
  void flush() {  //{{{
    m_connection.copy_area(
        m_pixmap, m_window, m_gcontexts.at(gc::FG), 0, 0, 0, 0, m_bar.width, m_bar.height);
    m_connection.copy_area(
        m_pixmap, m_window, m_gcontexts.at(gc::BT), 0, 0, 0, 0, m_bar.width, m_bar.height);
    m_connection.copy_area(
        m_pixmap, m_window, m_gcontexts.at(gc::BB), 0, 0, 0, 0, m_bar.width, m_bar.height);
    m_connection.copy_area(
        m_pixmap, m_window, m_gcontexts.at(gc::BL), 0, 0, 0, 0, m_bar.width, m_bar.height);
    m_connection.copy_area(
        m_pixmap, m_window, m_gcontexts.at(gc::BR), 0, 0, 0, 0, m_bar.width, m_bar.height);

#if DEBUG and DRAW_CLICKABLE_AREA_HINTS
    map<alignment, int> hint_num{
        { {alignment::LEFT, 0},
          {alignment::CENTER, 0},
          {alignment::RIGHT, 0},
        }};
#endif

    for (auto&& action : m_actions) {
      if (action.active) {
        m_log.warn("Action block not closed");
        m_log.warn("action.command = %s", action.command);
      } else {
        m_log.trace("bar: Action details");
        m_log.trace("action.command = %s", action.command);
        m_log.trace("action.button = %i", static_cast<int>(action.button));
        m_log.trace("action.start_x = %i", action.start_x);
        m_log.trace("action.end_x = %i", action.end_x);
#if DEBUG and DRAW_CLICKABLE_AREA_HINTS
        m_log.info("Drawing clickable area hints");

        hint_num[action.align]++;

        auto x = action.start_x;
        auto y = m_bar.y + hint_num[action.align]++ * DRAW_CLICKABLE_AREA_HINTS_OFFSET_Y;
        auto w = action.end_x - action.start_x - 2;
        auto h = m_bar.height - 2;

        const uint32_t mask = XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT;
        const uint32_t border_color = hint_num[action.align] % 2 ? 0xff0000 : 0x00ff00;
        const uint32_t values[2]{border_color, true};

        auto scr = m_connection.screen();

        action.clickable_area = m_connection.generate_id();
        m_connection.create_window_checked(scr->root_depth, action.clickable_area, scr->root, x, y,
            w, h, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual, mask, values);
        m_connection.map_window_checked(action.clickable_area);
#else
        m_log.trace("bar: Visual hints for clickable area's disabled");
#endif
      }
    }
  }  //}}}

  /**
   * Get the bar settings container
   */
  const bar_settings settings() const {  // {{{
    return m_bar;
  }  // }}}

  /**
   * Get the tray settings container
   */
  const tray_settings tray() const {  // {{{
    return m_tray;
  }  // }}}

  /**
   * Mouse button event handler
   */
  void handle(const evt::button_press& evt) {  // {{{
    std::lock_guard<threading_util::spin_lock> lck(m_lock);
    {
      m_log.trace("bar: Received button press event: %i at pos(%i, %i)",
          static_cast<int>(evt->detail), evt->event_x, evt->event_y);

      mousebtn button = static_cast<mousebtn>(evt->detail);

      for (auto&& action : m_actions) {
        if (action.active) {
          m_log.trace("bar: Ignoring action: unclosed)");
          continue;
        } else if (action.button != button) {
          m_log.trace("bar: Ignoring action: button mismatch");
          continue;
        } else if (action.start_x > evt->event_x) {
          m_log.trace(
              "bar: Ignoring action: start_x(%i) > event_x(%i)", action.start_x, evt->event_x);
          continue;
        } else if (action.end_x < evt->event_x) {
          m_log.trace("bar: Ignoring action: end_x(%i) < event_x(%i)", action.end_x, evt->event_x);
          continue;
        }

        m_log.info("Found matching input area");
        m_log.trace("action.command = %s", action.command);
        m_log.trace("action.button = %i", static_cast<int>(action.button));
        m_log.trace("action.start_x = %i", action.start_x);
        m_log.trace("action.end_x = %i", action.end_x);

        if (!bar_signals::action_click.empty())
          bar_signals::action_click.emit(action.command);
        else
          m_log.warn("No signal handler's connected to 'action_click'");

        return;
      }

      m_log.warn("No matching input area found");
    }
  }  // }}}

  /**
   * Event handler for XCB_EXPOSE events
   */
  void handle(const evt::expose& evt) {  // {{{
    if (evt->window != m_window)
      return;
    m_log.trace("bar: Received expose event");
    flush();
  }  // }}}

 protected:
  /**
   * Handle alignment update
   */
  void on_alignment_change(alignment align) {  //{{{
    if (align == m_bar.align)
      return;
    m_log.trace("bar: alignment_change(%i)", static_cast<int>(align));
    m_bar.align = align;

    if (align == alignment::LEFT) {
      m_xpos = m_borders[border::LEFT].size;
    } else if (align == alignment::RIGHT) {
      m_xpos = m_borders[border::RIGHT].size;
    } else {
      m_xpos = 0;
    }
  }  //}}}

  /**
   * Handle attribute on state
   */
  void on_attribute_set(attribute attr) {  //{{{
    int val{static_cast<int>(attr)};
    if ((m_attributes & val) != 0)
      return;
    m_log.trace("bar: attribute_set(%i)", val);
    m_attributes |= val;
  }  //}}}

  /**
   * Handle attribute off state
   */
  void on_attribute_unset(attribute attr) {  //{{{
    int val{static_cast<int>(attr)};
    if ((m_attributes & val) == 0)
      return;
    m_log.trace("bar: attribute_unset(%i)", val);
    m_attributes ^= val;
  }  //}}}

  /**
   * Handle attribute toggle state
   */
  void on_attribute_toggle(attribute attr) {  //{{{
    int val{static_cast<int>(attr)};
    m_log.trace("bar: attribute_toggle(%i)", val);
    m_attributes ^= val;
  }  //}}}

  /**
   * Handle action block start
   */
  void on_action_block_open(mousebtn btn, string cmd) {  //{{{
    if (btn == mousebtn::NONE)
      btn = mousebtn::LEFT;
    m_log.trace("bar: action_block_open(%i, %s)", static_cast<int>(btn), cmd);
    action_block action;
    action.active = true;
    action.align = m_bar.align;
    action.button = btn;
    action.start_x = m_xpos;
    action.command = string_util::replace_all(cmd, ":", "\\:");
    m_actions.emplace_back(action);
  }  //}}}

  /**
   * Handle action block end
   */
  void on_action_block_close(mousebtn btn) {  //{{{
    m_log.trace("bar: action_block_close(%i)", static_cast<int>(btn));

    for (auto i = m_actions.size(); i > 0; i--) {
      auto& action = m_actions[i - 1];

      if (!action.active || action.button != btn)
        continue;

      action.active = false;

      if (action.align == alignment::LEFT) {
        action.end_x = m_xpos;
      } else if (action.align == alignment::CENTER) {
        int base_x = m_bar.width;
        base_x -= m_borders[border::RIGHT].size;
        base_x /= 2;
        base_x += m_borders[border::LEFT].size;

        int clickable_width = m_xpos - action.start_x;
        action.start_x = base_x - clickable_width / 2 + action.start_x / 2;
        action.end_x = action.start_x + clickable_width;
      } else if (action.align == alignment::RIGHT) {
        int base_x = m_bar.width - m_borders[border::RIGHT].size;
        action.start_x = base_x - m_xpos + action.start_x;
        action.end_x = base_x;
      }

      return;
    }
  }  //}}}

  /**
   * Handle color change
   */
  void on_color_change(gc gc_, color color_) {  //{{{
    m_log.trace(
        "bar: color_change(%i, %s -> %s)", static_cast<int>(gc_), color_.hex(), color_.rgb());

    const uint32_t value_list[32]{color_.value()};
    m_connection.change_gc(m_gcontexts.at(gc_), XCB_GC_FOREGROUND, value_list);

    if (gc_ == gc::FG)
      m_fontmanager->allocate_color(color_);
  }  //}}}

  /**
   * Handle font change
   */
  void on_font_change(int index) {  //{{{
    m_log.trace("bar: font_change(%i)", index);
    m_fontmanager->set_preferred_font(index);
  }  //}}}

  /**
   * Handle pixel offsetting
   */
  void on_pixel_offset(int px) {  //{{{
    m_log.trace("bar: pixel_offset(%i)", px);
    draw_shift(m_xpos, px);
    m_xpos += px;
  }  //}}}

  /**
   * Proess systray report
   */
  void on_tray_report(uint16_t slots) {  // {{{
    if (m_tray.slots == slots)
      return;

    m_log.trace("bar: tray_report(%lu)", slots);
    m_tray.slots = slots;

    if (!m_prevdata.empty())
      parse(m_prevdata, true);
  }  // }}}

  /**
   * Draw background onto the pixmap
   */
  void draw_background() {  //{{{
    draw_util::fill(
        m_connection, m_pixmap, m_gcontexts.at(gc::BG), 0, 0, m_bar.width, m_bar.height);
  }  //}}}

  /**
   * Draw borders onto the pixmap
   */
  void draw_border(border border_) {  //{{{
    switch (border_) {
      case border::NONE:
        break;

      case border::TOP:
        if (m_borders[border::TOP].size > 0) {
          draw_util::fill(m_connection, m_pixmap, m_gcontexts.at(gc::BT),
              m_borders[border::LEFT].size, 0,
              m_bar.width - m_borders[border::LEFT].size - m_borders[border::RIGHT].size,
              m_borders[border::TOP].size);
        }
        break;

      case border::BOTTOM:
        if (m_borders[border::BOTTOM].size > 0) {
          draw_util::fill(m_connection, m_pixmap, m_gcontexts.at(gc::BB),
              m_borders[border::LEFT].size, m_bar.height - m_borders[border::BOTTOM].size,
              m_bar.width - m_borders[border::LEFT].size - m_borders[border::RIGHT].size,
              m_borders[border::BOTTOM].size);
        }
        break;

      case border::LEFT:
        if (m_borders[border::LEFT].size > 0) {
          draw_util::fill(m_connection, m_pixmap, m_gcontexts.at(gc::BL), 0, 0,
              m_borders[border::LEFT].size, m_bar.height);
        }
        break;

      case border::RIGHT:
        if (m_borders[border::RIGHT].size > 0) {
          draw_util::fill(m_connection, m_pixmap, m_gcontexts.at(gc::BR),
              m_bar.width - m_borders[border::RIGHT].size, 0, m_borders[border::RIGHT].size,
              m_bar.height);
        }
        break;

      case border::ALL:
        draw_border(border::TOP);
        draw_border(border::BOTTOM);
        draw_border(border::LEFT);
        draw_border(border::RIGHT);
        break;
    }
  }  //}}}

  /**
   * Draw over- and underline onto the pixmap
   */
  void draw_lines(int x, int w) {  //{{{
    if (!m_bar.lineheight)
      return;

    if (m_attributes & static_cast<int>(attribute::o))
      draw_util::fill(m_connection, m_pixmap, m_gcontexts.at(gc::OL), x,
          m_borders[border::TOP].size, w, m_bar.lineheight);

    if (m_attributes & static_cast<int>(attribute::u))
      draw_util::fill(m_connection, m_pixmap, m_gcontexts.at(gc::UL), x,
          m_bar.height - m_borders[border::BOTTOM].size - m_bar.lineheight, w, m_bar.lineheight);
  }  //}}}

  /**
   * Shift the contents of the pixmap horizontally
   */
  int draw_shift(int x, int chr_width) {  //{{{
    int delta = chr_width;

    if (m_bar.align == alignment::CENTER) {
      int base_x = m_bar.width;
      base_x -= m_borders[border::RIGHT].size;
      base_x /= 2;
      base_x += m_borders[border::LEFT].size;
      m_connection.copy_area(m_pixmap, m_pixmap, m_gcontexts.at(gc::FG), base_x - x / 2, 0,
          base_x - (x + chr_width) / 2, 0, x, m_bar.height);
      x = base_x - (x + chr_width) / 2 + x;
      delta /= 2;
    } else if (m_bar.align == alignment::RIGHT) {
      m_connection.copy_area(m_pixmap, m_pixmap, m_gcontexts.at(gc::FG), m_bar.width - x, 0,
          m_bar.width - x - chr_width, 0, x, m_bar.height);
      x = m_bar.width - chr_width - m_borders[border::RIGHT].size;
    }

    draw_util::fill(m_connection, m_pixmap, m_gcontexts.at(gc::BG), x, 0, chr_width, m_bar.height);

    // Translate pos of clickable areas
    if (m_bar.align != alignment::LEFT)
      for (auto&& action : m_actions) {
        if (action.active || action.align != m_bar.align)
          continue;
        action.start_x -= delta;
        action.end_x -= delta;
      }

    return x;
  }  //}}}

  /**
   * Draw text contents
   */
  void draw_character(uint16_t character) {  // {{{
    // TODO: cache
    auto& font = m_fontmanager->match_char(character);

    if (!font) {
      m_log.warn("No suitable font found for character at index %i", character);
      return;
    }

    if (font->ptr && font->ptr != m_gcfont) {
      m_gcfont = font->ptr;
      m_fontmanager->set_gcontext_font(m_gcontexts.at(gc::FG), m_gcfont);
    }

    // TODO: cache
    auto chr_width = m_fontmanager->char_width(font, character);

    // Avoid odd glyph width's for center-aligned text
    // since it breaks the positioning of clickable area's
    if (m_bar.align == alignment::CENTER && chr_width % 2)
      chr_width++;

    auto x = draw_shift(m_xpos, chr_width);
    auto y = m_bar.vertical_mid + font->height / 2 - font->descent + font->offset_y;

    // m_log.trace("Draw char(%c, width: %i) at pos(%i,%i)", character, chr_width, x, y);

    if (font->xft != nullptr) {
      auto color = m_fontmanager->xftcolor();
      XftDrawString16(m_xftdraw, &color, font->xft, x, y, &character, 1);
    } else {
      character = (character >> 8) | (character << 8);
      draw_util::xcb_poly_text_16_patched(
          m_connection, m_pixmap, m_gcontexts.at(gc::FG), x, y, 1, &character);
    }

    draw_lines(x, chr_width);
    m_xpos += chr_width;
  }  // }}}

 private:
  connection& m_connection;
  const config& m_conf;
  const logger& m_log;
  unique_ptr<fontmanager> m_fontmanager;

  threading_util::spin_lock m_lock;

  xcb_screen_t* m_screen;
  xcb_visualtype_t* m_visual;

  window m_window{m_connection};
  colormap m_colormap{m_connection, m_connection.generate_id()};
  pixmap m_pixmap{m_connection, m_connection.generate_id()};

  bar_settings m_bar;
  tray_settings m_tray;
  map<border, border_settings> m_borders;
  map<gc, gcontext> m_gcontexts;
  vector<action_block> m_actions;

  stateflag m_sinkattached{false};

  string m_prevdata;
  int m_xpos{0};
  int m_attributes{0};

  xcb_font_t m_gcfont{0};
  XftDraw* m_xftdraw;
};

LEMONBUDDY_NS_END