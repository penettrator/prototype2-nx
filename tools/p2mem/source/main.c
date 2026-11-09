// Prototype 2 Switch: p2mem, the helper that lends the game memory from the applet pool.
//
// The game runs as an application and the kernel only lets it allocate from the application pool. While it
// runs, the applet pool is mostly idle (measured: 99 of 500 MB used). This process is declared in the applet
// pool (p2mem.json, pool_partition 1), so its heap comes from there. It registers the service "p2:mem",
// accepts one session (the game), allocates the requested size, wraps it in transfer memory with no access
// left to itself, and copies the handle to the game. It then waits; when the game closes the session or its
// process ends, the session dies and this process exits, which returns the memory to the applet pool.
//
// It is never started at boot (no boot2.flag): the game launches it with pm:shell when
// p2_applet_memory_mb > 0. Command 0: in u64 requested bytes, out u64 granted bytes + copy handle.
#include <string.h>
#include <switch.h>

#define INNER_HEAP_SIZE 0x10000

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void) {
  static u8 inner_heap[INNER_HEAP_SIZE];
  extern void* fake_heap_start;
  extern void* fake_heap_end;
  fake_heap_start = inner_heap;
  fake_heap_end = inner_heap + sizeof(inner_heap);
}

void __appInit(void) {
  if (R_FAILED(smInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
}

void __appExit(void) { smExit(); }

static void Reply(Result result, u64 granted, Handle copy) {
  HipcRequest w = hipcMakeRequestInline(armGetTls(), .type = 0, .num_data_words = 4 + 4 + 2,
                                        .num_copy_handles = copy != INVALID_HANDLE ? 1 : 0);
  if (copy != INVALID_HANDLE) w.copy_handles[0] = copy;
  CmifOutHeader* out = (CmifOutHeader*)cmifGetAlignedDataStart(w.data_words, armGetTls());
  out->magic = CMIF_OUT_HEADER_MAGIC;
  out->version = 0;
  out->result = result;
  out->token = 0;
  memcpy(out + 1, &granted, sizeof(granted));
}

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;
  const SmServiceName name = smEncodeName("p2:mem");
  Handle port = INVALID_HANDLE;
  if (R_FAILED(smRegisterService(&port, name, false, 1))) return 0;

  // The game connects right after launching us; if it does not within 15 s, leave.
  s32 index = 0;
  Handle session = INVALID_HANDLE;
  Result rc = svcWaitSynchronization(&index, &port, 1, 15000000000ull);
  if (R_SUCCEEDED(rc)) rc = svcAcceptSession(&session, port);
  smUnregisterService(name);
  svcCloseHandle(port);
  if (R_FAILED(rc)) return 0;

  void* heap = NULL;
  u64 granted = 0;
  Handle tmem = INVALID_HANDLE;
  Handle reply_to = INVALID_HANDLE;
  for (;;) {
    rc = svcReplyAndReceive(&index, &session, 1, reply_to, UINT64_MAX);
    reply_to = INVALID_HANDLE;
    if (R_FAILED(rc)) break;  // the game closed the session or died
    const HipcParsedRequest req = hipcParseRequest(armGetTls());
    if (req.meta.type == CmifCommandType_Close) break;
    const CmifInHeader* in = (const CmifInHeader*)cmifGetAlignedDataStart(req.data.data_words, armGetTls());
    u64 want = 0;
    memcpy(&want, in + 1, sizeof(want));
    Result result = 0;
    if (req.meta.type != CmifCommandType_Request || in->magic != CMIF_IN_HEADER_MAGIC || in->command_id != 0 ||
        tmem != INVALID_HANDLE) {
      Reply(MAKERESULT(Module_Libnx, LibnxError_BadInput), 0, INVALID_HANDLE);
      reply_to = session;
      continue;
    }
    // Heap sizes are multiples of 2 MiB. Back off in 32 MiB steps if the pool has less than asked.
    u64 size = want & ~(u64)0x1FFFFF;
    while (size >= 0x2000000) {
      if (R_SUCCEEDED(svcSetHeapSize(&heap, size))) break;
      size -= 0x2000000;
    }
    if (size < 0x2000000) {
      result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    } else if (R_FAILED(result = svcCreateTransferMemory(&tmem, heap, size, Perm_None))) {
      tmem = INVALID_HANDLE;
      svcSetHeapSize(&heap, 0);
    } else {
      granted = size;
    }
    Reply(result, granted, tmem);
    reply_to = session;
  }
  svcCloseHandle(session);
  if (tmem != INVALID_HANDLE) svcCloseHandle(tmem);
  if (granted) svcSetHeapSize(&heap, 0);
  return 0;
}
