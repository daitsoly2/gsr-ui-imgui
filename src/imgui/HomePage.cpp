#include "../../include/imgui/HomePage.hpp"
#include "../../include/imgui/Host.hpp"
#include "../../include/Translation.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <string>
#include <vector>

namespace gsr {
namespace imgui_ui {

    namespace {
        // ---------- 颜色工具 ----------

        ImU32 lerp_color(ImU32 from, ImU32 to, float t) {
            const ImVec4 a = ImGui::ColorConvertU32ToFloat4(from);
            const ImVec4 b = ImGui::ColorConvertU32ToFloat4(to);
            const ImVec4 r(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
            return ImGui::ColorConvertFloat4ToU32(r);
        }

        ImU32 with_alpha(ImU32 color, float alpha) {
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
            c.w = alpha;
            return ImGui::ColorConvertFloat4ToU32(c);
        }

        // 强调色：apply_theme() 把 ImGuiCol_ButtonActive 设成了 tint_color
        ImU32 accent_u32() {
            return ImGui::GetColorU32(ImGuiCol_ButtonActive);
        }

        // ---------- 动画 ----------
        // 用 ImGui 自带的状态存储按 id 保存插值进度，避免引入全局静态状态
        float animate_towards(const char* key, bool target, float speed) {
            float* value = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID(key), target ? 1.0f : 0.0f);
            const float dt = ImGui::GetIO().DeltaTime;
            const float goal = target ? 1.0f : 0.0f;
            const float step = ImMin(1.0f, dt * speed);
            *value += (goal - *value) * step;
            if(std::fabs(*value - goal) < 0.002f)
                *value = goal;
            return *value;
        }

        // ---------- 矢量图标（不依赖图标字体，任意 DPI 都清晰）----------
        enum class IconKind {
            RECORD,
            REPLAY,
            STREAM,
            SCREENSHOT,
            SETTINGS,
        };

        void draw_icon(ImDrawList* draw_list, IconKind kind, ImVec2 center, float size, ImU32 color) {
            const float half = size * 0.5f;
            const float thickness = ImMax(1.5f, size * 0.085f);

            switch(kind) {
                case IconKind::RECORD: {
                    draw_list->AddCircle(center, half - thickness * 0.5f, color, 0, thickness);
                    draw_list->AddCircleFilled(center, half * 0.52f, color);
                    break;
                }
                case IconKind::REPLAY: {
                    // 逆时针圆弧 + 箭头
                    draw_list->PathArcTo(center, half - thickness * 0.5f, IM_PI * 0.35f, IM_PI * 1.92f, 28);
                    draw_list->PathStroke(color, ImDrawFlags_None, thickness);

                    const ImVec2 tip(center.x + std::cos(IM_PI * 0.35f) * (half - thickness * 0.5f),
                        center.y + std::sin(IM_PI * 0.35f) * (half - thickness * 0.5f));
                    draw_list->AddTriangleFilled(
                        ImVec2(tip.x - half * 0.34f, tip.y - half * 0.05f),
                        ImVec2(tip.x + half * 0.30f, tip.y - half * 0.36f),
                        ImVec2(tip.x + half * 0.14f, tip.y + half * 0.34f),
                        color);
                    break;
                }
                case IconKind::STREAM: {
                    // 同心弧线，表示推流/信号
                    for(int i = 0; i < 3; ++i) {
                        const float radius = half * (0.30f + 0.26f * (float)i);
                        ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
                        c.w *= 1.0f - 0.22f * (float)i;
                        draw_list->PathArcTo(ImVec2(center.x, center.y + half * 0.42f), radius,
                            IM_PI * 1.18f, IM_PI * 1.82f, 20);
                        draw_list->PathStroke(ImGui::ColorConvertFloat4ToU32(c), ImDrawFlags_None, thickness);
                    }
                    draw_list->AddCircleFilled(ImVec2(center.x, center.y + half * 0.42f), thickness * 0.75f, color);
                    break;
                }
                case IconKind::SCREENSHOT: {
                    const ImVec2 a(center.x - half, center.y - half * 0.74f);
                    const ImVec2 b(center.x + half, center.y + half * 0.74f);
                    draw_list->AddRect(a, b, color, size * 0.14f, ImDrawFlags_None, thickness);
                    draw_list->AddCircle(ImVec2(center.x, center.y), half * 0.34f, color, 0, thickness);
                    break;
                }
                case IconKind::SETTINGS: {
                    // 齿轮：8 根齿 + 外圈
                    const float r = half * 0.72f;
                    for(int i = 0; i < 8; ++i) {
                        const float angle = (float)i * (IM_PI * 0.25f);
                        draw_list->AddLine(
                            ImVec2(center.x + std::cos(angle) * r * 0.85f, center.y + std::sin(angle) * r * 0.85f),
                            ImVec2(center.x + std::cos(angle) * r * 1.35f, center.y + std::sin(angle) * r * 1.35f),
                            color, thickness * 1.1f);
                    }
                    draw_list->AddCircle(center, r * 0.9f, color, 0, thickness);
                    break;
                }
            }
        }

