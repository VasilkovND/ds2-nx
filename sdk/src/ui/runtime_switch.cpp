/**
 * @file        rex/os/switch/runtime_switch.cpp
 * @brief       libnx startup constants for the Switch executable
 *
 * This file is compiled directly into the final executable, not into a
 * library.
 *
 * Everything here replaces weak libnx symbols, and crt0 only references them
 * weakly. A weak reference does not pull an object out of a static library, so
 * if this ended up in rexcore.a the linker would silently ignore it and the
 * defaults would be used: no error, no warning, and the process starting in a
 * different state than expected. skate3recomp-nx solves it the same way, adding
 * it to the game target.
 *
 * The values are those of MarathonRecomp-NX, a finished Horizon port.
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include <cxxabi.h>

#include <switch.h>

extern "C" void RexSwitchCrashLog(const char* reason, const ThreadExceptionDump* ctx, u64 stack,
                                  u64 pc);

namespace {

/*
 * An uncaught exception. devkitA64's libstdc++ handler writes nothing
 * (rex_stderr.log came out empty when SetupVfs let a filesystem_error
 * escape), so the type and what() are recorded here; for std::filesystem
 * errors what() includes the path. It ends with error 2345-0104.
 */
[[noreturn]] void RexSwitchTerminate() {
  char reason[768] = "std::terminate sin excepcion activa";
  if (std::exception_ptr current = std::current_exception()) {
    const std::type_info* type = abi::__cxa_current_exception_type();
    int status = -1;
    char* demangled = type ? abi::__cxa_demangle(type->name(), nullptr, nullptr, &status) : nullptr;
    const char* type_name = demangled ? demangled : (type ? type->name() : "?");
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& e) {
      std::snprintf(reason, sizeof(reason), "std::terminate: %s: %s", type_name, e.what());
    } catch (...) {
      std::snprintf(reason, sizeof(reason), "std::terminate: %s", type_name);
    }
    std::free(demangled);
  }
  RexSwitchCrashLog(reason, nullptr, reinterpret_cast<u64>(__builtin_frame_address(0)),
                    reinterpret_cast<u64>(__builtin_return_address(0)));
  diagAbortWithResult(MAKERESULT(Module_Libnx, 104));
}

__attribute__((constructor(101))) void RexSwitchInstallTerminate() {
  std::set_terminate(RexSwitchTerminate);
}

}  // namespace

extern "C" {

/*
 * Declared as an application instead of an applet. It goes with title takeover,
 * which was measured to be mandatory: in applet mode the process gets 400 MB, and
 * guest physical memory alone is 512.
 */
u32 __nx_applet_type = AppletType_Application;

/*
 * The heap size libnx asks for when nothing hands the program a heap. Started
 * by hbloader (the Homebrew Menu or a forwarder), which is how this port runs,
 * libnx takes the heap hbloader already set up, all the memory but about 2 MB,
 * and ignores this value.
 *
 * That is why the profiler reads 3,185 of 3,189 MB: InfoType_UsedMemorySize
 * counts that whole heap from the start. It was the same with 1,536 as with
 * 1,024 MB here, and already before anything was loaded, so it does not show
 * how much is free.
 *
 * Measured on the console: the game died with "no se pudo confirmar 0x1000
 * bytes" (kernel 2001-0103) at ~1.8 GB mapped, and even a 4 KB request and
 * thread creation failed. Changing this value did not move that point; what
 * fixed it was mapping each guest chunk into a view only when that view touches
 * it (guest_memory_switch.cpp).
 *
 * Everything the port uses comes out of that heap: the guest backing (measured
 * at 506 MB), the host thread stacks (16 MB each) and Mesa, GPU memory included.
 * How much of it the GPU uses shows in Vulkan's memory budget.
 */
size_t __nx_heap_size = 1024ull * 1024 * 1024;  /* see below */

/*
 * The exception handler stack is no longer declared here.
 *
 * libnx has a single one for the whole process, and with twenty-odd threads
 * faulting at the same time they overwrote each other. There are now eight
 * stack and dump sets in exception_handler_switch.cpp, which hands them out.
 */

}  // extern "C"
