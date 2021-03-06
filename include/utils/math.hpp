#pragma once

#include <algorithm>

#include "common.hpp"

POLYBAR_NS

namespace math_util {
  /**
   * Limit value T by min and max bounds
   */
  template <typename ValueType>
  ValueType cap(ValueType value, ValueType min_value, ValueType max_value) {
    value = std::min<ValueType>(value, max_value);
    value = std::max<ValueType>(value, min_value);
    return value;
  }

  /**
   * Calculate the percentage for a value
   * within given range
   */
  template <typename ValueType, typename ReturnType = int>
  ReturnType percentage(ValueType value, ValueType min_value, ValueType max_value) {
    auto upper = (max_value - min_value);
    auto lower = static_cast<float>(value - min_value);
    ValueType percentage = (lower / upper) * 100.0f;
    if (std::is_integral<ReturnType>())
      percentage += 0.5f;
    return cap<ReturnType>(percentage, 0.0f, 100.0f);
  }

  /**
   * Get value for percentage of `max_value`
   */
  template <typename ValueType, typename ReturnType = int>
  ReturnType percentage_to_value(ValueType percentage, ValueType max_value) {
    if (std::is_integral<ReturnType>())
      return cap<ReturnType>(percentage * max_value / 100.0f + 0.5f, 0, max_value);
    else
      return cap<ReturnType>(percentage * max_value / 100.0f, 0.0f, max_value);
  }

  /**
   * Get value for percentage of `min_value` to `max_value`
   */
  template <typename ValueType, typename ReturnType = int>
  ReturnType percentage_to_value(ValueType percentage, ValueType min_value, ValueType max_value) {
    if (std::is_integral<ReturnType>())
      return cap<ReturnType>(percentage * (max_value - min_value) / 100.0f + 0.5f, 0, max_value - min_value) + min_value;
    else
      return cap<ReturnType>(percentage * (max_value - min_value) / 100.0f, 0.0f, max_value - min_value) + min_value;
  }
}

POLYBAR_NS_END