        // ---------- 快捷键按键帽 ----------
        ImVec2 measure_keycap(const std::string& text, float scale) {
            if(text.empty())
                return ImVec2(0.0f, 0.0f);

            ImGui::PushFont(ui_font(FontRole::Small));
            const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
            ImGui::PopFont();

            return ImVec2(text_size.x + 12.0f * scale, text_size.y + 6.0f * scale);
        }

        void draw_keycap(ImDrawList* draw_list, ImVec2 pos, const std::string& text,
            float scale, float max_width) {
            if(text.empty())
                return;

            const ImVec2 keycap_size = measure_keycap(text, scale);
            const float chip_w = ImMin(keycap_size.x, max_width);
            const float chip_h = keycap_size.y;

            draw_list->AddRectFilled(pos, ImVec2(pos.x + chip_w, pos.y + chip_h),
                IM_COL32(255, 255, 255, 18), 4.0f * scale);
            draw_list->AddRect(pos, ImVec2(pos.x + chip_w, pos.y + chip_h),
                IM_COL32(255, 255, 255, 26), 4.0f * scale, ImDrawFlags_None, 1.0f);

            ImGui::PushFont(ui_font(FontRole::Small));
            draw_list->AddText(ImVec2(pos.x + 6.0f * scale, pos.y + 3.0f * scale),
                IM_COL32(255, 255, 255, 150), text.c_str());
            ImGui::PopFont();
        }

        // 计算文本尺寸（用指定档位的字体）
        ImVec2 measure_text(const char* text, FontRole role) {
            if(!text || !text[0])
                return ImVec2(0.0f, 0.0f);
            ImGui::PushFont(ui_font(role));
            const ImVec2 size = ImGui::CalcTextSize(text);
            ImGui::PopFont();
            return size;
        }

        // ---------- 卡片 ----------

        struct CardSpec {
            const char* id;                     // ImGui 内部 id（不翻译）
            const char* title_key;              // 英文原文，用 TR() 翻译
            IconKind icon = IconKind::RECORD;
            std::string status;                 // 已翻译的当前状态
            bool active = false;
            const char* action_key = nullptr;   // 主操作按钮文案（英文原文，用 TR() 翻译）
            std::function<void()> on_action;    // 主操作（整卡可点）
            std::function<void()> on_settings;  // 右上角齿轮：该模块的独立设置页
            std::vector<std::string> hints;     // 快捷键提示
        };

