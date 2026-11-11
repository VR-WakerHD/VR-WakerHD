#include "compile_journal.h"
#include <exception>
#include "shader_compiler.h"
#include "spirv_precision.h"
#include "stutter_trace.h"
#include "worker_metrics.h"
#include "platform/host.h"
#include "atomic_temporary.h"
#include "shader_disk_cache.h"
#include <glslang/build_info.h>
#include <filesystem>
#include <future>
#include <unordered_map>
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <condition_variable>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace gfxvk::vk {
struct ShaderCompileTicket::State {
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool ready = false, consumed = false;
    ShaderCompileResult result;
    std::exception_ptr error;
};
namespace {
std::atomic<bool> fullPrecision{false};
std::atomic<uint64_t> fullPrecisionModules{0}, fullPrecisionStripped{0};
}
void set_full_precision(bool on) { fullPrecision = on; }
bool full_precision_enabled() { return fullPrecision.load(std::memory_order_relaxed); }
void note_relaxed_precision_stripped(size_t removed) {
    const uint64_t modules = ++fullPrecisionModules;
    const uint64_t total = fullPrecisionStripped += removed;
    // the first module, then every 256th: how many decorations the driver no longer sees
    if (modules == 1 || modules % 256 == 0)
        fprintf(stderr, "[vulkan diagnostics] full precision: %llu SPIR-V modules, %llu RelaxedPrecision decorations removed\n",
                (unsigned long long)modules, (unsigned long long)total);
}
namespace {
std::mutex glslangMutex; // Outlives the function-static worker.
static std::vector<uint32_t> compile_stage(const std::string& source, EShLanguage stage, std::string* error) {
    // Serialize even diagnostic inline calls against background work.
    std::lock_guard lock(glslangMutex);
    static std::once_flag init;
    static bool initialized = false;
    std::call_once(init, [] { initialized = glslang::InitializeProcess(); });
    if (error) error->clear();
    auto fail = [&](const std::string& reason) { if (error) *error = reason; return std::vector<uint32_t>{}; };
    if (!initialized) return fail("glslang initialization failed");
    glslang::TShader shader(stage);
    const char* text = source.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, messages))
        return fail(std::string(shader.getInfoLog()) + shader.getInfoDebugLog());
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) return fail(program.getInfoLog());
    std::vector<uint32_t> words;
    glslang::SpvOptions options;
    options.disableOptimizer = true; // no SPIRV-Tools runtime dependency
    glslang::GlslangToSpv(*program.getIntermediate(stage), words, &options);
    if (words.empty()) return fail("glslang emitted empty SPIR-V");
    if (full_precision_enabled()) note_relaxed_precision_stripped(strip_relaxed_precision(words));
    return words;
}

