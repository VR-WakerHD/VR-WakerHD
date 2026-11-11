#include "crash_memory.h"
// Guest memory, RPX loading, function dispatch, logging and HLE registry.
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/ldsyms.h>
#endif
#include "platform/host.h"
#include "write_watch.h"
#include <zlib.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "recomp_table.h"
#include "runtime.h"
#include "guest_module_entry.h"
#include "mods/guest_mods.h"
#include "native_text_startup_seal.h"

bool g_trace_hle = false;
namespace config {
std::string game_dir = "game";
std::string save_dir = "save";
}  // namespace config

static std::mutex g_log_mutex;

// the last log lines, kept for crash logs (crash handlers read them without the lock)
static constexpr int kLogRing = 200, kLogLine = 240;
static char g_log_ring[kLogRing][kLogLine];
static std::atomic<uint32_t> g_log_next{0};
// the log file (main.cpp, captures/wwhd.log): every line in full
static void (*g_log_sink)(const char*, size_t) = nullptr;
void log_set_sink(void (*sink)(const char*, size_t)) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    g_log_sink = sink;
}

// Log lines are written by a thread of their own (docs/VR_PERFORMANCE.md, "Log lines off the calling thread"). The
// thread that logs formats the line once, keeps it in the crash ring and queues it; the writer thread puts the queued
// lines, in order, into the log file (redacted there, main.cpp) and onto the console. A line had cost every thread
// that logged, the render thread among them, its file write and an unbuffered console write under one lock: a slow
// disk or terminal (or a pipe the reader is slow on) held up the frame. Bounded: past 8 MiB queued the thread that
// logs waits for room (a flood, never in normal play), so nothing is lost or reordered. log_flush() writes everything
// queued and waits for it: at the end of the program (before std::_Exit, which skips atexit) and in fatal(). A crash
// handler does not wait for the thread: the crash log has the last 200 lines from the ring
namespace {
struct QueuedLine {
    std::string text;
    void (*sink)(const char*, size_t);  // the sink when it was logged (lines from before the log file stay out of it)
};
struct LogWriter {
    std::mutex mutex;
    std::condition_variable wake, room, drained;
    std::vector<QueuedLine> queue, writing;
    size_t bytes = 0;
    uint64_t queued = 0, written = 0;
    bool started = false;
};
LogWriter& writer() {
    static LogWriter* w = new LogWriter;  // (never destroyed: lines may be logged until the process ends)
    return *w;
}
constexpr size_t kQueueMax = 8u << 20;
void write_line(const QueuedLine& l) {
    if (l.sink) l.sink(l.text.data(), l.text.size());
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "wwhd", l.text.c_str());  // adb logcat -s wwhd
#else
    fwrite(l.text.data(), 1, l.text.size(), stderr);
    fputc('\n', stderr);
#endif
}
void writer_main() {
    auto& w = writer();
    std::unique_lock<std::mutex> lk(w.mutex);
    for (;;) {
        w.wake.wait(lk, [&] { return !w.queue.empty(); });
        w.writing.swap(w.queue);
        w.bytes = 0;
        lk.unlock();
        w.room.notify_all();
        for (const auto& l : w.writing) write_line(l);
        fflush(stderr);
        const size_t n = w.writing.size();
        w.writing.clear();
        lk.lock();
        w.written += n;
        w.drained.notify_all();
    }
}
}  // namespace

void log_flush() {
    auto& w = writer();
    std::unique_lock<std::mutex> lk(w.mutex);
    if (!w.started) return;
    const uint64_t target = w.queued;
    w.drained.wait_for(lk, std::chrono::seconds(2), [&] { return w.written >= target; });
}

