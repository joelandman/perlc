/* Force complete definitions of libstdc++ containers and iterators
   before any other header.  This defeats the LLVM 18 Orc/ADT incomplete
   iterator problem on this clang-18 + gcc-16 libstdc++ host.
*/
#ifndef PERLC_FORCE_COMPLETE_STD_H
#define PERLC_FORCE_COMPLETE_STD_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cassert>

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <iterator>
#include <utility>
#include <functional>
#include <array>
#include <deque>
#include <list>
#include <queue>
#include <stack>
#include <bitset>
#include <limits>

#include <bits/stl_iterator.h>
#include <bits/stl_iterator_base_types.h>
#include <bits/stl_iterator_base_funcs.h>
#include <bits/stl_construct.h>
#include <bits/stl_uninitialized.h>

#endif
