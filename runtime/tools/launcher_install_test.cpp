// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The VR launcher's Install game tab (tools/vr_launcher/game_install.h, extract_runner.h): its decisions on their own,
// then wwhd-extract run through the same runner on the synthetic disc images and Cemu archive its own tests build
// (made-up keys and contents, no game data: tools/wudextract/extract_test.cpp, wua_test.cpp), in a folder whose name
// has spaces and non-ASCII letters.
//
// usage: launcher_install_test WWHD_EXTRACT EXTRACT_TEST WUA_TEST WORKDIR   (ctest "launcher_install")
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "extract_runner.h"
#include "game_install.h"

namespace fs = std::filesystem;
using namespace install;

static int failures = 0;
static void expect(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
static void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o << s;
}
static std::string slurp(const fs::path& p) { return read_file(p); }
static bool rename_replace(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::rename(u8path(from), u8path(to), ec);
    return !ec;
}
static const char* kMeta =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<menu type=\"complex\" access=\"777\">\n"
    "  <title_id type=\"hexBinary\" length=\"8\">0005000010143500</title_id>\n"
    "  <longname_en type=\"string\" length=\"512\">The Legend of Zelda:\nThe Wind Waker HD</longname_en>\n"
    "  <shortname_en type=\"string\" length=\"256\">Wind Waker HD</shortname_en>\n</menu>\n";
// a complete-looking game folder (made-up bytes)
static void fake_game(const fs::path& d, const std::string& meta = kMeta) {
    write(d / "code" / "cking.rpx", "not a real program");
    write(d / "content" / "Common" / "x.bin", "x");
    write(d / "meta" / "meta.xml", meta);
}

static void test_parsing() {
    // a disc image's info
    SourceInfo d = parse_info("title_id 0005000010143500\nfiles 3\nbytes 123456\n");
    expect(!d.archive && d.titleId == "0005000010143500" && d.files == 3 && d.bytes == 123456, "info: disc image");
    // a title id keeps its leading zeros (a string)
    expect(parse_info("title_id 00050000101435AB\n").titleId == "00050000101435ab", "info: the title id is a string, lower case");
    // an archive with the game, its update and DLC (no title chosen)
    SourceInfo a = parse_info("format wua\ntitle 0005000e10143500 16 0005000e10143500_v16 3 100\r\n"
                              "title 0005000010143500 0 0005000010143500_v0 6 2000\ntitle 0005000c10143500 5 0005000c10143500_v5 1 9\n");
    expect(a.archive && a.titles.size() == 3 && a.titleId.empty(), "info: archive titles (CRLF too)");
    std::string why;
    const ArchiveTitle* t = choose_archive_title(a.titles, why);
    expect(t && t->folder == "0005000010143500_v0", "archive: the game itself is chosen, not its update or DLC");
    SourceInfo u = parse_info("format wua\ntitle 0005000e10143500 16 0005000e10143500_v16 3 100\n");
    expect(!choose_archive_title(u.titles, why) && why.find("only an update") != std::string::npos, "archive: only an update -> explained");
    expect(!choose_archive_title({}, why) && why.find("no Wii U titles") != std::string::npos, "archive: no titles -> explained");
    SourceInfo two = parse_info("format wua\ntitle 0005000010999900 0 0005000010999900_v0 1 1\ntitle 0005000010143600 0 0005000010143600_v0 1 1\n");
    t = choose_archive_title(two.titles, why);
    expect(t && t->id == "0005000010143600", "archive: of several games, the supported one");
    SourceInfo other = parse_info("format wua\ntitle 0005000010999900 0 0005000010999900_v0 1 1\ntitle 0005000010888800 0 0005000010888800_v0 1 1\n");
    expect(!choose_archive_title(other.titles, why), "archive: several unknown games -> none chosen");

    // the name from meta.xml
    Meta m = parse_meta(kMeta);
    expect(m.title == "The Legend of Zelda: The Wind Waker HD" && m.titleId == "0005000010143500", "meta.xml: name (its two lines) and title id");
    expect(parse_meta("<menu><shortname_en>A &amp; B</shortname_en></menu>").title == "A & B", "meta.xml: shortname, entities");
    expect(parse_meta(std::string("\x01\x02\x03 binary", 10)).title.empty(), "meta.xml: not XML -> unknown title");
    expect(known_title("0005000010143500") && known_title("0005000010143500")->supported && known_title("0005000010143400") &&
               !known_title("0005000010143400")->supported && !known_title("0005000010999900"),
           "known versions: USA and Europe supported, Japan known, others not");
    expect(title_text("0005000010143500") == "00050000-10143500", "title id shown as the setup shows it");

    // exit codes in plain words, with the extractor's own detail
    expect(extract_error(3, "", false).find("disc image's own key") != std::string::npos, "exit 3: the disc key");
    expect(extract_error(5, "", false).find("common key is missing") != std::string::npos, "exit 5: missing prerequisite (common key)");
    expect(extract_error(7, "", false).find("could not be recognized as a supported Wii U game") != std::string::npos, "exit 7: not a game");
    expect(extract_error(8, "", true).find("damaged") != std::string::npos, "exit 8: damaged");
    expect(extract_error(9, "", false).find("not writable") != std::string::npos, "exit 9: cannot write");
    expect(error_detail("progress 1 2\nerror: disc key not found: x\n") == "disc key not found: x", "the extractor's error line kept");

    // progress lines
    Progress p;
    expect(parse_progress("phase verify", p) && p.phase == "verify", "progress: phase");
    expect(parse_progress("progress 5 10", p) && p.done == 5 && p.total == 10, "progress: bytes");
    Progress q;
    expect(parse_progress("progress 0 100", q) && q.phase == "extract", "progress: a disc image's (no phase line)");
    expect(!parse_progress("title_id 0005000010143500", q), "progress: other lines are not progress");
}