// ---- the utility compiles on disk (shader_compiler.h load_utility_spirv): the key is the source's 128-bit hash and
// length with its stage and the full-precision choice; the file holds glslang's version, so another glslang starts
// empty. One zlib payload: count, then per entry hash (16), length (4), stage and precision (4), word count (4), words
namespace utility {
constexpr char kMagic[8] = {'W', 'W', 'U', 'T', 'S', 'P', '0', '1'};
constexpr size_t kMaxEntries = 16384, kMaxRawBytes = 96u * 1024u * 1024u;
struct Key {
    uint64_t lo = 0, hi = 0;
    uint32_t length = 0, tag = 0;  // tag: stage | full precision << 8
    bool operator==(const Key&) const = default;
};
struct KeyHash { size_t operator()(const Key& k) const { return size_t(k.lo ^ (k.hi * 31) ^ k.tag); } };
std::mutex mutex;  // the map, the path and the dirty mark (any compiling thread)
std::unordered_map<Key, std::shared_ptr<const std::vector<uint32_t>>, KeyHash> entries;
std::string path;
bool dirty = false;
uint64_t revision = 0;
std::future<bool> saving;
uint64_t fingerprint() {
    return (uint64_t(GLSLANG_VERSION_MAJOR) << 48) ^ (uint64_t(GLSLANG_VERSION_MINOR) << 32) ^
           (uint64_t(GLSLANG_VERSION_PATCH) << 16) ^ 0x5554494cull;  // ("UTIL")
}
Key key_of(const std::string& source, ShaderStage stage) {
    const auto id = shader_source_id(source);
    return {id.low, id.high, id.length, uint32_t(stage) | (full_precision_enabled() ? 0x100u : 0u)};
}
void put(std::vector<uint8_t>& out, uint64_t v, size_t n) { for (size_t i = 0; i < n; ++i) out.push_back(uint8_t(v >> (8 * i))); }
bool take(const std::vector<uint8_t>& in, size_t& at, uint64_t& v, size_t n) {
    if (at > in.size() || n > in.size() - at) return false;
    v = 0;
    for (size_t i = 0; i < n; ++i) v |= uint64_t(in[at++]) << (8 * i);
    return true;
}
std::shared_ptr<const std::vector<uint32_t>> find(const Key& k) {
    std::lock_guard lock(mutex);
    auto it = entries.find(k);
    return it == entries.end() ? nullptr : it->second;
}
void insert(const Key& k, const std::vector<uint32_t>& words) {
    if (!diskcache::spirv_valid(words)) return;
    std::lock_guard lock(mutex);
    if (entries.size() >= kMaxEntries || entries.count(k)) return;
    entries.emplace(k, std::make_shared<const std::vector<uint32_t>>(words));
    dirty = !path.empty();
    ++revision;
}
// (immutable snapshot, any thread) the file's bytes
std::vector<uint8_t> encode(const std::vector<std::pair<Key, std::shared_ptr<const std::vector<uint32_t>>>>& all) {
    std::vector<uint8_t> raw;
    put(raw, all.size(), 4);
    for (const auto& [k, words] : all) {
        put(raw, k.lo, 8); put(raw, k.hi, 8); put(raw, k.length, 4); put(raw, k.tag, 4); put(raw, words->size(), 4);
        const auto* b = reinterpret_cast<const uint8_t*>(words->data());
        raw.insert(raw.end(), b, b + words->size() * 4);
        if (raw.size() > kMaxRawBytes) return {};
    }
    uLongf packedSize = compressBound(uLong(raw.size()));
    std::vector<uint8_t> packed(packedSize);
    if (compress2(packed.data(), &packedSize, raw.data(), uLong(raw.size()), 1) != Z_OK) return {};
    packed.resize(packedSize);
    std::vector<uint8_t> out(kMagic, kMagic + 8);
    put(out, fingerprint(), 8); put(out, raw.size(), 8); put(out, diskcache::checksum(raw.data(), raw.size()), 8);
    out.insert(out.end(), packed.begin(), packed.end());
    return out;
}
bool write(std::vector<std::pair<Key, std::shared_ptr<const std::vector<uint32_t>>>> all, std::string to) {
    std::string temporary;
    try {
        const auto bytes = encode(all);
        if (bytes.empty()) return false;
        std::error_code ec;
        const auto parent = std::filesystem::u8path(to).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        FILE* f = open_atomic_temporary(to, temporary);
        if (!f) return false;
        bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        if (fclose(f) != 0) ok = false;
        if (ok && host::replace_file(temporary, to)) return true;
    } catch (...) {}
    if (!temporary.empty()) { std::error_code ec; std::filesystem::remove(std::filesystem::u8path(temporary), ec); }
    return false;
}
}  // namespace utility