        void draw_action_card(const CardSpec& card, ImVec2 position, ImVec2 size, float scale) {
            // 统一用前景绘制列表：整屏暗化与卡片都提交到同一列表，靠提交顺序保证层级
            ImDrawList* draw_list = ImGui::GetForegroundDrawList();
            const ImU32 accent = accent_u32();

            // 注意：整个卡片（含动画）都在同一个 PushID 作用域内，
            // 否则 "hover"/"active"/"press" 这些动画状态会在卡片之间互相串。
            ImGui::PushID(card.id);
            ImGui::SetCursorScreenPos(position);
            const bool clicked = ImGui::InvisibleButton("##hit", size);
            const bool card_held = ImGui::IsItemActive();

            // 悬停视觉：矩形判定（仅用于外观，点击已由上面的真实控件处理）
            const bool mouse_in_card = ImGui::IsWindowHovered()
                && ImGui::IsMouseHoveringRect(position, ImVec2(position.x + size.x, position.y + size.y));
            const bool hovered = mouse_in_card;

            const float hover_t = animate_towards("hover", hovered, 14.0f);
            const float active_t = animate_towards("active", card.active, 8.0f);
            const float press_t = animate_towards("press", card_held, 22.0f);

            const float lift = (3.0f * scale) * hover_t;         // 悬停时轻微上浮
            const float shrink = (1.5f * scale) * press_t;       // 按下时轻微收缩
            const ImVec2 a(position.x, position.y - lift + shrink);
            const ImVec2 b(position.x + size.x, position.y + size.y - lift - shrink);
            const float rounding = 14.0f * scale;
            const float pad = 16.0f * scale;

            // 阴影：多层半透明圆角矩形模拟（ImGui 没有 blur，这样成本最低）
            for(int i = 3; i >= 1; --i) {
                const float spread = (float)i * 3.0f * scale;
                draw_list->AddRectFilled(
                    ImVec2(a.x - spread * 0.4f, a.y + spread * 0.5f),
                    ImVec2(b.x + spread * 0.4f, b.y + spread),
                    IM_COL32(0, 0, 0, 16 + 8 * (3 - i)), rounding + spread * 0.5f);
            }

            // 底色：基础色 -> 悬停色 -> 激活时叠加强调色
            ImU32 fill = lerp_color(IM_COL32(38, 43, 47, 255), IM_COL32(48, 55, 61, 255), hover_t);
            if(active_t > 0.0f)
                fill = lerp_color(fill, with_alpha(accent, 0.22f * active_t), ImMin(1.0f, active_t + 0.35f));
            draw_list->AddRectFilled(a, b, fill, rounding);

            // 边框：悬停/激活时用强调色
            const ImU32 border = lerp_color(IM_COL32(255, 255, 255, 26), with_alpha(accent, 0.85f),
                ImMax(hover_t, active_t));
            draw_list->AddRect(a, b, border, rounding, ImDrawFlags_None, 1.0f * scale);

            // 内容：居中排版（图标在上，标题与状态居中，下面是操作与快捷键）
            const float center_x = (a.x + b.x) * 0.5f;
            const float content_left = a.x + pad;
            const float content_right = b.x - pad;
            float y = a.y + pad;

            const float icon_size = 40.0f * scale;

            // 只有图标+标题的卡片（例如"设置"）把内容垂直居中，避免下方留一大块空白
            const bool minimal = card.status.empty() && !card.action_key && card.hints.empty();
            if(minimal) {
                const ImVec2 title_only = measure_text(TR(card.title_key), FontRole::Body);
                const float block_height = icon_size + 14.0f * scale + title_only.y;
                y += ImMax(0.0f, ((size.y - pad * 2.0f) - block_height) * 0.45f);
            }

            draw_icon(draw_list, card.icon, ImVec2(center_x, y + icon_size * 0.5f), icon_size,
                lerp_color(IM_COL32(255, 255, 255, 120), accent, ImMax(hover_t, active_t)));
            y += icon_size + 14.0f * scale;

            // 标题
            ImGui::PushFont(ui_font(FontRole::Body));
            const ImVec2 title_size = ImGui::CalcTextSize(TR(card.title_key));
            draw_list->AddText(ImVec2(center_x - title_size.x * 0.5f, y), IM_COL32(255, 255, 255, 245),
                TR(card.title_key));
            ImGui::PopFont();
            y += title_size.y + 6.0f * scale;

            // 状态
            if(!card.status.empty()) {
                ImGui::PushFont(ui_font(FontRole::Small));
                const ImVec2 status_size = ImGui::CalcTextSize(card.status.c_str());
                draw_list->AddText(ImVec2(center_x - status_size.x * 0.5f, y),
                    card.active ? accent : IM_COL32(255, 255, 255, 150), card.status.c_str());
                ImGui::PopFont();
                y += status_size.y + 12.0f * scale;
            }

            // 主操作：药丸按钮（整卡可点，这里只是视觉提示）
            if(card.action_key) {
                const float button_h = 32.0f * scale;
                const ImVec2 ba(content_left, y);
                const ImVec2 bb(content_right, y + button_h);
                const float button_rounding = button_h * 0.5f;

                const float emphasis = ImMax(hover_t, active_t);
                draw_list->AddRectFilled(ba, bb, lerp_color(with_alpha(accent, 0.22f), accent, emphasis),
                    button_rounding);
                if(hover_t > 0.0f)
                    draw_list->AddRectFilled(ba, bb, IM_COL32(255, 255, 255, (int)(18.0f * hover_t)), button_rounding);

                ImGui::PushFont(ui_font(FontRole::Small));
                const ImVec2 label_size = ImGui::CalcTextSize(TR(card.action_key));
                draw_list->AddText(ImVec2(ba.x + (bb.x - ba.x - label_size.x) * 0.5f,
                    ba.y + (button_h - label_size.y) * 0.5f), IM_COL32(255, 255, 255, 250), TR(card.action_key));
                ImGui::PopFont();

                y += button_h + 10.0f * scale;
            }

            // 快捷键提示（按键帽样式，水平居中）
            for(const std::string& hint : card.hints) {
                if(hint.empty())
                    continue;
                const ImVec2 keycap_size = measure_keycap(hint, scale);
                if(keycap_size.x <= 0.0f)
                    break;
                draw_keycap(draw_list, ImVec2(center_x - keycap_size.x * 0.5f, y), hint,
                    scale, content_right - content_left);
                y += keycap_size.y + 4.0f * scale;
            }

            // 整卡点击 = 主操作（齿轮按钮已移除）
            if(clicked && card.on_action)
                card.on_action();

            ImGui::PopID();
        }
    }

