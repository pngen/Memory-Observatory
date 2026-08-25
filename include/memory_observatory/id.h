// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Strong, non-numeric public identifiers. Stable public identifiers are
// never raw pointers: they are integer-backed strong handles that are safe
// to copy, compare, hash, and serialize.

#ifndef MEMORY_OBSERVATORY_ID_HPP
#define MEMORY_OBSERVATORY_ID_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <ostream>

namespace memory_observatory {

// A strong, opaque identifier. The Tag type distinguishes identifiers with
// the same underlying integer representation. Value 0 is reserved as the
// invalid / unset identifier.
template <typename Tag>
class Id {
 public:
  using TagType = Tag;

  constexpr Id() noexcept = default;  // invalid
  constexpr explicit Id(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return valid();
  }

  constexpr bool operator==(const Id&) const noexcept = default;
  constexpr auto operator<=>(const Id&) const noexcept = default;

  // Increments the underlying value to mint the next identifier. 0 stays 0;
  // UINT64_MAX stays UINT64_MAX (callers must not rely on wrap-around for
  // correctness).
  constexpr Id next() const noexcept {
    return value_ == UINT64_MAX ? Id{} : Id{value_ + 1};
  }

  Id& operator++() noexcept {
    if (value_ != UINT64_MAX) {
      ++value_;
    }
    return *this;
  }

  friend std::ostream& operator<<(std::ostream& os, const Id& id) {
    if (id.valid()) {
      os << id.value();
    } else {
      os << "<invalid>";
    }
    return os;
  }

 private:
  std::uint64_t value_ = 0;
};

// Hash support so strong identifiers can key unordered containers.
template <typename Tag>
struct IdHash {
  std::size_t operator()(const Id<Tag>& id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_ID_HPP
