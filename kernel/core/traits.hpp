#pragma once

namespace Kernel::Traits {

// Primary template: evaluates to false for all non-boolean types
template <typename T> struct is_bool { static constexpr bool value = false; };

// Explicit specializations for bool (including cv-qualified variants)
template <> struct is_bool<bool> { static constexpr bool value = true; };

template <> struct is_bool<const bool> { static constexpr bool value = true; };

template <> struct is_bool<volatile bool> { static constexpr bool value = true; };

template <> struct is_bool<const volatile bool> { static constexpr bool value = true; };

// C++17/20 inline variable template helper
template <typename T> inline constexpr bool is_bool_v = is_bool<T>::value;

} // namespace Kernel::Traits