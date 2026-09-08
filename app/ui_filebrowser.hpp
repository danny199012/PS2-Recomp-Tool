// SPDX-License-Identifier: GPL-3.0-only
//
// ui_filebrowser.hpp — small in-app file/folder browser for the SDL3 GUI apps.
//
// Used instead of OS-native file dialogs (SDL_ShowOpenFileDialog and friends)
// so that "Browse" buttons work identically on every SDL3 build and platform,
// without relying on the OS dialog subsystem.

#pragma once

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace uitools {

class FileBrowser {
public:
    // Open the browser. `filter` is a ';'-separated list of lowered file
    // extensions (e.g. ".elf" or ".iso;.bin"); empty = show all files.
    // With folder_mode=true only folders are listed and "Select this folder"
    // returns the current directory.
    void open(std::string filter, bool folder_mode = false) {
        open_ = true;
        folder_mode_ = folder_mode;
        exts_.clear();
        std::string cur;
        for (char c : filter) {
            if (c == ';') { add_ext_(cur); cur.clear(); }
            else cur += char(std::tolower((unsigned char)c));
        }
        add_ext_(cur);
        result_ready = false;
        result.clear();
        std::error_code ec;
        cwd_ = std::filesystem::current_path(ec);
        refresh_();
    }

    bool is_open() const { return open_; }

    // Set by draw() when the user picked something; result holds the path.
    bool result_ready = false;
    std::string result;

    // Draw the window (call every frame while open).
    void draw() {
        if (!open_) return;
        if (!ImGui::Begin("Browse", &open_, ImGuiWindowFlags_NoCollapse)) {
            ImGui::End();
            return;
        }
        {
            char buf[1024];
            std::snprintf(buf, sizeof buf, "Current: %s", cwd_.generic_string().c_str());
            ImGui::Text("%s", buf);
        }
        ImGui::SameLine();
        if (ImGui::Button("Up")) {
            std::filesystem::path parent = cwd_.parent_path();
            if (!parent.empty() && parent != cwd_) { cwd_ = parent; refresh_(); }
        }
        ImGui::Separator();

        ImGui::Text("Folders");
        ImGui::BeginChild("bdirs", ImVec2(0, 120), ImGuiChildFlags_Border);
        if (dirs_.empty()) ImGui::TextDisabled("(no subfolders)");
        for (const auto& d : dirs_) {
            const std::string label = "[D] " + d.filename().generic_string();
            if (ImGui::Selectable(label.c_str())) {
                cwd_ = d;
                refresh_();
                break;
            }
        }
        ImGui::EndChild();

        if (!folder_mode_) {
            ImGui::Text("Files");
            ImGui::BeginChild("bfiles", ImVec2(0, 200), ImGuiChildFlags_Border);
            if (files_.empty()) ImGui::TextDisabled("(no matching files)");
            for (size_t i = 0; i < files_.size(); ++i) {
                const std::string label = files_[i].filename().generic_string();
                if (ImGui::Selectable(label.c_str(), i == sel_idx_)) sel_idx_ = i;
            }
            ImGui::EndChild();
        }
        ImGui::Separator();

        if (folder_mode_) {
            if (ImGui::Button("Select this folder")) {
                result = cwd_.generic_string();
                result_ready = true;
                open_ = false;
            }
        } else {
            if (ImGui::Button("Open")) {
                if (sel_idx_ < files_.size()) {
                    result = files_[sel_idx_].generic_string();
                    result_ready = true;
                    open_ = false;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) open_ = false;
        ImGui::End();
    }

private:
    bool open_ = false;
    bool folder_mode_ = false;
    std::vector<std::string> exts_;
    std::filesystem::path cwd_;
    size_t sel_idx_ = 0;
    std::vector<std::filesystem::path> dirs_;
    std::vector<std::filesystem::path> files_;

    void add_ext_(std::string ext) {
        if (ext.empty()) return;
        for (char& c : ext) c = char(std::tolower((unsigned char)c));
        if (ext == ".") return;
        exts_.push_back(std::move(ext));
    }

    bool matches_(const std::filesystem::path& p) const {
        if (exts_.empty()) return true;
        std::string ext = p.extension().generic_string();
        for (char& c : ext) c = char(std::tolower((unsigned char)c));
        for (const auto& want : exts_)
            if (ext == want) return true;
        return false;
    }

    void refresh_() {
        dirs_.clear();
        files_.clear();
        sel_idx_ = 0;
        std::error_code ec;
        std::filesystem::directory_iterator it(
            cwd_, std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec)) {
            std::error_code lec;
            const std::filesystem::path p = it->path();
            const std::string name = p.filename().generic_string();
            if (name.empty() || name[0] == '.') continue;
            if (it->is_directory(lec)) {
                dirs_.push_back(p);
            } else if (!folder_mode_ && matches_(p)) {
                files_.push_back(p);
            }
        }
        std::sort(dirs_.begin(), dirs_.end());
        std::sort(files_.begin(), files_.end());
    }
};

} // namespace uitools