// Sorting without std::sort's and std::stable_sort's code size: each of their uses instantiates
// its own introsort or merge sort for the element type and the comparison, 0.6 to 2 KB a time
// (27 KB over the firmware's 21 uses, measured 2026-11-24). sorting::stable() shares one merge
// sort, over indices, and a use only adds its comparison and the loop that moves the elements
// into place.
//
// Stable, O(n log n) comparisons, and n moves or so; it takes 8 bytes of heap an element while
// it runs. For a few plain numbers in a hot loop std::sort is still the one (ui/icons.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <utility>

namespace sorting {
namespace detail {

using Less = bool (*)(const void *context, uint32_t a, uint32_t b);

// Puts 0..n-1 into order[0..n), sorted stably by less. scratch has room for n more.
void order(uint32_t *order, uint32_t *scratch, size_t n, Less less, const void *context);

}  // namespace detail

// Sorts [first, last) by less, keeping equal elements in the order they had. Random-access
// iterators (a vector's, or pointers into an array); the elements are moved, not copied.
template <typename It, typename Less>
void stable(It first, It last, Less less) {
  const size_t n = static_cast<size_t>(last - first);
  if (n < 2) return;
  const std::unique_ptr<uint32_t[]> index(new uint32_t[2 * n]);
  struct Context {
    It first;
    Less *less;
  } context{first, &less};
  detail::order(
      index.get(), index.get() + n, n,
      [](const void *c, uint32_t a, uint32_t b) -> bool {
        const Context &x = *static_cast<const Context *>(c);
        return (*x.less)(x.first[a], x.first[b]);
      },
      &context);
  // index[i] is where the element for place i is now. Each cycle of that permutation is walked
  // once: one element is held aside, the others each move straight to their place.
  for (size_t i = 0; i < n; ++i) {
    if (index[i] == i) continue;
    auto held = std::move(first[i]);
    size_t to = i;
    for (;;) {
      const size_t from = index[to];
      index[to] = static_cast<uint32_t>(to);
      if (from == i) break;
      first[to] = std::move(first[from]);
      to = from;
    }
    first[to] = std::move(held);
  }
}

// By operator<.
template <typename It>
void stable(It first, It last) {
  stable(first, last, std::less<>());
}

}  // namespace sorting
