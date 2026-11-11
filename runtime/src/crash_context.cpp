#include "crash_memory.h"
#include "runtime.h"
#include "crash_context.h"
#include "crash_redact.h"
#include "gfx/renderer.h"
#include "input.h"
#include "interp.h"
#include "mods/manager.h"
#include "mods/packages.h"
#include "render_objects.h"
#include <atomic>
#include <cstdlib>
#include <sstream>
#include <string>
#ifdef __APPLE__
#include <crt_externs.h>
#elif !defined(_WIN32)
extern char** environ;
#endif

namespace gfxvk { std::string multiview_repair_note(); }  // (gfx/vulkan/surfaces.cpp)
namespace crash_context {
namespace {
constexpr size_t capacity = 32768;
static_assert(std::atomic<char>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);
std::atomic<char> snapshot[capacity];
std::atomic<unsigned> revision{0}, length{0};
Redactor redactor;
void publish(const std::string& text) {
    revision.fetch_add(1, std::memory_order_seq_cst);
    auto size = text.size() < capacity - 1 ? text.size() : capacity - 1;
    for (size_t i = 0; i < size; ++i) snapshot[i].store(text[i], std::memory_order_seq_cst);
    length.store(unsigned(size), std::memory_order_seq_cst);
    revision.fetch_add(1, std::memory_order_seq_cst);
}
void environment(std::ostringstream& out) {
#ifdef __APPLE__
    char** env = *_NSGetEnviron();
#elif defined(_WIN32)
    char** env = _environ;
#else
    char** env = environ;
#endif
    for (char** e = env; e && *e; ++e) {
        std::string_view line(*e);
        if (!line.starts_with("WWHD_")) continue;
        auto equal = line.find('=');
        auto key = line.substr(0, equal);
        // Settings are useful; key/token material must never enter a crash artifact.
        if (key.find("KEY") != key.npos || key.find("TOKEN") != key.npos || key.find("SECRET") != key.npos || key.find("PASSWORD") != key.npos)
            out << key << "=<redacted>\n";
        else {
            for (char c : line) out << (c == '\n' || c == '\r' ? ' ' : c);
            out << '\n';
        }
    }
}
}
void initialize() {
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    const char* user = std::getenv("USER");
    if (!user) user = std::getenv("USERNAME");
    redactor.initialize(home, user);
    std::ostringstream out; out << "\n--- crash context (startup) ---\n"; environment(out);
    publish(out.str());
}
void refresh() {
    if (crash_memory::enabled()) crash_memory::refresh(threads::diagnostic_count(), mem::runtime_top());
    std::ostringstream out;
    out << "\n--- crash context (latest game-thread snapshot) ---\n";
    out << "renderer=" << render::api_name(render::active()) << "\nresolution=" << (render::g_backend ? render::res_scale() : 1.f)
        << "\nframe_mode=" << interp::mode_name() << "\nframe_target=" << interp::fps()
        << "\ncontroller=" << (input::pro_controller() ? "Pro" : "GamePad") << '\n';
    // live shaders/pipelines and memory mappings (render_objects.h): running out of driver mappings
    out << render_objects::line() << '\n';
    // multiview's two-eye targets paired again after a rescale (gfx/vulkan/surfaces.cpp): a crash in the Adreno driver
    // follows its cause by some frames; this says how many frames before this snapshot (1 s old at most) the last was
    out << "multiview_repairs=" << gfxvk::multiview_repair_note() << '\n';
    for (const auto& entry : mods::manager::entries()) out << "builtin." << entry.id << '=' << entry.enabled() << '\n';
    for (const auto& package : mods::packages::list()) if (package.enabled || package.active)
        out << "package=" << package.id << " version=" << package.version << " enabled=" << package.enabled << " active=" << package.active << '\n';
    environment(out); publish(out.str());
}
void redact(int fd, std::string_view text, Output out) { redactor.write(fd, text, out); }
void note(int fd, Output out) {
    // Atomic bytes avoid a data race; no retry/spin if a crash interrupts the publisher.
    // The copy is static, not on the stack: a handler runs on the thread's signal stack, which on Android is the
    // system's small one for every thread but the first (main.cpp installs 64 KB there only), and 32 KB overran it.
    // One caller at a time; a second (two threads crashing at once) says so rather than waiting.
    static char text[capacity];
    static std::atomic_flag busy = ATOMIC_FLAG_INIT;
    if (busy.test_and_set(std::memory_order_acquire)) { out(fd, "\ncrash context: another thread is writing it\n", 45); return; }
    struct Release { ~Release() { busy.clear(std::memory_order_release); } } release;
    auto before = revision.load(std::memory_order_seq_cst);
    if (before & 1) { out(fd, "\ncrash context: update interrupted\n", 34); return; }
    auto size = length.load(std::memory_order_seq_cst);
    for (unsigned i = 0; i < size; ++i) text[i] = snapshot[i].load(std::memory_order_seq_cst);
    if (before == revision.load(std::memory_order_seq_cst)) out(fd, text, size);
    else out(fd, "\ncrash context: update interrupted\n", 34);
}
}
