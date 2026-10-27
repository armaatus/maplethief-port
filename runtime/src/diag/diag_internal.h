// Shared between the diagnostics sources (ADR 0030); not part of maplethief/diag.h.
#ifndef MAPLETHIEF_RUNTIME_DIAG_DIAG_INTERNAL_H
#define MAPLETHIEF_RUNTIME_DIAG_DIAG_INTERNAL_H

#include <cstdint>

extern "C" {

// The name of import `id` of the table mt_imports_link last linked, or nullptr.
const char *mt_diag_import_name(std::uint32_t id) noexcept;

} // extern "C"

#endif // MAPLETHIEF_RUNTIME_DIAG_DIAG_INTERNAL_H