ShaderCompileResult execute(const std::string& source, ShaderStage stage, uint64_t frame, uint64_t key) {
    EShLanguage language;
    switch (stage) {
    case ShaderStage::Vertex: language = EShLangVertex; break;
    case ShaderStage::Fragment: language = EShLangFragment; break;
    case ShaderStage::Compute: language = EShLangCompute; break;
    default: throw std::runtime_error("invalid shader compiler stage");
    }
    ShaderCompileResult result;
    // the renderer's own compiles: from disk when a session before made the same (no compile, no lock)
    const bool utilityCompile = frame == ~uint64_t{0};
    const utility::Key utilityKey = utilityCompile ? utility::key_of(source, stage) : utility::Key{};
    if (utilityCompile)
        if (auto words = utility::find(utilityKey)) {
            result.words = *words;
            result.thread = uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id()));
            return result;
        }
    // the compile journal (compile_journal.h): GLSL that crashed glslang at an earlier start is not compiled again
    const uint64_t journalId = compile_journal::glslang_id(source, int(language));
    if (compile_journal::left_out(compile_journal::kGlslang, journalId)) {
        result.error = "left out: it crashed the shader compiler at an earlier start (compile journal)";
        result.thread = uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        return result;
    }
    const auto cpuStart = stutter::trace().enabled() ? stutter::thread_cpu_now() : 0;
    const auto start = stutter::now();
    {
        compile_journal::Scope journal(compile_journal::kGlslang, journalId);
        result.words = compile_stage(source, language, &result.error);
    }
    if (utilityCompile && !result.words.empty()) utility::insert(utilityKey, result.words);
    const auto end = stutter::now();
    result.compileNs = end - start;
    result.thread = uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    stutter::trace().add(frame == ~uint64_t{0} ? "utility_spirv_compile" : "spirv_compile",
                         frame, key, start, end, result.words.empty() ? -1 : 0, stutter::cpu_elapsed(cpuStart));
    return result;
}
struct Job {
    std::string source;
    ShaderStage stage;
    uint64_t frame, key;
    std::shared_ptr<ShaderCompileTicket::State> done;
};
struct Compiler {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::shared_ptr<Job>> queue;
    std::thread thread;
    bool stopped = false;
    std::atomic<unsigned> active{0};
    void finish() {
        { std::lock_guard lock(mutex); stopped = true; }
        wake.notify_all();
        if (thread.joinable()) thread.join();
    }
    ~Compiler() { finish(); }
};
Compiler& compiler() {
    (void)stutter::trace(); // Trace outlives the worker at process exit.
    static Compiler value;
    return value;
}
void run(Compiler& c) {
    host::set_thread_name("Vulkan SPIR-V");
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(c.mutex);
            c.wake.wait(lock, [&] { return c.stopped || !c.queue.empty(); });
            if (c.queue.empty()) return;
            job = std::move(c.queue.front()); c.queue.pop_front();
        }
        ShaderCompileResult result;
        std::exception_ptr error;
        try { result = execute(job->source, job->stage, job->frame, job->key); }
        catch (...) { error = std::current_exception(); }
        {
            std::lock_guard lock(job->done->mutex);
            job->done->result = std::move(result);
            job->done->error = error;
            job->done->ready = true;
        }
        c.active.fetch_sub(1, std::memory_order_release);
        job->done->wake.notify_all();
        c.wake.notify_all();
    }
}
}
ShaderCompileTicket::ShaderCompileTicket(std::shared_ptr<State> state) : state_(std::move(state)) {}
void ShaderCompileTicket::prioritize() const {
    if (!state_) return;
    auto& c = compiler();
    std::lock_guard lock(c.mutex);
    for (auto it = c.queue.begin(); it != c.queue.end(); ++it)
        if ((*it)->done == state_) {
            if (it != c.queue.begin()) { auto job = std::move(*it); c.queue.erase(it); c.queue.push_front(std::move(job)); }
            return;
        }
}
bool ShaderCompileTicket::ready() const {
    if (!state_) throw std::logic_error("empty shader compile ticket");
    std::lock_guard lock(state_->mutex);
    return state_->ready;
}
std::optional<ShaderCompileResult> ShaderCompileTicket::wait_for(uint64_t budgetNs) {
    if (!state_) throw std::logic_error("empty shader compile ticket");
    std::unique_lock lock(state_->mutex);
    if (state_->consumed) throw std::logic_error("shader result already consumed");
    if (!state_->ready && budgetNs)
        state_->wake.wait_for(lock, std::chrono::nanoseconds(std::min<uint64_t>(budgetNs,
            uint64_t(std::numeric_limits<int64_t>::max()))), [&] { return state_->ready; });
    if (!state_->ready) return std::nullopt;
    if (state_->consumed) throw std::logic_error("shader result already consumed");
    state_->consumed = true;
    if (state_->error) std::rethrow_exception(state_->error);
    return std::move(state_->result);
}
ShaderCompileResult ShaderCompileTicket::consume() {
    if (!state_) throw std::logic_error("empty shader compile ticket");
    std::unique_lock lock(state_->mutex);
    state_->wake.wait(lock, [&] { return state_->ready; });
    if (state_->consumed) throw std::logic_error("shader result already consumed");
    state_->consumed = true;
    if (state_->error) std::rethrow_exception(state_->error);
    return std::move(state_->result);
}
std::optional<ShaderCompileTicket> request_shader_compile(const std::string& source,
    ShaderStage stage, uint64_t frame, uint64_t key, bool background) {
    if (source.size() > 4 * 1024 * 1024)
        throw std::runtime_error("shader compiler source exceeds 4 MiB");
    if (!background) {
        auto state = std::make_shared<ShaderCompileTicket::State>();
        try { state->result = execute(source, stage, frame, key); }
        catch (...) { state->error = std::current_exception(); }
        state->ready = true;
        return ShaderCompileTicket(std::move(state));
    }
    auto& c = compiler();
    std::lock_guard lock(c.mutex);
    if (c.stopped && c.thread.joinable())
        throw std::runtime_error("shader compiler shutdown in progress");
    if (c.queue.size() >= 8) return std::nullopt;
    auto state = std::make_shared<ShaderCompileTicket::State>();
    auto job = std::make_shared<Job>(Job{source, stage, frame, key, state});
    if (!c.thread.joinable()) { c.stopped = false; c.thread = std::thread(run, std::ref(c)); }
    c.queue.push_back(std::move(job));
    c.active.fetch_add(1, std::memory_order_release);
    if (stutter::trace().enabled()) try {
        const auto now = stutter::now();
        stutter::trace().add("shader_request", frame, key, now, now, int(c.queue.size()));
    } catch (...) {} // Trace allocation cannot strand an admitted job.
    c.wake.notify_one();
    return ShaderCompileTicket(std::move(state));
}
ShaderCompileResult compile_shader(const std::string& source, ShaderStage stage,
    uint64_t frame, uint64_t key, const std::function<void()>& whilePending, bool background) {
    const auto requested = stutter::trace().enabled() ? stutter::now() : 0;
    std::exception_ptr presentationError;
    auto service = [&] {
        if (!presentationError && whilePending) try { whilePending(); }
        catch (...) { presentationError = std::current_exception(); }
    };
    std::optional<ShaderCompileTicket> ticket;
    std::optional<std::string> admissionSource;
    while (!(ticket = request_shader_compile(admissionSource ? *admissionSource : source,
                                            stage, frame, key, background))) {
        // A presentation callback may mutate the original caller string. Keep
        // the requested bytes stable even while waiting for queue admission.
        if (!admissionSource) admissionSource.emplace(source);
        service();
        auto& c = compiler();
        std::unique_lock lock(c.mutex);
        c.wake.wait_for(lock, std::chrono::milliseconds(1));
    }
    std::optional<ShaderCompileResult> result;
    while (!(result = ticket->wait_for(1000000))) service();
    if (requested && background) try {
        stutter::trace().add("shader_pending", frame, key, requested, stutter::now(), result->words.empty() ? -1 : 0);
    } catch (...) {}
    if (presentationError) std::rethrow_exception(presentationError);
    return std::move(*result);
}
bool async_shaders_enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("WWHD_VK_ASYNC_SHADERS");
        return !value || std::strcmp(value, "0");
    }();
    return enabled;
}
bool shader_compiler_active() { return compiler().active.load(std::memory_order_acquire) != 0; }
void finish_shader_compiler() { compiler().finish(); }
size_t load_utility_spirv(const std::string& file) {
    std::error_code ec;
    {
        std::lock_guard lock(utility::mutex);
        utility::path = file;
    }
    const auto length = std::filesystem::file_size(std::filesystem::u8path(file), ec);
    if (ec || length < 32 || length > utility::kMaxRawBytes) return 0;
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    FILE* f = open_utf8_file(file, "rb");
    if (!f) return 0;
    const bool read = fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
    if (!read || std::memcmp(bytes.data(), utility::kMagic, 8) != 0) return 0;
    size_t at = 8;
    uint64_t print = 0, rawSize = 0, sum = 0, count = 0;
    if (!utility::take(bytes, at, print, 8) || print != utility::fingerprint() || !utility::take(bytes, at, rawSize, 8) ||
        rawSize > utility::kMaxRawBytes || !utility::take(bytes, at, sum, 8))
        return 0;
    std::vector<uint8_t> raw(static_cast<size_t>(rawSize));
    uLongf size = uLongf(rawSize);
    if (uncompress(raw.data(), &size, bytes.data() + at, uLong(bytes.size() - at)) != Z_OK || size != rawSize ||
        diskcache::checksum(raw.data(), raw.size()) != sum)
        return 0;
    at = 0;
    if (!utility::take(raw, at, count, 4) || count > utility::kMaxEntries) return 0;
    decltype(utility::entries) loaded;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t lo, hi, len, tag, n;
        if (!utility::take(raw, at, lo, 8) || !utility::take(raw, at, hi, 8) || !utility::take(raw, at, len, 4) ||
            !utility::take(raw, at, tag, 4) || !utility::take(raw, at, n, 4) || n > diskcache::kMaxSpirvWords ||
            n * 4 > raw.size() - at)
            return 0;  // (a damaged file: nothing taken from it)
        std::vector<uint32_t> words(static_cast<size_t>(n));
        std::memcpy(words.data(), raw.data() + at, size_t(n) * 4);
        at += size_t(n) * 4;
        if (!diskcache::spirv_valid(words)) return 0;
        loaded.emplace(utility::Key{lo, hi, uint32_t(len), uint32_t(tag)},
                       std::make_shared<const std::vector<uint32_t>>(std::move(words)));
    }
    std::lock_guard lock(utility::mutex);
    const size_t n = loaded.size();
    for (auto& e : loaded) utility::entries.insert(std::move(e));
    return n;
}
bool save_utility_spirv(bool wait) {
    std::unique_lock lock(utility::mutex);
    if (utility::saving.valid()) {
        if (!wait && utility::saving.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        auto job = std::move(utility::saving);
        lock.unlock();
        const bool saved = job.get();
        lock.lock();
        if (!saved) utility::dirty = true;
    }
    if (!utility::dirty || utility::path.empty()) return false;
    // (in play at most every 10 s: a burst of new twins does not rewrite the file each time)
    static auto lastStart = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (!wait && now - lastStart < std::chrono::seconds(10)) return false;
    lastStart = now;
    std::vector<std::pair<utility::Key, std::shared_ptr<const std::vector<uint32_t>>>> all(utility::entries.begin(),
                                                                                           utility::entries.end());
    utility::dirty = false;
    const std::string to = utility::path;
    if (wait) {
        lock.unlock();
        const bool saved = utility::write(std::move(all), to);
        if (!saved) { std::lock_guard again(utility::mutex); utility::dirty = true; }
        return saved;
    }
    utility::saving = std::async(std::launch::async, utility::write, std::move(all), to);
    return true;
}
}
