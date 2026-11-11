// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Runs wwhd-extract (or another program) for the launcher's "Install game" tab (game_install.h): its arguments as a
// list, never through a shell, so spaces, quotes and non-ASCII letters in paths are safe (SDL turns them into the
// platform's own argument form). A worker thread reads its output line by line (its error output goes into the same
// stream: wwhd-extract's errors are its "error: " lines), keeps the last progress and all other lines, and the window
// reads that each frame. The window never waits for the process. Cancel kills it; the result then says cancelled.
#pragma once
#include <SDL3/SDL.h>
#if defined(__ANDROID__) || defined(WWHD_RUNNER_POSIX_SPAWN)
// Android: SDL has no processes there (its process API is a stub); the extractor is run with posix_spawn, from the
// app's native-library folder (the one place an app may execute files: docs/VR_ANDROID.md). WWHD_RUNNER_POSIX_SPAWN
// builds the same path elsewhere, for its test on Linux (launcher_install_posix)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "game_install.h"

namespace install {

class Runner {
public:
    struct Snapshot {
        bool running = false, finished = false, cancelled = false, startFailed = false;
        int exitCode = 0;
        Progress progress;
        std::string output;  // every line that is not progress
        std::string error;   // why it could not start
    };
    Runner() = default;
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;
    ~Runner() {
        cancel();
        if (thread_.joinable()) thread_.join();
    }
    // false (with the reason in snapshot().error) when it could not start, or one is running already
    bool start(const std::vector<std::string>& args) {
        if (running()) return false;
        if (thread_.joinable()) thread_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = Snapshot{};
            state_.running = true;
        }
        cancel_ = false;
        std::vector<const char*> argv;
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
#if defined(__ANDROID__) || defined(WWHD_RUNNER_POSIX_SPAWN)
        int fds[2];
        pid_t pid = -1;
        int err = pipe2(fds, O_CLOEXEC);
        if (err == 0) {
            posix_spawn_file_actions_t fa;
            posix_spawn_file_actions_init(&fa);
            posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
            posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
            posix_spawn_file_actions_adddup2(&fa, fds[1], 2);  // (its errors in the same stream, as below)
            // its libraries (libc++_shared.so) are in its own folder, the app's native-library folder, where Android's
            // linker does not look for a program: without this it does not start ("CANNOT LINK EXECUTABLE", exit 1).
            // The app's setup runs it the same way (tools/android/setup_adapter.py)
            std::vector<std::string> envStore;
            const std::string self = args[0];
            const size_t slash = self.find_last_of('/');
            std::string libPath = "LD_LIBRARY_PATH=" + (slash == std::string::npos ? std::string(".") : self.substr(0, slash));
            for (char** e = environ; e && *e; ++e) {
                if (std::strncmp(*e, "LD_LIBRARY_PATH=", 16) == 0) libPath += std::string(":") + (*e + 16);
                else envStore.push_back(*e);
            }
            envStore.push_back(libPath);
            std::vector<char*> envp;
            for (auto& e : envStore) envp.push_back(e.data());
            envp.push_back(nullptr);
            err = posix_spawn(&pid, argv[0], &fa, nullptr, const_cast<char* const*>(argv.data()), envp.data());
            posix_spawn_file_actions_destroy(&fa);
            close(fds[1]);
            if (err != 0) close(fds[0]);
        } else {
            err = errno;
        }
        if (err != 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.running = false;
            state_.finished = state_.startFailed = true;
            state_.error = std::string("cannot start ") + argv[0] + ": " + std::strerror(err);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(processMutex_);
            pid_ = pid;
        }
        thread_ = std::thread([this, pid, fd = fds[0]] { work_android(pid, fd); });
        return true;
#endif
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)argv.data());
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
        SDL_Process* p = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        if (!p) {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.running = false;
            state_.finished = state_.startFailed = true;
            state_.error = SDL_GetError();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(processMutex_);
            process_ = p;
        }
        thread_ = std::thread([this, p] { work(p); });
        return true;
    }
    void cancel() {
        cancel_ = true;
        std::lock_guard<std::mutex> lock(processMutex_);
        if (process_) SDL_KillProcess(process_, true);
#if defined(__ANDROID__) || defined(WWHD_RUNNER_POSIX_SPAWN)
        if (pid_ > 0) kill(pid_, SIGKILL);
#endif
    }
    bool running() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_.running;
    }
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }
    // waits for the end (tests, the launcher's --check mode)
    Snapshot wait() {
        if (thread_.joinable()) thread_.join();
        return snapshot();
    }

private:
#if defined(__ANDROID__) || defined(WWHD_RUNNER_POSIX_SPAWN)
    pid_t pid_ = -1;
    void work_android(pid_t pid, int fd) {
        std::string pending;
        char buf[4096];
        auto take = [&](const std::string& line) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!parse_progress(line, state_.progress)) state_.output += line + "\n";
        };
        for (;;) {
            const ssize_t n = read(fd, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;  // the end (or the process was killed)
            pending.append(buf, size_t(n));
            for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                std::string line = pending.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                take(line);
                pending.erase(0, nl + 1);
            }
        }
        close(fd);
        if (!pending.empty()) take(pending);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        {
            std::lock_guard<std::mutex> lock(processMutex_);
            pid_ = -1;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        state_.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        state_.cancelled = cancel_.load();
        state_.running = false;
        state_.finished = true;
    }
#endif
    void work(SDL_Process* p) {
        SDL_IOStream* out =
            static_cast<SDL_IOStream*>(SDL_GetPointerProperty(SDL_GetProcessProperties(p), SDL_PROP_PROCESS_STDOUT_POINTER, nullptr));
        std::string pending;
        char buf[4096];
        auto take = [&](const std::string& line) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!parse_progress(line, state_.progress)) state_.output += line + "\n";
        };
        while (out) {
            const size_t n = SDL_ReadIO(out, buf, sizeof buf);
            if (n == 0) {
                if (SDL_GetIOStatus(out) == SDL_IO_STATUS_NOT_READY) {
                    SDL_Delay(5);
                    continue;
                }
                break;  // the end (or the process was killed)
            }
            pending.append(buf, n);
            for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                std::string line = pending.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                take(line);
                pending.erase(0, nl + 1);
            }
        }
        if (!pending.empty()) take(pending);
        int code = 0;
        SDL_WaitProcess(p, true, &code);
        {
            std::lock_guard<std::mutex> lock(processMutex_);
            process_ = nullptr;
        }
        SDL_DestroyProcess(p);
        std::lock_guard<std::mutex> lock(mutex_);
        state_.exitCode = code;
        state_.cancelled = cancel_.load();
        state_.running = false;
        state_.finished = true;
    }

    mutable std::mutex mutex_;
    Snapshot state_;
    std::mutex processMutex_;
    SDL_Process* process_ = nullptr;
    std::atomic<bool> cancel_{false};
    std::thread thread_;
};

}  // namespace install