static void test_destination(const fs::path& work) {
    const fs::path base = work / u8path("Zielordner mit Leerzeichen \xc3\xa4\xc3\xb6\xc3\xbc \xe3\x82\xbc\xe3\x83\xab\xe3\x83\x80");
    std::error_code ec;
    fs::remove_all(base, ec);
    // a new folder (spaces and non-ASCII letters)
    Destination d = check_destination(utf8(base / "new game"));
    expect(d.valid && !d.exists && d.empty, "destination: a new folder under a Unicode path with spaces");
    expect(!check_destination("").valid && !check_destination("relative/path").valid, "destination: empty or relative refused");
    // the launcher's own folder
    fs::create_directories(base / "app");
    expect(!check_destination(utf8(base / "app"), utf8(base / "app")).valid, "destination: never the launcher's own folder");
    // a file, not a folder
    write(base / "a file", "x");
    expect(!check_destination(utf8(base / "a file")).valid, "destination: a file is refused");
    // unrelated files
    write(base / "busy" / "notes.txt", "keep me");
    d = check_destination(utf8(base / "busy"));
    expect(d.valid && d.exists && !d.empty && !d.hasGame, "destination: a folder with unrelated files (not empty, no game)");
    // an existing extraction
    fake_game(base / "done");
    d = check_destination(utf8(base / "done"));
    expect(d.valid && d.hasGame && d.gameTitleId == "0005000010143500", "destination: an existing game and its title id");
#ifndef _WIN32
    // not writable
    fs::create_directories(base / "locked");
    fs::permissions(base / "locked", fs::perms::owner_read | fs::perms::owner_exec);
    if (getuid() != 0) {
        d = check_destination(utf8(base / "locked" / "game"));
        expect(!d.valid && d.problem.find("not writable") != std::string::npos, "destination: not writable -> explained");
    }
    fs::permissions(base / "locked", fs::perms::owner_all);
#endif
    expect(check_destination(utf8(base / "new game")).spaceKnown, "destination: free space known here");

    // the extracted game checked
    Verify v = verify_game(utf8(base / "done"), "0005000010143500");
    expect(v.ok && v.titleId == "0005000010143500", "verify: a complete game");
    expect(!verify_game(utf8(base / "done"), "0005000010143600").ok, "verify: another title id than the source's -> not ok");
    fake_game(base / "empty rpx2");
    write(base / "empty rpx2" / "code" / "cking.rpx", "");
    expect(!verify_game(utf8(base / "empty rpx2"), "").ok, "verify: an empty cking.rpx -> not ok");
    expect(!verify_game(utf8(base / "busy"), "").ok, "verify: a folder without the game -> not ok");
}

