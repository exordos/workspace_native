#include "native-diagnostics.h"

// The diagnostics collector core is header-only pure math (see the header);
// this translation unit keeps the module shape consistent with the rest of
// the core library so the build can grow non-inline helpers here later.
namespace workspace::diagnostics {
} // namespace workspace::diagnostics
