#include "../../include/imgui/Settings.hpp"
#include "../../include/Config.hpp"
#include "../../include/GsrInfo.hpp"
#include "../../include/Translation.hpp"
#include "../../include/Theme.hpp"
#include "../../include/imgui/Host.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

namespace gsr {
    // set_xdg_autostart 在 src/Utils.cpp 定义（gsr 命名空间）
    int set_xdg_autostart(bool enable);
}

namespace gsr {
namespace imgui_ui {

    namespace {
        ImU32 accent_u32() {
            return ImGui::GetColorU32(ImGuiCol_ButtonActive);
        }

        // 全局缩放：与 Host::load_font / 通知 toast 一致，1080p 基准，夹在 [0.75, 2.0]
        float ui_scale() {
            return std::min(std::max(ImGui::GetIO().DisplaySize.y / 1080.0f, 0.75f), 2.0f);
        }

        // ---- 持久文本缓冲（ImGui::InputText 只接受 char*，这里桥接 std::string）----
        std::string& text_buf(const char* id) {
            static std::unordered_map<std::string, std::string> bufs;
            return bufs[id];
        }
        std::unordered_map<std::string, bool>& text_inited() {
            static std::unordered_map<std::string, bool> m;
            return m;
        }
        // 返回 true 表示本帧文本被改（value 已同步写入）
        bool input_text_cfg(const char* label, const char* id, std::string& value, bool secret = false) {
            std::string& buf = text_buf(id);
            auto& inited = text_inited();
            if(!inited[id]) { buf = value; inited[id] = true; }
            char tmp[1024];
            snprintf(tmp, sizeof(tmp), "%s", buf.c_str());
            ImGuiInputTextFlags flags = secret ? ImGuiInputTextFlags_Password : 0;
            ImGui::InputText(label, tmp, sizeof(tmp), flags);
            if(buf != tmp) {
                buf = tmp;
                if(value != buf) { value = buf; return true; }
            }
            return false;
        }

        struct AccentOption {
            const char* id;         // config 里存的值
            const char* label_key;  // TR 的英文原文
            mgl::Color color;
        };

        const AccentOption accent_options[] = {
            { "amd",    "Red",   mgl::Color(221, 0, 49) },
            { "nvidia", "Green", mgl::Color(118, 185, 0) },
            { "intel",  "Blue",  mgl::Color(8, 109, 183) },
        };

        struct LanguageOption {
            const char* label;      // 原样显示（语言自己的名字）
            const char* id;         // config 里存的值（空 = 跟随系统）
        };

        const LanguageOption language_options[] = {
            { "System language", "" },
            { "Deutsch",    "de" },
            { "English",    "en" },
            { "Español",   "es" },
            { "Français",   "fr" },
            { "Magyar",     "hu" },
            { "日本語",      "ja" },
            { "Português",  "pt" },
            { "Русский",    "ru" },
            { "Türkçe",     "tr" },
            { "Українська", "uk" },
            { "简体中文",     "zh_CN" },
        };

        const char* kImGuiUiVersion = "1.0";

        const char* gpu_vendor_to_string(GpuVendor vendor) {
            switch(vendor) {
                case GpuVendor::AMD:      return "AMD";
                case GpuVendor::INTEL:    return "Intel";
                case GpuVendor::NVIDIA:   return "NVIDIA";
                case GpuVendor::BROADCOM: return "Broadcom";
                case GpuVendor::APPLE:    return "Apple";
                default:                  return "Unknown";
            }
        }

        // ================= 现代化小组件 =================

        // 配色常量（深色面板 #1A1A1A 之上）
        static const ImU32 kCardBg     = IM_COL32(38, 42, 47, 255);   // 卡片底
        static const ImU32 kCardBorder = IM_COL32(255, 255, 255, 16);
        static const ImU32 kPillBg     = IM_COL32(255, 255, 255, 18); // 分段容器/开关关闭态
        static const ImU32 kTextSecond = IM_COL32(255, 255, 255, 140);

        // 分区标题：左侧强调色竖条 + 标题字体（替代原来的 Separator）
        void section_title(const char* text) {
            const float s = ui_scale();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushFont(ui_font(FontRole::Title));
            const float lh = ImGui::GetTextLineHeight();
            ImGui::PushStyleColor(ImGuiCol_Text, accent_u32());
            ImGui::TextUnformatted(TR(text));
            ImGui::PopStyleColor();
            ImGui::PopFont();
            // 竖条画在文字左侧（文字已占据该行，直接把条画在行左缘即可）
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(p.x, p.y + 3.0f * s), ImVec2(p.x + 4.0f * s, p.y + lh - 2.0f * s),
                accent_u32(), 2.0f * s);
            ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
        }

