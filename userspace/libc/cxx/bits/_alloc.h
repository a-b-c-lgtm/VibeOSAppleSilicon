// userspace/libc/cxx/bits/_alloc.h — chapter 186
//
// Internal helper: provide std::allocator + std::char_traits in
// one place so both <string> and <memory> can include it without
// duplicate definitions.
#ifndef _OSDEV_CXX_BITS_ALLOC
#define _OSDEV_CXX_BITS_ALLOC 1

#include "../../malloc.h"

namespace std {

template <class _CharT> struct char_traits {
    typedef _CharT char_type;
    static unsigned long length(const _CharT *s) {
        unsigned long n = 0;
        while (s[n]) ++n;
        return n;
    }
    static int compare(const _CharT *a, const _CharT *b, unsigned long n) {
        for (unsigned long i = 0; i < n; ++i) {
            if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        }
        return 0;
    }
};

template <class _T> class allocator {
public:
    typedef _T value_type;
    allocator() noexcept = default;
    template <class _U> allocator(const allocator<_U> &) noexcept {}
    _T *allocate(unsigned long n) { return (_T *)::malloc(n * sizeof(_T)); }
    void deallocate(_T *p, unsigned long) { ::free(p); }
};

}  // namespace std

#endif
