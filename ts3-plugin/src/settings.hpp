// settings.hpp — live-tunable DSP settings for the TS plugin.
// No platform or TS3 deps; unit-testable off-target (like radio_dsp.hpp).
//
// The values ship as an ini file (key = value, ';'/'#' comments) that the
// poll thread hot-reloads while TeamSpeak runs, and every key can also be
// set live from the TS chat: /rtr set <key> <value>. Defaults here are the
// constants the DSP used before they became tunable.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace rtr {

struct Settings {
    // Proximity voice
    float proxMaxDistM = 40.0f;  // inaudible beyond this many meters
    float proxRolloff  = 1.5f;   // falloff curve exponent
    float proxMaxPan   = 0.85f;  // cap so the far ear is never fully silent
    float proxSmooth   = 0.002f; // per-sample gain/pan slew (zipper guard)
    // Occlusion muffling
    float occlStrength    = 1.0f;     // scales the game's occlusion value
    float occlMinCutoffHz = 500.0f;   // lowpass cutoff when fully occluded
    float occlBypassHz    = 18000.0f; // cutoff at zero occlusion (transparent)
    float occlMaxAtten    = 0.55f;    // extra gain cut when fully occluded
    float occlSlewMs      = 150.0f;   // fade time for occlusion changes
    // Radio effect
    float radioDrive = 4.0f;  // distortion drive
    float radioNoise = 0.02f; // static noise level
    // Diagnostics
    float debugLog = 0.0f;    // 1 = dump received game state to the TS log
};

struct SettingDesc {
    const char* key;
    float Settings::* field;
    float min, max;
    const char* help;
};

inline const std::vector<SettingDesc>& settingsTable()
{
    static const std::vector<SettingDesc> t = {
        {"prox.maxdist",    &Settings::proxMaxDistM,   5.0f,   200.0f,
         "meters; voice is inaudible beyond this"},
        {"prox.rolloff",    &Settings::proxRolloff,    0.5f,   4.0f,
         "distance falloff exponent (higher = fades sooner)"},
        {"prox.maxpan",     &Settings::proxMaxPan,     0.0f,   1.0f,
         "stereo pan cap; 1 = hard pan, 0 = mono"},
        {"prox.smooth",     &Settings::proxSmooth,     0.0001f, 0.05f,
         "per-sample gain/pan slew (anti-zipper)"},
        {"occl.strength",   &Settings::occlStrength,   0.0f,   4.0f,
         "scales wall muffling; 0 disables occlusion"},
        {"occl.mincutoffhz", &Settings::occlMinCutoffHz, 100.0f, 8000.0f,
         "lowpass cutoff at full occlusion (lower = more muffled)"},
        {"occl.bypasshz",   &Settings::occlBypassHz,   4000.0f, 20000.0f,
         "lowpass cutoff at zero occlusion"},
        {"occl.maxatten",   &Settings::occlMaxAtten,   0.0f,   0.95f,
         "extra volume cut at full occlusion (0..0.95)"},
        {"occl.slewms",     &Settings::occlSlewMs,     10.0f,  2000.0f,
         "fade time when occlusion changes (corner transitions)"},
        {"radio.drive",     &Settings::radioDrive,     1.0f,   16.0f,
         "radio distortion drive"},
        {"radio.noise",     &Settings::radioNoise,     0.0f,   0.2f,
         "radio static level"},
        {"debug.log",       &Settings::debugLog,       0.0f,   1.0f,
         "1 = dump received game state to the TS log every ~2s"},
    };
    return t;
}

inline std::string settingsLowerTrim(std::string s)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// Set one value by key (case-insensitive), clamped to its range.
// Returns false and fills *err when the key is unknown.
inline bool settingsSet(Settings& s, const std::string& key, float value,
                        std::string* err = nullptr)
{
    const std::string k = settingsLowerTrim(key);
    for (const SettingDesc& d : settingsTable()) {
        if (k != d.key) continue;
        s.*(d.field) = std::clamp(value, d.min, d.max);
        return true;
    }
    if (err) *err = "unknown setting '" + k + "' (try: /rtr show)";
    return false;
}

// Parse "key = value" lines; unknown keys and malformed lines are skipped so
// an old ini never blocks loading. Returns number of values applied.
inline int settingsParseIni(Settings& s, std::istream& in)
{
    int applied = 0;
    std::string line;
    while (std::getline(in, line)) {
        const size_t cmt = line.find_first_of(";#");
        if (cmt != std::string::npos) line.erase(cmt);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = settingsLowerTrim(line.substr(0, eq));
        const std::string val = settingsLowerTrim(line.substr(eq + 1));
        if (key.empty() || val.empty()) continue;
        char* end = nullptr;
        const float f = std::strtof(val.c_str(), &end);
        if (end == val.c_str()) continue;
        if (settingsSet(s, key, f)) ++applied;
    }
    return applied;
}

// Commented ini with current values (defaults + ranges in the comments).
inline std::string settingsToIni(const Settings& s)
{
    const Settings defaults{};
    std::string out =
        "; RoN Tactical Radio - TeamSpeak plugin settings\n"
        "; Reloaded automatically (~1s) while TeamSpeak runs - edit, save, listen.\n"
        "; Also adjustable live from the TS chat:\n"
        ";   /rtr show            current values\n"
        ";   /rtr set <key> <val> change one value (until reload/restart)\n"
        ";   /rtr save            write current values to this file\n"
        ";   /rtr reload          re-read this file\n"
        ";   /rtr reset           back to defaults (in memory)\n\n";
    for (const SettingDesc& d : settingsTable()) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "%-16s = %-8g ; %s (default %g, %g..%g)\n",
                      d.key, double(s.*(d.field)), d.help,
                      double(defaults.*(d.field)), double(d.min), double(d.max));
        out += buf;
    }
    return out;
}

// One "key = value" line per setting, for /rtr show.
inline std::string settingsDescribe(const Settings& s)
{
    std::string out;
    for (const SettingDesc& d : settingsTable()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s = %g\n", d.key, double(s.*(d.field)));
        out += buf;
    }
    return out;
}

inline bool settingsLoad(Settings& s, const std::string& path)
{
    std::ifstream in(path);
    if (!in) return false;
    settingsParseIni(s, in);
    return true;
}

inline bool settingsSave(const Settings& s, const std::string& path)
{
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << settingsToIni(s);
    return bool(out);
}

} // namespace rtr