static void test_config(const fs::path& work) {
    const fs::path dir = work / u8path("config \xc3\xa9t\xc3\xa9");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path ini = dir / "vr-launcher.ini";
    // a new file
    ConfigResult r = configure_game_dir(utf8(ini), "/home/someone/Games/Wind Waker HD", rename_replace);
    expect(r.ok && r.backup.empty() && slurp(ini) == "launcher.format=1\nlauncher.gameDir=/home/someone/Games/Wind Waker HD\n",
           "config: a new file gets the format and the game folder");
    // an existing one: comments, other keys and their order kept; a backup made
    write(ini, "# my notes\nlauncher.format=1\nlauncher.gameExe=/opt/wwhd/wwhd\nlauncher.gameDir=/old/game\nsomething.else=42\n");
    const std::string before = slurp(ini);
    r = configure_game_dir(utf8(ini), "/new/Spiel \xc3\xa4", rename_replace);
    expect(r.ok && slurp(ini) == "# my notes\nlauncher.format=1\nlauncher.gameExe=/opt/wwhd/wwhd\nlauncher.gameDir=/new/Spiel \xc3\xa4\n"
                                  "something.else=42\n",
           "config: every other line kept, the game folder replaced in place");
    expect(!r.backup.empty() && slurp(u8path(r.backup)) == before, "config: the previous file backed up unchanged");
    // a second change: a new backup name, the first kept
    const std::string firstBackup = r.backup;
    r = configure_game_dir(utf8(ini), "C:\\Games\\Wind Waker HD", rename_replace);
    expect(r.ok && r.backup != firstBackup && fs::exists(u8path(firstBackup)), "config: backups never overwrite one another");
    // Windows paths kept as written (the launcher reads them back verbatim)
    vr::settings::File f;
    std::string e, got;
    expect(f.load(utf8(ini), e) && f.get(kGameDirKey, got) && got == "C:\\Games\\Wind Waker HD", "config: a Windows drive path kept as written");
    r = configure_game_dir(utf8(ini), "\\\\server\\share\\Wind Waker HD", rename_replace);
    expect(r.ok && f.load(utf8(ini), e) && f.get(kGameDirKey, got) && got == "\\\\server\\share\\Wind Waker HD", "config: a UNC path kept as written");
    // the replace step failing: the file as it was, an error
    const std::string kept = slurp(ini);
    r = configure_game_dir(utf8(ini), "/elsewhere", [](const std::string&, const std::string&) { return false; });
    expect(!r.ok && !r.error.empty() && slurp(ini) == kept, "config: a failed write leaves the file as it was, with an error");
    // recovery: the backup restores the earlier file
    fs::copy_file(u8path(firstBackup), ini, fs::copy_options::overwrite_existing);
    expect(slurp(ini) == before, "config: a backup restores the earlier file");
#ifndef _WIN32
    // a folder that cannot be written: an error, nothing claimed
    const fs::path ro = work / "readonly config";
    fs::create_directories(ro);
    write(ro / "vr-launcher.ini", "launcher.gameDir=/old\n");
    fs::permissions(ro, fs::perms::owner_read | fs::perms::owner_exec);
    if (getuid() != 0) {
        r = configure_game_dir(utf8(ro / "vr-launcher.ini"), "/new", rename_replace);
        expect(!r.ok && slurp(ro / "vr-launcher.ini") == "launcher.gameDir=/old\n", "config: an unwritable folder -> error, file unchanged");
    }
    fs::permissions(ro, fs::perms::owner_all);
#endif
}

// wwhd-extract through the runner, as the tab runs it
static Runner::Snapshot run(const std::vector<std::string>& args) {
    Runner r;
    if (!r.start(args)) return r.snapshot();
    return r.wait();
}
static bool run_quiet(const std::string& cmd) { return std::system(cmd.c_str()) == 0; }
static std::string q(const fs::path& p) { return "\"" + utf8(p) + "\""; }

