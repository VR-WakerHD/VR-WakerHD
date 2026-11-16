// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The PS VR2 Sense controllers' binding for SteamVR, set by the VR launcher (docs/VR_CONTROLS.md, "PS VR2 Sense
// controllers"). SteamVR has no OpenXR profile for the Sense controllers and maps the game's Oculus Touch bindings onto
// them; with no binding saved for the game its automatic mapping put the menu on an input the Sense controller does not
// have (a test build's players: "menu buttons don't seem to be mapped"). The binding written here is the one that works
// in the headset (2026-11-15, the developer's PS VR2, SteamVR 2.18.4: Workshop binding 3825041755): Touch compatibility
// mode, L2/R2 triggers, L1/R1 grips, Square/Cross primary, Triangle/Circle secondary, Create the menu, the sticks and
// their clicks, the handgrip and aim poses, haptics. Its app key is the game's on this machine: SteamVR keys an OpenXR
// game by "system.generated.openxr." + its OpenXR application name + "." + its executable's file name, lower case.
// SteamVR is pointed at it in its settings file (config/steamvr.vrsettings), only while SteamVR is closed: it writes
// that file back when it exits. Pure text work here (no files): steamvr_binding_test checks it.
#pragma once
#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace steamvr_binding {

constexpr const char* kSettingKey = "playstation_vr2_sense_250820_CurrentURL_openxr";
constexpr const char* kPreviousKey = "playstation_vr2_sense_250820_PreviousURL_openxr";

// the game's app key from its executable's file name ("wwhd.exe"; "wwhd" on Linux)
inline std::string app_key(const std::string& exeFileName) {
    std::string k = "system.generated.openxr.the wind waker hd." + exeFileName;
    for (char& ch : k) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return k;
}

// a file URL as SteamVR writes them: "file:///C:/Program%20Files%20%28x86%29/..." (letters, digits and - _ . / : ~ as
// they are, everything else percent-encoded; backslashes turned into slashes)
inline std::string file_url(std::string path) {
    for (char& ch : path)
        if (ch == '\\') ch = '/';
    std::string out = "file:///";
    if (!path.empty() && path[0] == '/') path.erase(0, 1);
    for (unsigned char ch : path) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '/' || ch == ':' || ch == '~') {
            out += char(ch);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", ch);
            out += buf;
        }
    }
    return out;
}

// ---- a small JSON value (objects keep their order: SteamVR's file is written back as it was, one value changed)
struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    std::string text;  // a number as written, or a string's value
    std::vector<Json> items;
    // (an object's key and value: its own type, completed after Json, with every function that makes or ends a Json defined
    // after it. A std::pair<std::string, Json> inside Json is a pair
    // of an incomplete type, which GCC 16's standard library rejects: the Linux launcher failed to build, 2026-11-15)
    struct Member;
    std::vector<Member> members;
    Json* find(const std::string& key);
    Json& at(const std::string& key);  // (added at the end when missing)
    static Json string(const std::string& s);
};
struct Json::Member {
    std::string first;
    Json second;
};
inline Json* Json::find(const std::string& key) {
    for (auto& m : members)
        if (m.first == key) return &m.second;
    return nullptr;
}
inline Json& Json::at(const std::string& key) {
    if (Json* j = find(key)) return *j;
    members.push_back({key, Json{}});
    return members.back().second;
}
inline Json Json::string(const std::string& s) {
    Json j;
    j.kind = String;
    j.text = s;
    return j;
}

struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool eat(char ch) {
        ws();
        if (i < s.size() && s[i] == ch) return ++i, true;
        return false;
    }
    std::string str() {
        std::string out;
        if (!eat('"')) return ok = false, out;
        while (i < s.size() && s[i] != '"') {
            char ch = s[i++];
            if (ch == '\\' && i < s.size()) {
                const char e = s[i++];
                switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {  // (kept as written: SteamVR's file is ASCII with \u escapes only for what it needs)
                    out += "\\u";
                    for (int k = 0; k < 4 && i < s.size(); ++k) out += s[i++];
                    break;
                }
                default: out += e;  // " \ /
                }
            } else {
                out += ch;
            }
        }
        if (i >= s.size()) return ok = false, out;
        ++i;
        return out;
    }
    Json value() {
        Json j;
        ws();
        if (i >= s.size()) return ok = false, j;
        const char ch = s[i];
        if (ch == '{') {
            ++i;
            j.kind = Json::Object;
            if (eat('}')) return j;
            do {
                std::string k = str();
                if (!ok || !eat(':')) return ok = false, j;
                Json v = value();
                if (!ok) return j;
                j.members.push_back({std::move(k), std::move(v)});
            } while (eat(','));
            if (!eat('}')) ok = false;
        } else if (ch == '[') {
            ++i;
            j.kind = Json::Array;
            if (eat(']')) return j;
            do {
                j.items.push_back(value());
                if (!ok) return j;
            } while (eat(','));
            if (!eat(']')) ok = false;
        } else if (ch == '"') {
            j.kind = Json::String;
            j.text = str();
        } else if (s.compare(i, 4, "true") == 0) {
            j.kind = Json::Bool, j.b = true, i += 4;
        } else if (s.compare(i, 5, "false") == 0) {
            j.kind = Json::Bool, i += 5;
        } else if (s.compare(i, 4, "null") == 0) {
            i += 4;
        } else {
            const size_t start = i;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' ||
                                    s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
                ++i;
            if (i == start) return ok = false, j;
            j.kind = Json::Number;
            j.text = s.substr(start, i - start);
        }
        return j;
    }
};

inline bool parse(const std::string& text, Json& out) {
    Parser p{text};
    out = p.value();
    p.ws();
    return p.ok && p.i == text.size() && out.kind == Json::Object;
}

inline void quote(std::string& out, const std::string& s) {
    out += '"';
    for (size_t k = 0; k < s.size(); ++k) {
        const char ch = s[k];
        if (ch == '\\' && k + 1 < s.size() && s[k + 1] == 'u') { out += "\\u"; ++k; continue; }  // (a kept \u escape)
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default: out += ch;
        }
    }
    out += '"';
}

// written as SteamVR writes its settings: three spaces an indent, "key" : value
inline void dump(const Json& j, std::string& out, int indent = 0) {
    const std::string pad(size_t(indent) * 3, ' '), inner(size_t(indent + 1) * 3, ' ');
    switch (j.kind) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.b ? "true" : "false"; break;
    case Json::Number: out += j.text; break;
    case Json::String: quote(out, j.text); break;
    case Json::Array:
        if (j.items.empty()) { out += "[]"; break; }
        out += "[\n";
        for (size_t k = 0; k < j.items.size(); ++k) {
            out += inner;
            dump(j.items[k], out, indent + 1);
            out += k + 1 < j.items.size() ? ",\n" : "\n";
        }
        out += pad + "]";
        break;
    case Json::Object:
        if (j.members.empty()) { out += "{}"; break; }
        out += "{\n";
        for (size_t k = 0; k < j.members.size(); ++k) {
            out += inner;
            quote(out, j.members[k].first);
            out += " : ";
            dump(j.members[k].second, out, indent + 1);
            out += k + 1 < j.members.size() ? ",\n" : "\n";
        }
        out += pad + "}";
        break;
    }
}

// SteamVR's settings with the game's PS VR2 binding set to `url` (the binding before it kept as the previous one).
// False (and `error`) when the settings are not a JSON object; `changed` false when it was already set
inline bool set_binding(const std::string& settings, const std::string& appKey, const std::string& url, std::string& out,
                        std::string& error, bool& changed) {
    Json root;
    if (settings.find_first_not_of(" \t\r\n") == std::string::npos) {
        root.kind = Json::Object;  // (no settings yet)
    } else if (!parse(settings, root)) {
        error = "SteamVR's settings file could not be read (not JSON)";
        return false;
    }
    Json& app = root.at(appKey);
    if (app.kind != Json::Object) app = Json{}, app.kind = Json::Object;
    Json& current = app.at(kSettingKey);
    changed = !(current.kind == Json::String && current.text == url);
    if (changed) {
        if (current.kind == Json::String && !current.text.empty()) app.at(kPreviousKey) = Json::string(current.text);
        app.at(kSettingKey) = Json::string(url);
    }
    out.clear();
    dump(root, out);
    out += "\n";
    // (its line endings as the file had them: SteamVR on Windows writes CRLF)
    if (settings.find("\r\n") != std::string::npos) {
        std::string crlf;
        crlf.reserve(out.size() + out.size() / 20);
        for (char ch : out) {
            if (ch == '\n') crlf += '\r';
            crlf += ch;
        }
        out.swap(crlf);
    }
    return true;
}

