// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
#include "local_conf.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include "log.h"

namespace woc {

const char *const kFeatureKeys[4] = {"hosted_vpp_scale", "always_packed", "hud_crop_cx_pm", "hud_crop_cy_pm"};

namespace {

std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool feature_key(const std::string &key)
{
    return std::find_if(std::begin(kFeatureKeys), std::end(kFeatureKeys), [&](const char *k) { return key == k; }) !=
           std::end(kFeatureKeys);
}

/* An opt-in key's value: 0 or 1; the HUD zoom's centre a whole number 0-1000 (per mille). */
bool feature_valid(const std::string &key, const std::string &value)
{
    if (key == "hud_crop_cx_pm" || key == "hud_crop_cy_pm") {
        char *end = nullptr;
        const long v = std::strtol(value.c_str(), &end, 10);
        return !value.empty() && !*end && value[0] != '-' && value[0] != '+' && v >= 0 && v <= 1000;
    }
    return value == "0" || value == "1";
}

/* The key of a key = value or key += value line. */
std::string key_of(const std::string &line)
{
    std::string key = trim(line.substr(0, line.find('=')));
    if (!key.empty() && key.back() == '+')
        key = trim(key.substr(0, key.size() - 1));
    return key;
}

}  // namespace

void load_local_conf(cld::Config *cfg, const std::string &path, std::vector<std::string> *warnings)
{
    if (path.empty() || access(path.c_str(), F_OK) != 0)
        return;
    std::ifstream f(path);
    if (!f) {
        warnings->push_back(path + ": cannot read it: the shipped settings instead");
        return;
    }
    std::string line, err;
    for (int n = 1; std::getline(f, line); n++) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos)
            line.erase(hash);
        line = trim(line);
        if (line.empty())
            continue;
        const std::string where = path + ":" + std::to_string(n) + ": ";
        if (feature_key(key_of(line)))
            warnings->push_back(where + key_of(line) + " is left out: it goes in features_file (patchd moves it there)");
        else if (!cfg->set_pair(line, &err))
            warnings->push_back(where + err + ": left out");
    }
}

void load_features(cld::Config *cfg, const std::string &path)
{
    if (path.empty() || access(path.c_str(), F_OK) != 0)
        return;
    std::ifstream f(path);
    if (!f) {
        log_warn("%s: cannot read it: the opt-in settings off", path.c_str());
        return;
    }
    std::string line, applied, err;
    while (std::getline(f, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos)
            line.erase(hash);
        if (trim(line).empty())
            continue;
        const size_t eq = line.find('=');
        const std::string key = key_of(line), value = eq == std::string::npos ? "" : trim(line.substr(eq + 1));
        if (!feature_key(key) || !feature_valid(key, value) || !cfg->set_pair(key + "=" + value, &err)) {
            log_warn("%s: \"%s\" left out", path.c_str(), trim(line).c_str());
            continue;
        }
        applied += (applied.empty() ? "" : ", ") + key + " " + value;
    }
    if (!applied.empty())
        log_info("opt-in settings from %s: %s", path.c_str(), applied.c_str());
}

}  // namespace woc