    void draw_home_page(const HomeState& state, const HomeCallbacks& callbacks) {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const ImU32 accent = accent_u32();
        ImDrawList* draw_list = ImGui::GetForegroundDrawList();

        // 以 1080p 为基准的缩放（与旧 UI 按 window_height 比例缩放的做法一致）
        const float scale = ImClamp(display.y / 1080.0f, 0.75f, 2.0f);

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
        ImGui::Begin("##gsr-home", nullptr, window_flags);

        // ---- 整屏暗化（等价于旧 UI 的 bg_color(0,0,0,100)）----
        draw_list->AddRectFilled(ImVec2(0.0f, 0.0f), display, IM_COL32(0, 0, 0, 100));

        // ---- 一行卡片：即时回放 / 录制 / 直播 / 截图 / 设置 ----
        const int card_count = 5;
        const float card_size = ImMin(display.y / 5.0f, display.x / 6.6f);
        const float gap = 18.0f * scale;
        const float total_width = card_size * (float)card_count + gap * (float)(card_count - 1);
        const float row_x = (display.x - total_width) * 0.5f;
        const float row_y = (display.y - card_size) * 0.5f + 18.0f * scale;

        // ---- 顶部状态信息（居中，位于卡片行上方）----
        {
            const float center_x = display.x * 0.5f;
            float y = row_y - 76.0f * scale;

            ImGui::PushFont(ui_font(FontRole::Title));
            const ImVec2 title_size = ImGui::CalcTextSize("GPU Screen Recorder");
            draw_list->AddText(ImVec2(center_x - title_size.x * 0.5f, y), IM_COL32(255, 255, 255, 235),
                "GPU Screen Recorder");
            ImGui::PopFont();
            y += title_size.y + 6.0f * scale;

            std::string subtitle = state.record_status;
            if(!state.target.empty())
                subtitle += "  ·  " + state.target;

            ImGui::PushFont(ui_font(FontRole::Small));
            const ImVec2 subtitle_size = ImGui::CalcTextSize(subtitle.c_str());
            draw_list->AddText(ImVec2(center_x - subtitle_size.x * 0.5f, y), accent, subtitle.c_str());
            ImGui::PopFont();
        }

        std::vector<CardSpec> cards;
        {
            CardSpec card;
            card.id = "replay";
            card.on_settings = callbacks.replay_settings;
            card.title_key = "Instant Replay";
            card.icon = IconKind::REPLAY;
            card.status = state.replay_status;
            card.active = state.replay_active;
            card.action_key = state.replay_action.empty() ? "Turn on" : nullptr;
            card.on_action = callbacks.replay_toggle;
            if(!state.replay_hotkey.empty())
                card.hints.push_back(state.replay_hotkey);
            if(!state.replay_save_hotkey.empty())
                card.hints.push_back(state.replay_save_hotkey);
            cards.push_back(std::move(card));
        }
        {
            CardSpec card;
            card.id = "record";
            card.on_settings = callbacks.record_settings;
            card.title_key = "Record";
            card.icon = IconKind::RECORD;
            card.status = state.record_status;
            card.active = state.record_active;
            card.action_key = state.record_action.empty() ? "Start" : nullptr;
            card.on_action = callbacks.record_toggle;
            if(!state.record_hotkey.empty())
                card.hints.push_back(state.record_hotkey);
            cards.push_back(std::move(card));
        }
        {
            CardSpec card;
            card.id = "stream";
            card.on_settings = callbacks.stream_settings;
            card.title_key = "Livestream";
            card.icon = IconKind::STREAM;
            card.status = state.stream_status;
            card.active = state.stream_active;
            card.action_key = state.stream_action.empty() ? "Start" : nullptr;
            card.on_action = callbacks.stream_toggle;
            if(!state.stream_hotkey.empty())
                card.hints.push_back(state.stream_hotkey);
            cards.push_back(std::move(card));
        }
        {
            CardSpec card;
            card.id = "screenshot";
            card.on_settings = callbacks.screenshot_settings;
            card.title_key = "Screenshot";
            card.icon = IconKind::SCREENSHOT;
            card.action_key = "Screenshot";
            card.on_action = callbacks.screenshot_take;
            if(!state.screenshot_hotkey.empty())
                card.hints.push_back(state.screenshot_hotkey);
            cards.push_back(std::move(card));
        }
        {
            CardSpec card;
            card.id = "settings";
            card.title_key = "Settings";
            card.icon = IconKind::SETTINGS;
            card.on_action = callbacks.open_settings;
            cards.push_back(std::move(card));
        }

        for(size_t i = 0; i < cards.size(); ++i) {
            const float x = row_x + (float)i * (card_size + gap);
            draw_action_card(cards[i], ImVec2(x, row_y), ImVec2(card_size, card_size), scale);
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
    }

}
}
