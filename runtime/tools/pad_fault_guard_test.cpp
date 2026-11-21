// pad_fault_guard.h: a read through an empty game pointer inside a guarded feature returns to the guard (the game
// goes on), the feature named once; the guard is left clean (nested and later runs work), and a fault outside any
// guarded feature still reaches the crash handler's own report.
#include "../src/pad_fault_guard.h"
#include <cassert>
#include <cstdio>
#include <string>
#ifndef _WIN32
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static std::string g_noted;
static uint32_t g_notedAt = 0;
static int g_notes = 0;
void pad_fault_guard::note(const char* what, uint32_t guest) {
    g_noted = what;
    g_notedAt = guest;
    ++g_notes;
}

#ifndef _WIN32
static volatile sig_atomic_t g_reported = 0;
static void handler(int sig, siginfo_t* si, void*) {
    pad_fault_guard::recover(sig, (uintptr_t)si->si_addr);  // (main.cpp's crash handler, first step)
    g_reported = 1;  // the crash report would follow here
    _exit(3);
}
static uint32_t read_guest(uint32_t ea) { return *(volatile uint32_t*)(PPC_MEM_BASE + ea); }
#endif

int main() {
#ifndef _WIN32
    // the guest's first page unmapped, as in the game (null + 0x400 faults)
    // (reserved without access; where the address is taken, it is unmapped all the same: either way a read faults)
    (void)mmap(PPC_MEM_BASE, 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);

    // a feature that runs normally
    int ran = 0;
    assert(pad_fault_guard::run("fine", [&] { ran = 1; }));
    assert(ran == 1 && g_notes == 0);

    // a feature reading through an empty game pointer: left there, named, the caller goes on
    int after = 0;
    const bool ok = pad_fault_guard::run("touch talk", [&] {
        (void)read_guest(0x400);
        after = 1;  // (never reached)
    });
    assert(!ok && after == 0);
    assert(g_notes == 1 && g_noted == "touch talk" && g_notedAt == 0x400);

    // the guard is left clean: the same and a nested run work again
    assert(pad_fault_guard::run("again", [&] {
        assert(!pad_fault_guard::run("inner", [&] { (void)read_guest(0x404); }));
    }));
    assert(g_notes == 2 && g_noted == "inner" && g_notedAt == 0x404);
    assert(pad_fault_guard::g_state.jump == nullptr);

    // a fault outside any guarded feature is still a crash: the handler's report path runs (exit 3)
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        (void)read_guest(0x400);
        _exit(5);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 3);
    std::puts("pad_fault_guard: ok");
#else
    int ran = 0;
    assert(pad_fault_guard::run("fine", [&] { ran = 1; }) && ran == 1);
    std::puts("pad_fault_guard: ok (not guarded on Windows)");
#endif
    return 0;
}
