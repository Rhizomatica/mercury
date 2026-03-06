#ifndef CANARY_GUARD_H_
#define CANARY_GUARD_H_

// Canary guard system — DISABLED (passthrough mode).
// CNEW/CDELETE macros kept so call sites don't need changes.

#include <cstdlib>

#define CNEW(type, count, name) (new type[(size_t)(count)])
#define CDELETE(ptr) do { if(ptr) { delete[] (ptr); (ptr) = NULL; } } while(0)

inline int canary_check_all() { return 0; }
inline void canary_clear() {}

#endif
