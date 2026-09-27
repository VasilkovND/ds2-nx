// nfsmw - PC build: unmasked floating-point exceptions.
//
// What happens
//   The PC build with the Switch SDK (tools/pc) closes after 4 s with 0xC000008F
//   (STATUS_FLOAT_INEXACT_RESULT), with and without the audio rescue. On x86 an MXCSR mask
//   bit at 0 unmasks that exception, and rex::ppc::FPSCR writes its cached copy "csr" to
//   MXCSR: if that copy is 0 on a thread that did not go through InitHost(), the first
//   inexact result kills the process. On Switch (AArch64) the same 0 leaves the traps off,
//   which is why it does not happen there.
//
// What it does
//   A Windows vectored exception handler that, on a floating-point exception, masks MXCSR
//   and the thread's x87 FPU again and lets execution continue. With everything masked the
//   operation yields the default result, which is what the console does with exceptions
//   off. Each new address is logged from a separate thread: the handler takes no locks,
//   because the exception can fire inside the logger itself (it formats floats).
//
// Windows only: the Switch does not need it.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <rex/logging.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

namespace {

constexpr DWORD kCodigosComaFlotante[] = {
    0xC000008D,  // STATUS_FLOAT_DENORMAL_OPERAND
    0xC000008E,  // STATUS_FLOAT_DIVIDE_BY_ZERO
    0xC000008F,  // STATUS_FLOAT_INEXACT_RESULT
    0xC0000090,  // STATUS_FLOAT_INVALID_OPERATION
    0xC0000091,  // STATUS_FLOAT_OVERFLOW
    0xC0000092,  // STATUS_FLOAT_STACK_CHECK
    0xC0000093,  // STATUS_FLOAT_UNDERFLOW
    0xC00002B4,  // STATUS_FLOAT_MULTIPLE_FAULTS
    0xC00002B5,  // STATUS_FLOAT_MULTIPLE_TRAPS
};

struct Sitio {
  std::atomic<uint64_t> rip{0};
  std::atomic<uint32_t> codigo{0};
  std::atomic<uint32_t> mxcsr{0};
  std::atomic<uint32_t> hilo{0};
  std::atomic<uint64_t> veces{0};
  bool anotado = false;  // only the reporter thread touches it
};

// Filled in order without allocating: the handler cannot allocate.
std::array<Sitio, 64> g_sitios;
std::atomic<uint64_t> g_total{0};

Sitio* BuscarOCrear(uint64_t rip) {
  for (Sitio& sitio : g_sitios) {
    uint64_t actual = sitio.rip.load(std::memory_order_acquire);
    if (actual == rip) {
      return &sitio;
    }
    if (actual == 0) {
      if (sitio.rip.compare_exchange_strong(actual, rip, std::memory_order_acq_rel)) {
        return &sitio;
      }
      if (actual == rip) {
        return &sitio;
      }
    }
  }
  return nullptr;  // tabla llena
}

LONG CALLBACK ManejadorComaFlotante(EXCEPTION_POINTERS* info) {
  const DWORD codigo = info->ExceptionRecord->ExceptionCode;
  bool es_coma_flotante = false;
  for (DWORD c : kCodigosComaFlotante) {
    es_coma_flotante |= (c == codigo);
  }
  if (!es_coma_flotante) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  CONTEXT* contexto = info->ContextRecord;
  const DWORD mxcsr = contexto->MxCsr;
  // All masks to 1 and the sticky flags to 0, in SSE and in x87.
  contexto->MxCsr = (mxcsr | 0x1F80) & ~DWORD(0x3F);
  contexto->FltSave.MxCsr = contexto->MxCsr;
  contexto->FltSave.ControlWord |= 0x3F;
  contexto->FltSave.StatusWord &= ~WORD(0xFF);

  g_total.fetch_add(1, std::memory_order_relaxed);
  if (Sitio* sitio = BuscarOCrear(contexto->Rip)) {
    if (sitio->veces.load(std::memory_order_acquire) == 0) {
      sitio->codigo.store(codigo, std::memory_order_release);
      sitio->mxcsr.store(mxcsr, std::memory_order_release);
      sitio->hilo.store(GetCurrentThreadId(), std::memory_order_release);
    }
    sitio->veces.fetch_add(1, std::memory_order_relaxed);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

void Informador() {
  uint64_t total_anotado = 0;
  for (;;) {
    Sleep(2000);
    for (Sitio& sitio : g_sitios) {
      const uint64_t rip = sitio.rip.load(std::memory_order_acquire);
      if (!rip) {
        break;
      }
      if (sitio.anotado || sitio.veces.load(std::memory_order_acquire) == 0) {
        continue;
      }
      sitio.anotado = true;
      HMODULE modulo = nullptr;
      char nombre[MAX_PATH] = "?";
      if (GetModuleHandleExA(
              GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
              reinterpret_cast<LPCSTR>(rip), &modulo)) {
        GetModuleFileNameA(modulo, nombre, MAX_PATH);
      }
      const uint64_t base = uint64_t(reinterpret_cast<uintptr_t>(modulo));
      REXLOG_WARN(
          "[pc] excepcion de coma flotante {:08X} en {}+0x{:X} (MXCSR {:08X}, hilo {}); se "
          "enmascara y sigue",
          sitio.codigo.load(), nombre, rip - base, sitio.mxcsr.load(), sitio.hilo.load());
    }
    const uint64_t total = g_total.load(std::memory_order_relaxed);
    if (total != total_anotado) {
      total_anotado = total;
      REXLOG_INFO("[pc] excepciones de coma flotante enmascaradas hasta ahora: {}", total);
    }
  }
}

struct Registro {
  Registro() {
    AddVectoredExceptionHandler(1, &ManejadorComaFlotante);
    std::thread(Informador).detach();
  }
};
Registro g_registro;

}  // namespace

#endif  // _WIN32