        // ---- 卡片分组：内容子窗口用双通道绘制，卡片底色提交到 0 号通道，
        //      控件在 1 号通道，合并后底色自然垫在控件下方且随滚动/裁剪移动 ----
        struct CardSpan { ImVec2 p0; float w; };
        void card_channels_begin() {
            ImGui::GetWindowDrawList()->ChannelsSplit(2);
            ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
        }
        void card_channels_end() {
            ImGui::GetWindowDrawList()->ChannelsMerge();
        }
        CardSpan begin_card() {
            const float s = ui_scale();
            ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
            CardSpan c;
            c.p0 = ImGui::GetCursorScreenPos();
            c.w = ImGui::GetContentRegionAvail().x;
            return c;
        }
        void end_card(const CardSpan& c) {
            const float s = ui_scale();
            const float pad = 12.0f * s;
            const float rounding = 12.0f * s;
            const ImVec2 p1(c.p0.x + c.w, ImGui::GetCursorScreenPos().y + 2.0f * s);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->ChannelsSetCurrent(0);
            dl->AddRectFilled(ImVec2(c.p0.x - pad, c.p0.y - pad), ImVec2(p1.x + pad, p1.y + pad), kCardBg, rounding);
            dl->AddRect(ImVec2(c.p0.x - pad, c.p0.y - pad), ImVec2(p1.x + pad, p1.y + pad), kCardBorder, rounding, ImDrawFlags_None, 1.0f);
            dl->ChannelsSetCurrent(1);
            ImGui::Dummy(ImVec2(0.0f, 8.0f * s));
        }

        // 行布局辅助：标签在左，控件右对齐。返回控件应放置的 x 偏移（相对行首）
        float row_right_x(float control_w) {
            const float row_w = ImGui::GetContentRegionAvail().x;
            return std::max(0.0f, row_w - control_w);
        }

        // ---- 胶囊开关（现代 toggle switch，带动画）----
        bool toggle_switch(const char* anim_key, bool* v) {
            const float s = ui_scale();
            const float w = 46.0f * s;
            const float h = 26.0f * s;
            const ImVec2 p = ImGui::GetCursorScreenPos();

            bool clicked = ImGui::InvisibleButton("##tg", ImVec2(w, h));
            if(clicked)
                *v = !*v;

            // 动画：按 id 缓存插值
            static std::unordered_map<std::string, float> anim;
            float& t = anim[anim_key];
            const float target = *v ? 1.0f : 0.0f;
            t += (target - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
            if(std::fabs(target - t) < 0.01f)
                t = target;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 bg = t > 0.5f ? accent_u32() : kPillBg;
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h * 0.5f);
            // 旋钮
            const float knob_d = h - 6.0f * s;
            const float kx = p.x + 3.0f * s + t * (w - h);
            const float ky = p.y + 3.0f * s;
            dl->AddCircleFilled(ImVec2(kx + knob_d * 0.5f, ky + knob_d * 0.5f), knob_d * 0.5f,
                IM_COL32(255, 255, 255, 245));
            return clicked;
        }

        // 复选行：标签在左，胶囊开关在右（替代 Checkbox）
        bool toggle_row(const char* label_key, bool* v) {
            ImGui::PushID(label_key);
            const float s = ui_scale();
            const float sw = 46.0f * s;
            ImGui::TextUnformatted(TR(label_key));
            ImGui::SameLine(row_right_x(sw));
            const bool changed = toggle_switch(label_key, v);
            ImGui::PopID();
            return changed;
        }

        // 整数输入行：标签在左，输入框右对齐
        bool input_int_row(const char* label_key, int* v) {
            ImGui::PushID(label_key);
            const float s = ui_scale();
            const float iw = std::min(220.0f * s, ImGui::GetContentRegionAvail().x * 0.5f);
            ImGui::TextUnformatted(TR(label_key));
            ImGui::SameLine(row_right_x(iw));
            ImGui::SetNextItemWidth(iw);
            const bool changed = ImGui::InputInt("##v", v);
            ImGui::PopID();
            return changed;
        }

        // ---- 分段选择器（segmented control）：容器 + 高亮当前段 ----
        bool segmented_row(const char* id, const char* const* labels, const char* const* ids, int count,
            const std::string& current, std::string* selected) {
            ImGui::PushID(id);
            const float s = ui_scale();
            const float pad_x = 14.0f * s;
            const float seg_h = 30.0f * s;

            std::vector<float> ws(count);
            float total = 0.0f;
            for(int i = 0; i < count; ++i) {
                ImGui::PushFont(ui_font(FontRole::Small));
                ws[i] = ImGui::CalcTextSize(TR(labels[i])).x + pad_x * 2.0f;
                ImGui::PopFont();
                total += ws[i];
            }

            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, ImVec2(p0.x + total, p0.y + seg_h), kPillBg, seg_h * 0.5f);

