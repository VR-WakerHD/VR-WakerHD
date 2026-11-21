// The controller read's fault guard (2026-11-16, 0.5.1): the features that run inside the game's GamePad read
// (VPADRead, hle/system_stubs.cpp: the touch talk helper, the item ring, the notebook and Delivery Bag pages, the
// settings page, menu sounds, renaming, code mods' filters ...) read the game's objects. A read through a game object
// that did not exist (an empty pointer: guest address 0x400) ended the game there: two 0.5.0 players, Linux and
// Windows, USA and EUR games, the GamePad controls, each time as a conversation began (crash logs of 2026-11-14/15).
//
// run(what, f) calls f; a fault in the game's memory while f runs (SIGSEGV / SIGBUS at a guest address, on this
// thread) returns to run instead of ending the game: that feature does nothing more this read, the controller input
// still reaches the game, and the log names the feature once (note, hle/system_stubs.cpp). Every other fault (host
// memory, another thread, outside a guarded feature) is still the crash it was, with its report. main.cpp's crash
// handler calls recover first.
//
// Limits: a feature left by the jump does not run its destructors from the fault on (its locals; a mutex it held
// stays held), so the guarded features read the game's memory outside their locks. Windows: not guarded yet (the
// signal mechanism is POSIX's); run only calls f there.
#pragma once
#include <cstdint>
#include "ppc.h"
#ifndef _WIN32
#include <csetjmp>
#include <csignal>
#endif

namespace pad_fault_guard {

// the log line for a guarded fault (once per feature, then a count): hle/system_stubs.cpp
void note(const char* what, uint32_t guest);

#ifndef _WIN32
struct State {
    sigjmp_buf* jump = nullptr;  // the innermost guarded feature's return point on this thread; null: none
    const char* what = nullptr;
    uint32_t guest = 0;          // the faulting guest address
};
inline thread_local State g_state;

// the crash handler's first step: a fault in the game's memory inside a guarded feature on this thread jumps back to
// it (async-signal-safe: thread-local reads and siglongjmp only); otherwise returns, and the crash report follows
inline void recover(int sig, uintptr_t addr) {
    State& s = g_state;
    if (!s.jump || (sig != SIGSEGV && sig != SIGBUS)) return;
    const uintptr_t base = (uintptr_t)PPC_MEM_BASE;
    if (addr < base || addr >= base + 0x100000000ull) return;
    s.guest = uint32_t(addr - base);
    sigjmp_buf* j = s.jump;
    s.jump = nullptr;  // (a second fault while leaving is a crash, not a loop)
    siglongjmp(*j, 1);
}

// f run under the guard; false when it faulted in the game's memory (it was left there)
template <class F>
bool run(const char* what, F&& f) {
    State& s = g_state;
    sigjmp_buf* const outer = s.jump;
    const char* const outerWhat = s.what;
    sigjmp_buf jb;
    s.what = what;
    s.jump = &jb;
    if (sigsetjmp(jb, 1) == 0) {  // (1: the signal mask saved; siglongjmp unblocks SIGSEGV again)
        f();
        s.jump = outer;
        s.what = outerWhat;
        return true;
    }
    const uint32_t at = s.guest;
    s.jump = outer;
    s.what = outerWhat;
    note(what, at);
    return false;
}
#else
inline void recover(int, uintptr_t) {}
template <class F>
bool run(const char*, F&& f) {
    f();
    return true;
}
#endif

}  // namespace pad_fault_guard
