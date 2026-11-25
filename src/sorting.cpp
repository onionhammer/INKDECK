#include "sorting.h"

#include <string.h>

#include <algorithm>

namespace sorting::detail {

// A bottom-up merge sort of the indices, between `order` and `scratch`.
void order(uint32_t *order, uint32_t *scratch, size_t n, Less less, const void *context) {
  for (size_t i = 0; i < n; ++i) order[i] = static_cast<uint32_t>(i);
  uint32_t *from = order;
  uint32_t *to = scratch;
  for (size_t width = 1; width < n; width *= 2) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      const size_t mid = std::min(lo + width, n);
      const size_t hi = std::min(lo + 2 * width, n);
      size_t i = lo, j = mid, k = lo;
      // The right run's element goes first only when it's less: equal ones keep their order.
      while (i < mid && j < hi) to[k++] = less(context, from[j], from[i]) ? from[j++] : from[i++];
      while (i < mid) to[k++] = from[i++];
      while (j < hi) to[k++] = from[j++];
    }
    std::swap(from, to);
  }
  if (from != order) memcpy(order, from, n * sizeof *order);
}

}  // namespace sorting::detail
