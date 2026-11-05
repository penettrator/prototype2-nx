// SPDX-License-Identifier: GPL-3.0-or-later
#include "resource_failure_probe.h"
#include "stream_heap_budget.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <atomic>
REXCVAR_DEFINE_BOOL(p2_resource_failure_probe, false, "Prototype2",
    "Failure-only guest resource diagnostics; requires restart")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
namespace {
std::atomic<unsigned> failures{0};
bool Record() {
  return REXCVAR_GET(p2_resource_failure_probe) &&
      failures.fetch_add(1, std::memory_order_relaxed) < 16;
}
}
extern "C" void P2ResourceAllocationFailed(uint32_t bytes, uint32_t flags,
    uint32_t metadata, uint32_t caller, uintptr_t context) {
  if (!Record()) return;
  // Metadata may already have been freed. Never dereference it.
  REXLOG_ERROR("[p2-resource-failure] allocation bytes={} flags={:08X} metadata={:08X} stage={} caller={:08X} context={:X}",
      bytes, flags, metadata, metadata ? "payload" : "metadata", caller, context);
  rex::FlushLogging();
}
extern "C" void P2ResourceMissingBeforeLock(uint32_t object, uint32_t descriptor,
    uintptr_t context) {
  if (!Record()) return;
  // Do not dereference suspect guest pointers or hide the original fault.
  REXLOG_ERROR("[p2-resource-failure] null resource before lock caller=82C02048 object={:08X} descriptor={:08X} context={:X}",
      object, descriptor, context);
  rex::FlushLogging();
}

extern "C" void P2AllocatorFailureMessage(uint32_t format, const uint32_t* a,
    unsigned count, uint32_t caller, uintptr_t context) {
  const unsigned expected = format == 0x8200A2D0 ? 9 :
      format == 0x8200A388 ? 13 : format == 0x8200A4E8 ? 12 : 0;
  if (!a || !expected || count != expected || !Record()) return;
  REXLOG_ERROR("[p2-resource-failure] engine allocation bytes={} allocator={} name_ptr={:08X} type_ptr={:08X} total_size={} caller={:08X} format={:08X} context={:X}",
      a[0], a[1], a[2], a[3], a[4], caller, format, context);
  if (format == 0x8200A4E8) {
    REXLOG_ERROR("[p2-resource-failure] segments free={} used={} total={} segment_bytes={}; current_segment bytes_free={} bytes_used={} allocs={}",
        a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
  } else {
    REXLOG_ERROR("[p2-resource-failure] {} memory bytes_free={} bytes_used={} bytes_total={} allocs={}",
        format == 0x8200A388 ? "normal" : "heap", a[5], a[6], a[7], a[8]);
    if (format == 0x8200A388)
      REXLOG_ERROR("[p2-resource-failure] other memory bytes_free={} bytes_used={} bytes_total={} allocs={}", a[9], a[10], a[11], a[12]);
  }
  P2StreamHeapFailureReport(a[1]);
  rex::FlushLogging();
}