// the binding file (the one that works in the headset: see above), keyed to `appKey`
inline std::string binding_file(const std::string& appKey) {
    std::string key;
    quote(key, appKey);
    auto in = [](const char* path, const char* mode, const char* input, const char* action) {
        return std::string("            {\n               \"inputs\" : {\n                  \"") + input +
               "\" : {\n                     \"output\" : \"/actions/gameplay/in/" + action +
               "\"\n                  }\n               },\n               \"mode\" : \"" + mode +
               "\",\n               \"path\" : \"" + path + "\"\n            }";
    };
    const std::string sources[] = {
        in("/user/hand/left/input/create", "button", "click", "menu"),
        in("/user/hand/left/input/create", "button", "touch", "menu"),
        in("/user/hand/right/input/create", "button", "click", "menu"),
        in("/user/hand/right/input/create", "button", "touch", "menu"),
        in("/user/hand/left/input/l1", "trigger", "pull", "grip"),
        in("/user/hand/right/input/r1", "trigger", "pull", "grip"),
        in("/user/hand/left/input/l2", "trigger", "pull", "trigger"),
        in("/user/hand/right/input/r2", "trigger", "pull", "trigger"),
        in("/user/hand/left/input/left_stick", "joystick", "position", "stick"),
        in("/user/hand/right/input/right_stick", "joystick", "position", "stick"),
        in("/user/hand/left/input/left_stick", "button", "click", "stick_click"),
        in("/user/hand/right/input/right_stick", "button", "click", "stick_click"),
        in("/user/hand/left/input/square", "button", "click", "primary"),
        in("/user/hand/right/input/cross", "button", "click", "primary"),
        in("/user/hand/left/input/triangle", "button", "click", "secondary"),
        in("/user/hand/right/input/circle", "button", "click", "secondary"),
    };
    std::string src;
    for (size_t k = 0; k < sizeof sources / sizeof sources[0]; ++k)
        src += sources[k] + (k + 1 < sizeof sources / sizeof sources[0] ? ",\n" : "\n");
    return std::string("{\n   \"action_manifest_version\" : 0,\n   \"alias_info\" : {},\n   \"app_key\" : ") + key +
           ",\n   \"bindings\" : {\n      \"/actions/gameplay\" : {\n         \"haptics\" : [\n"
           "            { \"output\" : \"/actions/gameplay/out/haptic\", \"path\" : \"/user/hand/left/output/haptic\" },\n"
           "            { \"output\" : \"/actions/gameplay/out/haptic\", \"path\" : \"/user/hand/right/output/haptic\" }\n"
           "         ],\n         \"poses\" : [\n"
           "            { \"output\" : \"/actions/gameplay/in/grip_pose\", \"path\" : \"/user/hand/left/pose/handgrip\" },\n"
           "            { \"output\" : \"/actions/gameplay/in/aim_pose\", \"path\" : \"/user/hand/left/pose/openxr_aim\" },\n"
           "            { \"output\" : \"/actions/gameplay/in/grip_pose\", \"path\" : \"/user/hand/right/pose/handgrip\" },\n"
           "            { \"output\" : \"/actions/gameplay/in/aim_pose\", \"path\" : \"/user/hand/right/pose/openxr_aim\" }\n"
           "         ],\n         \"sources\" : [\n" +
           src +
           "         ]\n      }\n   },\n   \"category\" : \"openxr\",\n   \"controller_type\" : \"playstation_vr2_sense\",\n"
           "   \"description\" : \"The Wind Waker HD VR: PS VR2 Sense (set by the VR launcher)\",\n"
           "   \"interaction_profile\" : \"/interaction_profiles/oculus/touch_controller\",\n"
           "   \"name\" : \"The Wind Waker HD VR - PS VR2 Sense\",\n"
           "   \"options\" : {\n      \"compatibility_mode\" : true,\n      \"simulate_hmd\" : true,\n"
           "      \"simulate_rendermodel\" : true,\n      \"simulated_controller_type\" : \"oculus_touch\"\n   },\n"
           "   \"simulated_actions\" : []\n}\n";
}

}  // namespace steamvr_binding