            bool changed = false;
            float x = p0.x;
            for(int i = 0; i < count; ++i) {
                const bool active = (current == ids[i]);
                ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
                ImGui::PushID(i);
                ImGui::InvisibleButton("##seg", ImVec2(ws[i], seg_h));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                if(ImGui::IsItemClicked()) {
                    *selected = ids[i];
                    changed = true;
                }
                // 段底色：当前段用强调色，悬停段微亮
                if(active || hovered) {
                    const ImU32 col = active ? accent_u32() : IM_COL32(255, 255, 255, 14);
                    dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + ws[i], p0.y + seg_h), col, seg_h * 0.5f);
                }
                // 文字居中
                ImGui::PushFont(ui_font(FontRole::Small));
                const ImVec2 ts = ImGui::CalcTextSize(TR(labels[i]));
                const ImU32 tc = active ? IM_COL32(255, 255, 255, 255) : kTextSecond;
                dl->AddText(ImVec2(x + (ws[i] - ts.x) * 0.5f, p0.y + (seg_h - ts.y) * 0.5f), tc, TR(labels[i]));
                ImGui::PopFont();
                x += ws[i];
            }
            // 恢复行光标，避免影响下一行布局
            ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + seg_h));
            ImGui::Dummy(ImVec2(total, seg_h));
            ImGui::PopID();
            return changed;
        }

        // 下拉行：标签在左，下拉在右。
        // 注意：PushID(label) —— 否则多个 BeginCombo("##c") 在同一窗口会 ID 冲突
        bool combo_row(const char* label, const char* const* ids, const char* const* labels, int count, std::string& current) {
            ImGui::PushID(label);
            const float s = ui_scale();
            bool changed = false;
            const char* cur_label = "";
            for(int i = 0; i < count; ++i)
                if(current == ids[i]) { cur_label = labels[i]; break; }

            const float cw = std::min(280.0f * s, ImGui::GetContentRegionAvail().x * 0.5f);
            ImGui::TextUnformatted(TR(label));
            ImGui::SameLine(row_right_x(cw));
            ImGui::SetNextItemWidth(cw);
            if(ImGui::BeginCombo("##c", TR(cur_label))) {
                for(int i = 0; i < count; ++i) {
                    bool sel = (current == ids[i]);
                    if(ImGui::Selectable(TR(labels[i]), sel)) { current = ids[i]; changed = true; }
                    if(sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
            return changed;
        }

        // 整数下拉行（id 即数字字符串），同样 PushID 防冲突
        bool combo_int(const char* label, const char* const* ids, int count, int& value) {
            ImGui::PushID(label);
            const float s = ui_scale();
            bool changed = false;
            const std::string cur = std::to_string(value);
            int idx = -1;
            for(int i = 0; i < count; ++i)
                if(ids[i] == cur) { idx = i; break; }

            const float cw = std::min(200.0f * s, ImGui::GetContentRegionAvail().x * 0.5f);
            ImGui::TextUnformatted(TR(label));
            ImGui::SameLine(row_right_x(cw));
            ImGui::SetNextItemWidth(cw);
            if(ImGui::BeginCombo("##ci", idx >= 0 ? ids[idx] : cur.c_str())) {
                for(int i = 0; i < count; ++i) {
                    if(ImGui::Selectable(ids[i], i == idx)) { value = std::atoi(ids[i]); changed = true; }
                    if(i == idx) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
            return changed;
        }

        std::vector<std::pair<std::string, std::string>> video_codec_options(const GsrInfo& info) {
            std::vector<std::pair<std::string, std::string>> opts = { { "auto", "Auto" } };
            const auto& c = info.supported_video_codecs;
            if(c.h264 || c.h264_software)  opts.push_back({ "h264", "H.264" });
            if(c.hevc)                      opts.push_back({ "hevc", "HEVC" });
            if(c.av1)                       opts.push_back({ "av1", "AV1" });
            if(c.vp9)                       opts.push_back({ "vp9", "VP9" });
            if(c.vp8)                       opts.push_back({ "vp8", "VP8" });
            return opts;
        }

        // ---- 捕获目标下拉 ----
        // 选项必须与后端 Overlay::validate_capture_target() 接受的值一致：
        //   window / focused / region / focused_monitor / portal / 各显示器名。
        // 旧代码里的 "screen" 后端并不认，选它会直接报
        // 「无法开始录制，捕获目标 "screen" 无效。请在设置中更改捕获目标。」
        struct AreaOpt { std::string id; std::string label; bool translate; };

        std::vector<AreaOpt> capture_area_options(const GsrInfo& info, bool include_focused_window) {
            // 捕获能力在一个会话内是稳定的，算一次缓存起来即可（避免每帧查询显示器）
            static SupportedCaptureOptions cached;
            static bool cached_ready = false;
            if(!cached_ready) {
                cached = get_supported_capture_options(info);
                cached_ready = true;
            }
            const SupportedCaptureOptions& co = cached;

            std::vector<AreaOpt> opts;
            if(co.window)
                opts.push_back({ "window", "Window", true });
            if(include_focused_window && co.focused)
                opts.push_back({ "focused", "Follow focused window", true });
            if(co.region)
                opts.push_back({ "region", "Region", true });
            if(!co.monitors.empty())
                opts.push_back({ "focused_monitor", "Focused monitor", true });
            for(const GsrMonitor& monitor : co.monitors)
                opts.push_back({ monitor.name, monitor.name, false });  // 显示器名不翻译
            if(co.portal)
                opts.push_back({ "portal", "Desktop portal", true });
            return opts;
        }

        bool combo_opts(const char* label, const std::vector<AreaOpt>& opts, std::string& current) {
            ImGui::PushID(label);
            const float s = ui_scale();
            bool changed = false;

            const char* cur_label = current.c_str();
            for(const AreaOpt& o : opts) {
                if(current == o.id) { cur_label = o.translate ? TR(o.label.c_str()) : o.label.c_str(); break; }
            }

            const float cw = std::min(280.0f * s, ImGui::GetContentRegionAvail().x * 0.5f);
            ImGui::TextUnformatted(TR(label));
            ImGui::SameLine(row_right_x(cw));
            ImGui::SetNextItemWidth(cw);
            if(ImGui::BeginCombo("##c", cur_label)) {
                for(const AreaOpt& o : opts) {
                    const bool sel = (current == o.id);
                    if(ImGui::Selectable(o.translate ? TR(o.label.c_str()) : o.label.c_str(), sel)) {
                        current = o.id;
                        changed = true;
                    }
                    if(sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
            return changed;
        }

        // 录制/回放/直播共用的 RecordOptions 编辑器（改动即 save_config）
        void record_options_editor(Config& config, RecordOptions& opt, const GsrInfo& info) {
            bool changed = false;

            if(combo_opts("Capture area", capture_area_options(info, true), opt.record_area_option)) changed = true;

            // 分辨率：预设 + 自定义（自定义宽高收进同一弹层，避免裸露两个 InputInt）
            static const char* res_presets[] = { "Native", "3840x2160", "2560x1440", "1920x1080", "1280x720" };
            int preset_idx = (opt.video_width == 0 && opt.video_height == 0) ? 0 : -1;
            if(preset_idx < 0) {
                for(int i = 1; i < 5; ++i) {
                    int w = 0, h = 0; sscanf(res_presets[i], "%dx%d", &w, &h);
                    if(w == opt.video_width && h == opt.video_height) { preset_idx = i; break; }
                }
            }
            {
                ImGui::PushID("resolution");
                const float s = ui_scale();
                char cur_label[64];
                if(preset_idx >= 0)
                    snprintf(cur_label, sizeof(cur_label), "%s", res_presets[preset_idx]);
                else
                    snprintf(cur_label, sizeof(cur_label), "%d x %d", opt.video_width, opt.video_height);
                const float cw = std::min(280.0f * s, ImGui::GetContentRegionAvail().x * 0.5f);
                ImGui::TextUnformatted(TR("Resolution"));
                ImGui::SameLine(row_right_x(cw));
                ImGui::SetNextItemWidth(cw);
                if(ImGui::BeginCombo("##res", cur_label)) {
                    for(int i = 0; i < 5; ++i) {
                        bool sel = (i == preset_idx);
                        if(ImGui::Selectable(res_presets[i], sel)) {
                            if(i == 0) { opt.video_width = 0; opt.video_height = 0; }
                            else { int w = 0, h = 0; sscanf(res_presets[i], "%dx%d", &w, &h); opt.video_width = w; opt.video_height = h; }
                            changed = true;
                        }
                        if(sel) ImGui::SetItemDefaultFocus();
                    }
                    // 自定义分辨率
                    ImGui::Separator();
                    int w = opt.video_width, h = opt.video_height;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
                    if(ImGui::InputInt("##rw", &w)) { opt.video_width = std::max(0, w); changed = true; }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if(ImGui::InputInt("##rh", &h)) { opt.video_height = std::max(0, h); changed = true; }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }

            static const char* fps_ids[] = { "24", "30", "60", "120", "144", "165", "240" };
            if(combo_int("Framerate (FPS)", fps_ids, 7, opt.fps)) changed = true;
            if(input_int_row("Bitrate (kbps)", &opt.video_bitrate)) changed = true;

            auto codecs = video_codec_options(info);
            std::vector<const char*> cids; std::vector<const char*> clabels;
            for(auto& p : codecs) { cids.push_back(p.first.c_str()); clabels.push_back(p.second.c_str()); }
            if(combo_row("Video codec", cids.data(), clabels.data(), (int)cids.size(), opt.video_codec)) changed = true;

            static const char* ac_ids[]   = { "opus", "aac", "ac3" };
            static const char* ac_labels[]= { "Opus", "AAC", "AC3" };
            if(combo_row("Audio codec", ac_ids, ac_labels, 3, opt.audio_codec)) changed = true;

            if(toggle_row("Record cursor", &opt.record_cursor)) changed = true;

            if(changed) save_config(config);
        }

        // 目录选择：优先用 zenity 打开文件夹选择对话框，失败返回空串
        std::string browse_directory(const std::string& current) {
            std::string cmd = "zenity --file-selection --directory --title='Select folder'";
            if(!current.empty())
                cmd += " --filename='" + current + "'";
            FILE* f = popen(cmd.c_str(), "r");
            if(!f) return "";
            char buf[4096];
            std::string out;
            while(fgets(buf, sizeof(buf), f)) out += buf;
            pclose(f);
            while(!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            return out;
        }

        // 保存目录行：小标签 + 全宽输入 + 浏览按钮
        void directory_row(Config& config, const char* id, std::string& value) {
            ImGui::PushID(id);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextSecond);
            ImGui::TextUnformatted(TR("Save directory"));
            ImGui::PopStyleColor();
            const float s = ui_scale();
            const float bw = std::max(1.0f, ImGui::CalcTextSize(TR("Browse")).x + 40.0f * s);
            ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - bw - 10.0f * s));
            if(input_text_cfg("##dir", id, value)) save_config(config);
            ImGui::SameLine();
            if(ImGui::Button(TR("Browse"))) {
                std::string p = browse_directory(value);
                if(!p.empty()) { value = p; save_config(config); }
            }
            ImGui::PopID();
        }
    }

    // ================= 各页面 =================

    static void draw_global_page(Config& config, GsrInfo& gsr_info, const SettingsCallbacks& callbacks,
                                 ConfigHotkey*& capture_target) {
        (void)gsr_info;

        // ---- 常规 ----
        section_title("General");
        {
            const CardSpan card = begin_card();

            ImGui::TextUnformatted(TR("Notification speed"));
            {
                static const char* const labels[] = { "Normal", "Fast" };
                static const char* const ids[] = { "normal", "fast" };
                static int selected = 0;
                std::string sel = ids[selected];
                if(segmented_row("notif_speed", labels, ids, 2, sel, &sel)) {
                    selected = (sel == "fast") ? 1 : 0;
                    if(callbacks.on_notification_speed)
                        callbacks.on_notification_speed(selected);
                }
            }

            // 语言
            ImGui::Spacing();
            ImGui::PushID("language");
            {
                static int selected = -1;
                if(selected < 0) {
                    selected = 0;
                    for(int i = 0; i < (int)(sizeof(language_options) / sizeof(language_options[0])); ++i) {
                        if(config.main_config.language == language_options[i].id)
                            selected = i;
                    }
                }
                const char* current_label = language_options[selected].label;
                if(strcmp(current_label, "System language") == 0)
                    current_label = TR("System language");
                const float s = ui_scale();
                const float cw = std::min(220.0f * s, ImGui::GetContentRegionAvail().x * 0.5f);
                ImGui::TextUnformatted(TR("Language"));
                ImGui::SameLine(row_right_x(cw));
                ImGui::SetNextItemWidth(cw);
                if(ImGui::BeginCombo("##language", current_label)) {
                    for(int i = 0; i < (int)(sizeof(language_options) / sizeof(language_options[0])); ++i) {
                        const char* label = language_options[i].label;
                        if(strcmp(label, "System language") == 0)
                            label = TR("System language");
                        const bool is_selected = selected == i;
                        if(ImGui::Selectable(label, is_selected)) {
                            selected = i;
                            Translation::instance().load_language(language_options[i].id);
                            config.main_config.language = language_options[i].id;
                            save_config(config);
                            if(callbacks.on_language_changed)
                                callbacks.on_language_changed(0);
                        }
                        if(is_selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::PopID();
            end_card(card);
        }

        // ---- 外观 ----
        section_title("Appearance");
        {
            const CardSpan card = begin_card();
            ImGui::TextUnformatted(TR("Accent color"));
            {
                static const char* const labels[] = { "Red", "Green", "Blue" };
                static const char* const ids[] = { accent_options[0].id, accent_options[1].id, accent_options[2].id };
                std::string sel = config.main_config.tint_color;
                if(segmented_row("accent", labels, ids, 3, sel, &sel)) {
                    config.main_config.tint_color = sel;
                    for(const AccentOption& option : accent_options) {
                        if(sel == option.id)
                            get_color_theme().tint_color = option.color;
                    }
                    save_config(config);
                    if(callbacks.on_config_changed)
                        callbacks.on_config_changed();
                }
            }
            end_card(card);
        }

        // ---- 启动 ----
        section_title("Startup");
        {
            const CardSpan card = begin_card();
            static const char* const labels[] = { "Yes", "No" };
            static const char* const ids[] = { "start_on_system_startup", "dont_start_on_system_startup" };
            static int selected = -1;
            if(selected < 0) {
                const char* home = getenv("HOME");
                std::string path = home ? home : "";
                path += "/.config/autostart/gpu-screen-recorder-ui.desktop";
                FILE *f = fopen(path.c_str(), "rb");
                if(f) { fclose(f); selected = 0; } else { selected = 1; }
            }
            std::string sel = ids[selected];
            if(segmented_row("startup", labels, ids, 2, sel, &sel)) {
                selected = (sel == ids[0]) ? 0 : 1;
                const bool enable = selected == 0;
                const int exit_status = set_xdg_autostart(enable);
                if(callbacks.on_startup_changed)
                    callbacks.on_startup_changed(enable, exit_status);
            }
            end_card(card);
        }

        // ---- 快捷键（逐项捕获）----
        section_title("Keyboard hotkeys");
        {
            const CardSpan card = begin_card();
            struct HotkeyRow { const char* label_key; ConfigHotkey* hotkey; };
            const HotkeyRow rows[] = {
                { "Show/hide UI", &config.main_config.show_hide_hotkey },
                { "Turn replay on/off", &config.replay_config.start_stop_hotkey },
                { "Save replay", &config.replay_config.save_hotkey },
                { "Save 1 minute replay", &config.replay_config.save_1_min_hotkey },
                { "Save 10 minute replay", &config.replay_config.save_10_min_hotkey },
                { "Start/stop recording", &config.record_config.start_stop_hotkey },
                { "Pause/unpause recording", &config.record_config.pause_unpause_hotkey },
                { "Start/stop recording a region", &config.record_config.start_stop_region_hotkey },
                { "Start/stop streaming", &config.streaming_config.start_stop_hotkey },
                { "Take a screenshot", &config.screenshot_config.take_screenshot_hotkey },
            };

            const float s = ui_scale();
            for(const HotkeyRow& row : rows) {
                const bool capturing = (capture_target == row.hotkey);
                const float bw = std::min(220.0f * s, ImGui::GetContentRegionAvail().x * 0.45f);
                ImGui::TextUnformatted(TR(row.label_key));
                ImGui::SameLine(row_right_x(bw));
                // PushID 用指针保证每行唯一：不同行的快捷键文本可能相同（比如都是未设置）
                ImGui::PushID(row.hotkey);
                ImGui::PushStyleColor(ImGuiCol_Text, accent_u32());
                if(capturing) {
                    ImGui::TextUnformatted("按下组合键… (Esc 取消)");
                } else {
                    const std::string hotkey_str = row.hotkey->to_string(false, false);
                    ImGui::SetNextItemWidth(bw);
                    if(ImGui::Button(hotkey_str.c_str(), ImVec2(bw, 0))) {
                        capture_target = row.hotkey;
                        auto& cap = hotkey_capture();
                        cap.active = true; cap.cancelled = false; cap.key = 0; cap.modifiers = 0;
                    }
                }
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            end_card(card);

            ImGui::Spacing();
            if(ImGui::Button(TR("Reset hotkeys to default"))) {
                config.set_hotkeys_to_default();
                save_config(config);
                if(callbacks.on_config_changed)
                    callbacks.on_config_changed();
            }
        }

        // ---- 退出 ----
        {
            const CardSpan card = begin_card();
            if(ImGui::Button(TR("Exit program"))) {
                if(callbacks.on_exit_program)
                    callbacks.on_exit_program();
            }
            end_card(card);
        }

        // ---- 应用信息 ----
        section_title("Application info");
        {
            const CardSpan card = begin_card();
            ImGui::PushStyleColor(ImGuiCol_Text, kTextSecond);
            char str[256];
            snprintf(str, sizeof(str), "gsr-imgui 版本: %s", kImGuiUiVersion);
            ImGui::TextUnformatted(str);
            snprintf(str, sizeof(str), "%s: %s", TR("GPU vendor"), gpu_vendor_to_string(gsr_info.gpu_info.vendor));
            ImGui::TextUnformatted(str);
            ImGui::PopStyleColor();
            end_card(card);
        }
    }

    static void draw_record_page(Config& config, GsrInfo& gsr_info, const SettingsCallbacks& callbacks) {
        (void)callbacks;
        section_title("Record");
        {
            const CardSpan card = begin_card();
            record_options_editor(config, config.record_config.record_options, gsr_info);
            static const char* cont_ids[]   = { "mp4", "mkv", "webm", "flv", "mov" };
            static const char* cont_labels[]= { "MP4", "MKV", "WebM", "FLV", "MOV" };
            combo_row("Container", cont_ids, cont_labels, 5, config.record_config.container);
            end_card(card);
        }
        section_title("File");
        {
            const CardSpan card = begin_card();
            if(toggle_row("Save video in game folder", &config.record_config.save_video_in_game_folder)) save_config(config);
            if(toggle_row("Name video file after game", &config.record_config.name_video_file_after_game)) save_config(config);
            directory_row(config, "rec_dir", config.record_config.save_directory);
            end_card(card);
        }
    }

    static void draw_replay_page(Config& config, GsrInfo& gsr_info, const SettingsCallbacks& callbacks) {
        (void)callbacks;
        section_title("Replay");
        {
            const CardSpan card = begin_card();
            record_options_editor(config, config.replay_config.record_options, gsr_info);
            end_card(card);
        }
        section_title("Options");
        {
            const CardSpan card = begin_card();
            static const char* mode_ids[]   = { "dont_turn_on_automatically", "turn_on_at_system_startup", "turn_on_at_game_launch" };
            static const char* mode_labels[]= { "Don't turn on", "At system startup", "At game launch" };
            combo_row("Turn on replay", mode_ids, mode_labels, 3, config.replay_config.turn_on_replay_automatically_mode);
            if(input_int_row("Replay length (seconds)", &config.replay_config.replay_time)) save_config(config);
            static const char* stor_ids[]   = { "ram", "disk" };
            static const char* stor_labels[]= { "RAM", "Disk" };
            combo_row("Replay storage", stor_ids, stor_labels, 2, config.replay_config.replay_storage);
            static const char* cont_ids[]   = { "mp4", "mkv", "webm", "flv", "mov" };
            static const char* cont_labels[]= { "MP4", "MKV", "WebM", "FLV", "MOV" };
            combo_row("Container", cont_ids, cont_labels, 5, config.replay_config.container);
            end_card(card);
        }
        section_title("File");
        {
            const CardSpan card = begin_card();
            if(toggle_row("Save video in game folder", &config.replay_config.save_video_in_game_folder)) save_config(config);
            if(toggle_row("Name video file after game", &config.replay_config.name_video_file_after_game)) save_config(config);
            directory_row(config, "rep_dir", config.replay_config.save_directory);
            end_card(card);
        }
    }

    static void draw_stream_page(Config& config, GsrInfo& gsr_info, const SettingsCallbacks& callbacks) {
        (void)callbacks;
        section_title("Stream");
        {
            const CardSpan card = begin_card();
            record_options_editor(config, config.streaming_config.record_options, gsr_info);
            end_card(card);
        }
        section_title("Streaming service");
        {
            const CardSpan card = begin_card();
            static const char* svc_ids[]   = { "twitch", "youtube", "rumble", "kick", "custom", "whip" };
            static const char* svc_labels[]= { "Twitch", "YouTube", "Rumble", "Kick", "Custom", "WHIP" };
            combo_row("Service", svc_ids, svc_labels, 6, config.streaming_config.streaming_service);
            end_card(card);

            const std::string& svc = config.streaming_config.streaming_service;
            const char* key_id = nullptr;
            std::string* key_value = nullptr;
            bool secret = true;
            const char* key_label = "Stream key";
            if(svc == "twitch")      { key_id = "tw_key";  key_value = &config.streaming_config.twitch.stream_key; }
            else if(svc == "youtube"){ key_id = "yt_key";  key_value = &config.streaming_config.youtube.stream_key; }
            else if(svc == "rumble") { key_id = "ru_key";  key_value = &config.streaming_config.rumble.stream_key; }
            else if(svc == "kick")   { key_id = "kick_key"; key_value = &config.streaming_config.kick.stream_key; }
            else if(svc == "custom") { key_id = "cust_key"; key_value = &config.streaming_config.custom.key; }
            else if(svc == "whip")   { key_id = "whip_tok"; key_value = &config.streaming_config.whip.bearer_token; key_label = "Bearer token"; }

            if(key_id && key_value) {
                const CardSpan card2 = begin_card();
                ImGui::PushStyleColor(ImGuiCol_Text, kTextSecond);
                ImGui::TextUnformatted(TR(key_label));
                ImGui::PopStyleColor();
                input_text_cfg("##key", key_id, *key_value, secret);
                end_card(card2);
            }

            // kick/custom/whip 额外的 URL 字段
            const char* url_id = nullptr;
            std::string* url_value = nullptr;
            const char* url_label = "URL";
            if(svc == "kick")        { url_id = "kick_url"; url_value = &config.streaming_config.kick.stream_url; url_label = "Stream URL"; }
            else if(svc == "custom") { url_id = "cust_url"; url_value = &config.streaming_config.custom.url; }
            else if(svc == "whip")   { url_id = "whip_url"; url_value = &config.streaming_config.whip.url; }
            if(url_id && url_value) {
                const CardSpan card3 = begin_card();
                ImGui::PushStyleColor(ImGuiCol_Text, kTextSecond);
                ImGui::TextUnformatted(TR(url_label));
                ImGui::PopStyleColor();
                input_text_cfg("##url", url_id, *url_value);
                end_card(card3);
            }
        }
    }

    static void draw_screenshot_page(Config& config, GsrInfo& gsr_info, const SettingsCallbacks& callbacks) {
        (void)callbacks;
        section_title("Screenshot");
        {
            const CardSpan card = begin_card();
            bool changed = false;
            // 截图页不提供 "focused"（跟随焦点窗口），与旧 UI 的 ScreenshotSettingsPage 一致
            changed |= combo_opts("Capture area", capture_area_options(gsr_info, false), config.screenshot_config.record_area_option);
            // 注意：翻译文件是 "key=value" 格式，按第一个 '=' 切分，
            // 所以 key 里不能出现 '='（否则永远翻译不到）。这里用 ':' 代替。
            if(input_int_row("Width (0: native)", &config.screenshot_config.image_width)) changed = true;
            if(input_int_row("Height (0: native)", &config.screenshot_config.image_height)) changed = true;
            static const char* q_ids[]   = { "very_high", "high", "medium", "low" };
            static const char* q_labels[]= { "Very high", "High", "Medium", "Low" };
            changed |= combo_row("Image quality", q_ids, q_labels, 4, config.screenshot_config.image_quality);
            static const char* f_ids[]   = { "jpg", "png", "bmp", "webp", "tga" };
            static const char* f_labels[]= { "JPG", "PNG", "BMP", "WebP", "TGA" };
            changed |= combo_row("Image format", f_ids, f_labels, 5, config.screenshot_config.image_format);
            if(changed) save_config(config);
            end_card(card);
        }
        section_title("Save");
        {
            const CardSpan card = begin_card();
            if(toggle_row("Save to disk", &config.screenshot_config.save_screenshot_to_disk)) save_config(config);
            if(toggle_row("Save to clipboard", &config.screenshot_config.save_screenshot_to_clipboard)) save_config(config);
            if(toggle_row("Save in game folder", &config.screenshot_config.save_screenshot_in_game_folder)) save_config(config);
            if(toggle_row("Name after game", &config.screenshot_config.name_screenshot_file_after_game)) save_config(config);
            end_card(card);
        }
        {
            directory_row(config, "ss_dir", config.screenshot_config.save_directory);
        }
    }

    bool SettingsUi::draw(Config& config, GsrInfo& gsr_info, const SettingsCallbacks& callbacks) {
        if(!open_)
            return false;

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        // 注意：面板是不透明控件，必须画到「背景」绘制列表（在所有窗口之下），
        // 否则它会盖住后面用普通 ImGui 控件（Text/Button 等）渲染的文字和按钮。
        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

        const float scale = std::min(std::max(display.y / 1080.0f, 0.75f), 2.0f);
        const float margin = display.y * 0.05f;
        // 面板不再铺满整屏：水平方向限宽（1180 逻辑宽度）并居中，四周留出暗边
        const float panel_w = std::min(display.x - margin * 2.0f, 1180.0f * scale);
        const ImVec2 panel_pos((display.x - panel_w) * 0.5f, margin);
        const ImVec2 panel_size(panel_w, display.y - margin * 2.0f);

        // ---- 现代化全局样式（圆角、内边距、控件配色）----
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f * scale, 8.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 10.0f * scale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(255, 255, 255, 16));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 26));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  IM_COL32(255, 255, 255, 36));
        ImGui::PushStyleColor(ImGuiCol_PopupBg,        IM_COL32(24, 26, 29, 252));
        ImGui::PushStyleColor(ImGuiCol_Header,         accent_u32());
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  accent_u32());
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,   accent_u32());
        ImGui::PushStyleColor(ImGuiCol_Button,         IM_COL32(255, 255, 255, 22));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  IM_COL32(255, 255, 255, 34));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   accent_u32());
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,    IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,  IM_COL32(255, 255, 255, 40));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(255, 255, 255, 60));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  IM_COL32(255, 255, 255, 80));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled,   IM_COL32(255, 255, 255, 110));
        ImGui::PushStyleColor(ImGuiCol_Separator,      IM_COL32(255, 255, 255, 20));

        const ImGuiWindowFlags window_flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoBackground;

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(display);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##gsr-settings", nullptr, window_flags);

        // 整屏暗化 + 圆角面板（用户反馈：不需要那么高的透明度 → 背景更实）
        draw_list->AddRectFilled(ImVec2(0.0f, 0.0f), display, IM_COL32(0, 0, 0, 185));
        const float rounding = 14.0f * scale;
        draw_list->AddRectFilled(panel_pos, ImVec2(panel_pos.x + panel_size.x, panel_pos.y + panel_size.y),
            IM_COL32(26, 26, 26, 255), rounding);
        draw_list->AddRect(panel_pos, ImVec2(panel_pos.x + panel_size.x, panel_pos.y + panel_size.y),
            IM_COL32(255, 255, 255, 30), rounding, ImDrawFlags_None, 1.0f * scale);

        // 标题栏：返回按钮 + 居中标题；ESC 也可返回主页
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36.0f * scale, 26.0f * scale));
        ImGui::SetCursorScreenPos(panel_pos);
        ImGui::BeginChild("##settings-root", panel_size, 0);

        ImGui::PushFont(ui_font(FontRole::Body));
        if(ImGui::Button(TR("Back")))
            open_ = false;
        if(ImGui::IsKeyPressed(ImGuiKey_Escape))
            open_ = false;

        ImGui::SameLine();
        ImGui::PushFont(ui_font(FontRole::Title));
        const ImVec2 title_size = ImGui::CalcTextSize(TR("Settings"));
        ImGui::SetCursorScreenPos(ImVec2(panel_pos.x + (panel_size.x - title_size.x) * 0.5f,
            ImGui::GetCursorScreenPos().y));
        ImGui::TextUnformatted(TR("Settings"));
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, 10.0f * scale));

        // 左侧导航 + 右侧内容（可滚动）
        const float nav_w = 220.0f * scale;
        ImGui::BeginChild("##nav", ImVec2(nav_w, 0.0f), false, ImGuiWindowFlags_NoSavedSettings);
        {
            struct NavItem { Page page; const char* key; };
            const NavItem nav_items[] = {
                { Page::Global, "General" },
                { Page::Record, "Record" },
                { Page::Replay, "Replay" },
                { Page::Stream, "Stream" },
                { Page::Screenshot, "Screenshot" },
            };
            for(const NavItem& it : nav_items) {
                const bool sel = (page_ == it.page);
                if(sel)
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
                else
                    ImGui::PushStyleColor(ImGuiCol_Text, kTextSecond);
                // 选中的导航项用强调色圆角胶囊（Header 配色已全局指向强调色）
                if(ImGui::Selectable(TR(it.key), sel, ImGuiSelectableFlags_SpanAllColumns))
                    {
                        page_ = it.page;
                        capture_target_ = nullptr;
                        hotkey_capture().active = false;
                    }
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f * scale, 22.0f * scale));
        ImGui::BeginChild("##content", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoSavedSettings);

        // 卡片双通道：0 号通道画卡片底色，1 号通道是控件
        card_channels_begin();

        // 热键捕获完成（全局页的快捷键在此处理）
        if(capture_target_ != nullptr) {
            auto& cap = hotkey_capture();
            if(!cap.active) {
                if(!cap.cancelled && cap.key != 0) {
                    capture_target_->key = cap.key;
                    capture_target_->modifiers = cap.modifiers;
                    save_config(config);
                    if(callbacks.on_config_changed)
                        callbacks.on_config_changed();
                }
                capture_target_ = nullptr;
            }
        }

        switch(page_) {
            case Page::Global:    draw_global_page(config, gsr_info, callbacks, capture_target_); break;
            case Page::Record:    draw_record_page(config, gsr_info, callbacks); break;
            case Page::Replay:    draw_replay_page(config, gsr_info, callbacks); break;
            case Page::Stream:    draw_stream_page(config, gsr_info, callbacks); break;
            case Page::Screenshot: draw_screenshot_page(config, gsr_info, callbacks); break;
        }

        card_channels_end();
        ImGui::EndChild();      // ##content
        ImGui::PopStyleVar();   // content WindowPadding
        ImGui::PopFont();       // Body（全局）
        ImGui::EndChild();      // ##settings-root
        ImGui::PopStyleVar();   // root WindowPadding
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(16);   // 必须与上面 16 个 PushStyleColor 一一对应（少 pop 会触发 "Missing PopStyleColor" 断言）
        ImGui::PopStyleVar(5);
        return true;
    }

}
}