static void test_extractor(const std::string& x, const std::string& extractTest, const std::string& wuaTest, const fs::path& work) {
    // the fixtures: the extractor's own tests, their files kept
    const fs::path fx = work / "fixtures";
    std::error_code ec;
    fs::remove_all(fx, ec);
#ifdef _WIN32
    _putenv_s("WWHD_KEEP_TEST", "1");
#else
    setenv("WWHD_KEEP_TEST", "1", 1);
#endif
    const bool built = run_quiet(q(extractTest) + " " + q(x) + " " + q(fx / "disc") + " > " + q(work / "fx1.log")) &&
                       run_quiet(q(wuaTest) + " " + q(x) + " " + q(fx / "wua") + " > " + q(work / "fx2.log"));
    expect(built, "fixtures built by the extractor's own tests");
    if (!built) return;
    // copied into a folder with spaces and non-ASCII letters
    const fs::path src = work / u8path("Quelle mit Leerzeichen \xc3\xa4 \xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0");
    fs::remove_all(src, ec);
    fs::create_directories(src);
    for (const char* n : {"disc.wud", "disc.key", "common_raw.bin", "disc.wux", "disc_wux.key", "damaged.wud", "damaged.key"})
        fs::copy_file(fx / "disc" / n, src / n, ec);
    fs::copy_file(fx / "wua" / "game.wua", src / "game.wua", ec);
    const std::string ck = utf8(src / "common_raw.bin");

    // .wud: info with the keys, then the tab's meta step, then a full extraction
    Runner::Snapshot s = run({x, "--common-key", ck, "info", utf8(src / "disc.wud")});
    SourceInfo i = parse_info(s.output);
    expect(s.exitCode == 0 && !i.archive && i.titleId == "0005000010143500", ".wud: info through the runner (Unicode path)");
    const fs::path meta = work / "meta tmp";
    s = run({x, "--common-key", ck, "--only", "meta/meta.xml", "--progress", "extract", utf8(src / "disc.wud"), utf8(meta)});
    expect(s.exitCode == 0 && fs::is_regular_file(meta / "meta" / "meta.xml") && !fs::exists(meta / "code"),
           ".wud: only meta/meta.xml extracted for the name");
    expect(parse_meta(read_file(meta / "meta" / "meta.xml")).title.empty(), ".wud: the synthetic meta.xml has no name -> Unknown title");
    const fs::path out = work / u8path("Ziel \xc3\xbc wud");
    fs::remove_all(out, ec);
    s = run({x, "--common-key", ck, "--progress", "extract", utf8(src / "disc.wud"), utf8(out)});
    expect(s.exitCode == 0 && !s.cancelled && s.progress.total > 0 && s.progress.done == s.progress.total,
           ".wud: extracted, progress reached its total");
    expect(fs::is_regular_file(out / "code" / "cking.rpx") || fs::exists(out / "meta" / "meta.xml"), ".wud: files written");

    // .wux
    s = run({x, "--common-key", ck, "--disc-key", utf8(src / "disc_wux.key"), "info", utf8(src / "disc.wux")});
    expect(s.exitCode == 0 && parse_info(s.output).titleId == "0005000010143500", ".wux: info with a chosen disc key");
    // a missing prerequisite: no common key anywhere
    s = run({x, "info", utf8(src / "disc.wux")});
    expect(s.exitCode == 3 || s.exitCode == 5, ".wux: a missing key -> exit " + std::to_string(s.exitCode));
    expect(!error_detail(s.output).empty(), "  ... with the extractor's own words");
    // damaged
    s = run({x, "--common-key", ck, "extract", utf8(src / "damaged.wud"), utf8(work / "out dmg")});
    expect(s.exitCode == 8, "damaged image -> exit 8 (" + std::to_string(s.exitCode) + ")");

    // .wua: the archive's titles, the game chosen, extracted with its checksum phase
    s = run({x, "info", utf8(src / "game.wua")});
    i = parse_info(s.output);
    std::string why;
    const ArchiveTitle* t = choose_archive_title(i.titles, why);
    expect(s.exitCode == 0 && i.archive && t && t->folder == "0005000010143500_v0", ".wua: titles listed, the game chosen");
    const fs::path outw = work / u8path("Ziel \xc3\xbc wua");
    fs::remove_all(outw, ec);
    s = run({x, "--title", t ? t->folder : "", "--progress", "extract", utf8(src / "game.wua"), utf8(outw)});
    expect(s.exitCode == 0 && s.progress.phase == "extract" && s.progress.done == s.progress.total, ".wua: extracted with progress");
    expect(fs::is_regular_file(outw / "code" / "cking.rpx") && fs::is_directory(outw / "meta"), ".wua: the game's folders written");
    // an unsupported file
    write(src / "notes.wud", "this is not a disc image at all, just text");
    s = run({x, "--common-key", ck, "--disc-key", utf8(src / "disc.key"), "info", utf8(src / "notes.wud")});
    expect(s.exitCode == 7, "an unsupported file -> exit 7 (" + std::to_string(s.exitCode) + ")");
    write(src / "fake.wua", "not an archive");
    s = run({x, "info", utf8(src / "fake.wua")});
    expect(s.exitCode == 7 || s.exitCode == 8, "a broken .wua -> exit " + std::to_string(s.exitCode));
    // a program that is not there
    Runner missing;
    expect(!missing.start({utf8(work / "no such program"), "info"}) && missing.snapshot().startFailed, "runner: a missing program -> start fails");
}

#ifndef _WIN32
static void test_cancel(const fs::path& work) {
    // a stand-in that reports progress and never finishes
    const fs::path script = work / "slow extractor.sh";
    write(script, "#!/bin/sh\necho phase extract\necho progress 10 100\nexec sleep 30\n");
    fs::permissions(script, fs::perms::owner_all);
    Runner r;
    expect(r.start({utf8(script)}), "cancel: started");
    for (int i = 0; i < 200 && r.snapshot().progress.done == 0; ++i) SDL_Delay(10);
    expect(r.running() && r.snapshot().progress.done == 10, "cancel: progress read while it runs");
    const Uint64 t0 = SDL_GetTicks();
    r.cancel();
    const auto s = r.wait();
    expect(s.finished && s.cancelled && SDL_GetTicks() - t0 < 5000, "cancel: stopped and reported as cancelled (not success)");
}
#endif

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: launcher_install_test WWHD_EXTRACT EXTRACT_TEST WUA_TEST WORKDIR\n");
        return 2;
    }
    const fs::path work = fs::absolute(u8path(argv[4]));
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work);
    test_parsing();
    test_destination(work);
    test_config(work);
    test_extractor(argv[1], argv[2], argv[3], work);
#ifndef _WIN32
    test_cancel(work);
#endif
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    if (!failures) fs::remove_all(work, ec);
    return failures ? 1 : 0;
}