void log_msg(const char* fmt, ...) {
    char full[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(full, sizeof full, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    const size_t len = std::min(size_t(n), sizeof full - 1);
    void (*sink)(const char*, size_t);
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        char* line = g_log_ring[g_log_next.load() % kLogRing];
        const size_t r = std::min(len, size_t(kLogLine - 1));
        std::memcpy(line, full, r);
        line[r] = 0;
        g_log_next++;
        sink = g_log_sink;
    }
    auto& w = writer();
    {
        std::unique_lock<std::mutex> lk(w.mutex);
        if (!w.started) {
            w.started = true;
            std::thread(writer_main).detach();
            std::atexit(log_flush);  // (exit() and a return from main: the queued lines first)
        }
        w.room.wait(lk, [&] { return w.bytes < kQueueMax; });
        w.queue.push_back({std::string(full, len), sink});
        w.bytes += len + sizeof(QueuedLine);
        ++w.queued;
    }
    w.wake.notify_one();
}

void log_ring_write(int fd, void (*out)(int, const char*, size_t)) {
    uint32_t n = g_log_next.load();
    uint32_t first = n > (uint32_t)kLogRing ? n - kLogRing : 0;
    for (uint32_t i = first; i < n; i++) {
        const char* l = g_log_ring[i % kLogRing];
        out(fd, l, strnlen(l, kLogLine));
        out(fd, "\n", 1);
    }
}

void fatal(const char* fmt, ...) {
    char message[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    exception_report::record(message);
    LOG("FATAL: %s", message); // file and crash ring, not just stderr
    log_flush();  // (the writer thread's queue: abort ends the process before it would get to it)
    abort();
}

// ---------------------------------------------------------------- memory
namespace mem {
static std::atomic<uint32_t> g_runtime_top{kRuntimeStart};

void init() {
#ifdef __APPLE__
    mach_vm_address_t addr = (mach_vm_address_t)PPC_MEM_BASE;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &addr, 0x100000000ull, VM_FLAGS_FIXED);
    if (kr != KERN_SUCCESS) fatal("cannot reserve guest address space at %p (kr=%d)", PPC_MEM_BASE, kr);
    // null page guard: catches guest null-pointer accesses
    mprotect(PPC_MEM_BASE, 0x10000, PROT_NONE);
#elif defined(_WIN32)
    void* p = VirtualAlloc(PPC_MEM_BASE, 0x100000000ull, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if(p != PPC_MEM_BASE) fatal("cannot reserve guest address space (error=%lu)",GetLastError());
    DWORD old; if(!VirtualProtect(PPC_MEM_BASE,0x10000,PAGE_NOACCESS,&old)) fatal("cannot protect guest null page");
#else
    // Never replace existing mappings: requesting a hint and checking the result is safe on
    // systems whose headers lack MAP_FIXED_NOREPLACE.
#ifdef __ANDROID__
    // phones have little RAM and strict commit accounting: pages are committed on first touch
    void* p = mmap(PPC_MEM_BASE,0x100000000ull,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
#else
    void* p = mmap(PPC_MEM_BASE,0x100000000ull,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
#endif
    if(p != PPC_MEM_BASE) { if(p!=MAP_FAILED)munmap(p,0x100000000ull); fatal("cannot reserve guest address space at %p",PPC_MEM_BASE); }
    if(mprotect(PPC_MEM_BASE,0x10000,PROT_NONE))fatal("cannot protect guest null page");
#endif
    // texture change detection (write_watch.h): after the crash handler, which it chains to
    if (!wwatch::init(PPC_MEM_BASE, 0x100000000ull)) LOG("[mem] write tracking unavailable: textures use sampled change checks");
}

static std::mutex g_alloc_log_m;
static std::vector<AllocRec> g_alloc_log;

static uint32_t bump(std::atomic<uint32_t>& top, uint32_t size, uint32_t align, uint32_t end) {
    uint32_t cur = top.load();
    uint32_t start;
    do {
        start = (cur + align - 1) & ~(align - 1);
        if (start + size > end) fatal("runtime guest region exhausted");
    } while (!top.compare_exchange_weak(cur, start + size));
    memset(ptr(start), 0, size);
    return start;
}

__attribute__((noinline)) uint32_t runtime_alloc(uint32_t size, uint32_t align) {
    uint32_t a = bump(g_runtime_top, size, align, kHostStart);
    // who allocated (relative to the executable, stable across runs of one build): a loaded save
    // state requires the same guest-visible allocations at the same addresses
    uint64_t tag = (uint64_t)((uintptr_t)__builtin_return_address(0) - host::executable_base());
    std::lock_guard<std::mutex> lk(g_alloc_log_m);
    g_alloc_log.push_back({a, size, tag});
    return a;
}

static std::atomic<uint32_t> g_host_top{kHostStart};
uint32_t host_alloc(uint32_t size, uint32_t align) { return bump(g_host_top, size, align, kFixedStart); }

uint32_t runtime_top() { return g_runtime_top.load(); }
void raise_runtime_top(uint32_t top) {
    uint32_t cur = g_runtime_top.load();
    while (cur < top && !g_runtime_top.compare_exchange_weak(cur, top)) {}
}
std::vector<AllocRec> runtime_alloc_log() {
    std::vector<AllocRec> v;
    {
        std::lock_guard<std::mutex> lk(g_alloc_log_m);
        v = g_alloc_log;
    }
    std::sort(v.begin(), v.end(), [](const AllocRec& a, const AllocRec& b) { return a.addr < b.addr; });
    return v;
}

std::string read_cstr(uint32_t ea) {
    if (!ea) return {};
    return std::string((const char*)ptr(ea));
}

void write_cstr(uint32_t ea, const std::string& s, uint32_t max) {
    uint32_t n = (uint32_t)std::min<size_t>(s.size(), max - 1);
    memcpy(ptr(ea), s.data(), n);
    ptr(ea)[n] = 0;
}
}  // namespace mem

// ---------------------------------------------------------------- loader
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

bool load_rpx(const std::string& path, LoadedModule& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    uint32_t shoff = be32(&d[0x20]);
    uint16_t shentsize = be16(&d[0x2E]), shnum = be16(&d[0x30]);
    out.entry = be32(&d[0x18]);
    out.data_end = 0;
    std::vector<crash_memory::Section> diagnostic_sections;
    size_t diagnostic_bytes = 0;
    for (int i = 0; i < shnum; i++) {
        const uint8_t* sh = &d[shoff + i * shentsize];
        uint32_t type = be32(sh + 4), flags = be32(sh + 8), addr = be32(sh + 12), off = be32(sh + 16), size = be32(sh + 20);
        std::vector<uint8_t> data;
        if (type != 8 && size) {
            if (flags & 0x08000000) {
                uLongf dlen = be32(&d[off]);
                data.resize(dlen);
                if (uncompress(data.data(), &dlen, &d[off + 4], size - 4) != Z_OK) fatal("zlib error in section %d", i);
            } else {
                data.assign(d.begin() + off, d.begin() + off + size);
            }
        }
        if (type == 0x80000004 && data.size() >= 0x30) {  // RPL file info
            out.sda_base = be32(&data[0x24]);
            out.sda2_base = be32(&data[0x28]);
            out.stack_size = be32(&data[0x2C]);
        }
        if (crash_memory::enabled() && (flags & 2)) {
            crash_memory::Section section{addr, flags, {}, type == 8 ? uint64_t(size) : uint64_t(data.size())};
            if ((flags & 7) == 6 && addr >= 0x02000000 && uint64_t(addr) + data.size() <= 0x10000000 && data.size() <= 16 * 1024 * 1024 - diagnostic_bytes) {
                section.bytes = data; diagnostic_bytes += data.size();
            }
            diagnostic_sections.push_back(std::move(section));
        }
        // allocated sections in the text/data regions
        if ((flags & 2) && addr >= 0x02000000 && addr < 0xC0000000) {
            if (type == 8) {
                memset(mem::ptr(addr), 0, size);
                out.data_end = std::max(out.data_end, addr + size);
            } else {
                memcpy(mem::ptr(addr), data.data(), data.size());
                if (addr >= mem::kMem2Start) out.data_end = std::max(out.data_end, addr + (uint32_t)data.size());
            }
        }
    }
    crash_memory::install(std::move(diagnostic_sections), PPC_MEM_BASE);
    return true;
}

// ---------------------------------------------------------------- dispatch
namespace dispatch {
static detail::StartupSeal native_text_seal;
static PpcFunc* sealed_text_table=nullptr;
static constexpr uint32_t kTextBase = 0x02000000;
static constexpr uint32_t kTextSize = 0x01000000;  // 16 MiB covers cking.rpx .text
static PpcFunc* g_text_table;                        // indexed by (addr - kTextBase) / 4
// lock-free direct tables for import slots and host functions (called through pointers a lot)
static constexpr uint32_t kSlotBase = 0xC0000000, kSlotSize = 0x40000;     // import stubs, 4-byte steps
static constexpr uint32_t kHostSize = 0x80000;                              // host functions, 8-byte steps
static std::atomic<PpcFunc> g_slot_table[kSlotSize / 4];
static std::atomic<PpcFunc> g_host_table[kHostSize / 8];
static std::unordered_map<uint32_t, PpcFunc> g_other;
static std::shared_mutex g_other_mutex;
static std::atomic<uint32_t> g_next_host{mem::kHleFuncBase};
static std::unordered_map<uint32_t, std::string> g_host_names;

void init() {
    auto mutation=native_text_seal.lock();
    if(!native_text_seal.begin_initialize())return;
    g_text_table = (PpcFunc*)calloc(kTextSize / 4, sizeof(PpcFunc));
    for (unsigned i = 0; i < g_recomp_func_count; i++) set(g_recomp_funcs[i].addr, g_recomp_funcs[i].fn);
    // imported functions are reachable through their import slot addresses
    for (unsigned i = 0; i < g_recomp_import_count; i++)
        if (g_recomp_imports[i].is_func) set(g_recomp_imports[i].slot, g_recomp_imports[i].fn);
}

void set(uint32_t addr, PpcFunc fn) {
    if (addr - kTextBase < kTextSize) {
        auto mutation=native_text_seal.lock();
        if(!native_text_seal.writable())return;
        g_text_table[(addr - kTextBase) >> 2] = fn;
        return;
    }
    if (addr - kSlotBase < kSlotSize && !(addr & 3)) { g_slot_table[(addr - kSlotBase) >> 2] = fn; return; }
    if (addr - mem::kHleFuncBase < kHostSize && !(addr & 7)) { g_host_table[(addr - mem::kHleFuncBase) >> 3] = fn; return; }
    std::unique_lock lk(g_other_mutex);
    g_other[addr] = fn;
}

bool seal_native_text() noexcept {try{
    auto mutation=native_text_seal.lock();
    if(!g_text_table||!native_text_seal.seal())return false;
    if(!sealed_text_table)sealed_text_table=g_text_table;
    return sealed_text_table==g_text_table;
}catch(...){return false;}}
bool native_text_sealed() noexcept{try{auto mutation=native_text_seal.lock();return native_text_seal.current()&&sealed_text_table&&sealed_text_table==g_text_table;}catch(...){return false;}}

PpcFunc lookup(uint32_t addr) {
    if (addr - kTextBase < kTextSize) return g_text_table[(addr - kTextBase) >> 2];
    if (addr - kSlotBase < kSlotSize && !(addr & 3)) return g_slot_table[(addr - kSlotBase) >> 2].load(std::memory_order_relaxed);
    if (addr - mem::kHleFuncBase < kHostSize && !(addr & 7))
        return g_host_table[(addr - mem::kHleFuncBase) >> 3].load(std::memory_order_relaxed);
    std::shared_lock lk(g_other_mutex);
    auto it = g_other.find(addr);
    return it == g_other.end() ? nullptr : it->second;
}

std::vector<std::pair<uint32_t, std::string>> host_functions() {
    std::shared_lock lk(g_other_mutex);
    std::vector<std::pair<uint32_t, std::string>> v(g_host_names.begin(), g_host_names.end());
    std::sort(v.begin(), v.end());
    return v;
}

uint32_t register_host(PpcFunc fn, const char* name) {
    uint32_t a = g_next_host.fetch_add(8);
    set(a, fn);
    std::unique_lock lk(g_other_mutex);
    g_host_names[a] = name;
    return a;
}
}  // namespace dispatch

extern "C" void ppc_host_call(Cpu* c, PpcFunc fn) {
    exception_report::boundary<GuestExit>("native game callback", [&] { fn(c); });
}

extern "C" void ppc_dispatch(Cpu* c) {
    if (mainprof::on(c)) mainprof::bump(mainprof::g_dispatches);
    PpcFunc f = dispatch::lookup(c->pc);
    if (!f) fatal("indirect branch to unknown address %08X (lr=%08X ctr=%08X)", c->pc, c->lr, c->ctr);
    if (c->pc - dispatch::kSlotBase < dispatch::kSlotSize ||
        c->pc - mem::kHleFuncBase < dispatch::kHostSize) {
        ppc_host_call(c, f);
        return;
    }
    if (c->pc - guestmods::kRegionStart < guestmods::kRegionSize) {
        guestmods::invoke_module_entry(c, f);
        return;
    }
    MUSTTAIL return f(c);
}

// checking builds of the recompiler (tools/recomp/recomp.py): WWHD_RECOMP_CR_CHECK=1 poisons the CR
// bits the liveness pass drops, and reading one stops here
extern "C" void ppc_cr_poisoned(Cpu* c, int bit, uint32_t addr) {
    fatal("CR liveness: bit %d read at %08X without a live store (lr=%08X)", bit, addr, c->lr);
}

uint32_t guest_call(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t save_lr = c->lr, save_ctr = c->ctr, save_sp = c->r[1];
    // open a minimal frame so the callee's LR save slot doesn't clobber our caller's frame
    c->r[1] -= 0x40;
    st32(c->r[1], save_sp);
    int i = 3;
    for (uint32_t a : args) c->r[i++] = a;
    c->lr = 0;
    c->pc = fn;
    ppc_dispatch(c);
    c->r[1] = save_sp;
    c->lr = save_lr;
    c->ctr = save_ctr;
    return c->r[3];
}

extern "C" void ppc_unimplemented(Cpu* c, uint32_t addr, uint32_t insn) {
    fatal("unimplemented instruction %08X at %08X (lr=%08X)", insn, addr, c->lr);
}

extern "C" void ppc_trap(Cpu* c, uint32_t addr) {
    fatal("guest trap at %08X (lr=%08X r3=%08X)", addr, c->lr, c->r[3]);
}

// ---------------------------------------------------------------- HLE registry
static std::vector<HleReg*>& hle_registry() {
    static std::vector<HleReg*> r;
    return r;
}
HleReg::HleReg(const char* l, const char* n, PpcFunc f) : lib(l), name(n), fn(f) { hle_registry().push_back(this); }

PpcFunc hle_find(const char* lib, const char* name) {
    std::string l = lib;
    if (l.size() > 4 && l.substr(l.size() - 4) == ".rpl") l.resize(l.size() - 4);
    for (HleReg* r : hle_registry())
        if (l == r->lib && strcmp(name, r->name) == 0) return r->fn;
    return nullptr;
}

PpcFunc hle_find_any(const char* name) {
    for (HleReg* r : hle_registry())
        if (strcmp(name, r->name) == 0) return r->fn;
    return nullptr;
}

extern "C" void hle_unimplemented(Cpu* c, const char* lib, const char* name) {
    static std::mutex m;
    static std::unordered_map<std::string, int> seen;
    {
        std::lock_guard<std::mutex> lk(m);
        int& n = seen[std::string(lib) + "." + name];
        if (n++ < 3)
            log_msg("[hle] unimplemented %s.%s(%08X, %08X, %08X, %08X) lr=%08X", lib, name, c->r[3], c->r[4], c->r[5],
                    c->r[6], c->lr);
    }
    c->r[3] = 0;
}
