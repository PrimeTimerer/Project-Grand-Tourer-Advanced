// [GT6 0132] Profile-guided optimisation, the instrumented build only (CMake GT6_PGO=GEN). A test run ends by stopping
// the process, and a stopped process writes no profile at exit - so this thread writes the counters to the profile
// file (default_*.profraw in the working directory, or LLVM_PROFILE_FILE) every 20 seconds.
#ifdef GT6_PGO_GEN
#include <windows.h>

extern "C" int __llvm_profile_write_file(void);

namespace {
DWORD WINAPI pgoThread(void*)
{
    for (;;) {
        Sleep(20000);
        __llvm_profile_write_file();
    }
}
struct PgoStart {
    PgoStart() { CloseHandle(CreateThread(nullptr, 0, pgoThread, nullptr, 0, nullptr)); }
} g_pgoStart;
}   // namespace
#endif
