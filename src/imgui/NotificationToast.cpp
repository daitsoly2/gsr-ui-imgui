#include "../../include/imgui/NotificationToast.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "../../include/imgui/Host.hpp"  // gsr::imgui_ui::ui_font / FontRole

namespace gsr {
namespace imgui_ui {

struct Toast {
    std::string text;
    double timeout = 5.0;
    std::chrono::steady_clock::time_point born;
    std::chrono::steady_clock::time_point expire;
    mgl::Color icon_color = mgl::Color(255, 255, 255);
    mgl::Color bg_color = mgl::Color(30, 30, 30);
    std::string icon_type;
};

static std::vector<Toast>& queue() {
    static std::vector<Toast> q;
    return q;
}

// 灵动岛的跨帧动画状态。draw_notifications() 与 has_pending_notifications() 共用，
// 这样覆盖层在队列清空后还能等胶囊收缩/淡出完成再销毁窗口。
struct IslandState {
    float cur_w = 0.0f;   // 胶囊当前宽
    float cur_h = 0.0f;   // 胶囊当前高
    float cur_a = 0.0f;   // 整体透明度 0..1
    Toast shown;          // 淡出阶段仍要绘制的内容
};

static IslandState& island() {
    static IslandState s;
    return s;
}

// mgl::Color(r,g,b,a) -> ImGui 0xAABBGGRR；extra_alpha 用于整体淡入淡出
static ImU32 to_imcolor(const mgl::Color& c, uint8_t extra_alpha) {
    const uint8_t a = (uint8_t)((uint32_t)c.a * extra_alpha / 255);
    return IM_COL32(c.r, c.g, c.b, a);
}

// 两个 ImU32 颜色按 t 插值（0..1），只处理 RGB，alpha 单独给
static ImU32 lerp_rgb(ImU32 a, ImU32 b, float t) {
    t = std::min(std::max(t, 0.0f), 1.0f);
    const int ar = (a >> 0) & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF;
    const int br = (b >> 0) & 0xFF, bg_ = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF;
    const int r = (int)(ar + (br - ar) * t);
    const int g = (int)(ag + (bg_ - ag) * t);
    const int bl = (int)(ab + (bb - ab) * t);
    return IM_COL32(r, g, bl, 255);
}

void push_notification(const std::string& text, double timeout_seconds,
                       mgl::Color icon_color, mgl::Color bg_color,
                       const char* icon_type) {
    auto& q = queue();

    Toast t;
    t.text = text;
    t.timeout = timeout_seconds > 0.0 ? timeout_seconds : 5.0;
    // 设一个下限：fast 模式会把时长乘 0.3，短通知（2s*0.3=0.6s）基本来不及看清
    if(t.timeout < 1.2)
        t.timeout = 1.2;
    t.born = std::chrono::steady_clock::now();
    t.expire = t.born + std::chrono::milliseconds((long)(t.timeout * 1000));
    t.icon_color = icon_color;
    t.bg_color = bg_color;
    t.icon_type = icon_type ? icon_type : "";

    q.push_back(std::move(t));

    // 最多保留 5 条，超出丢弃最旧的
    if(q.size() > 5)
        q.erase(q.begin(), q.begin() + (q.size() - 5));
}

void clear_notifications() {
    queue().clear();
}

/*
    灵动岛（Dynamic Island）式通知：

    - 顶部居中一颗胶囊（全圆角），平时完全隐藏；
    - 有通知时平滑「长大」并显示 图标 + 文本 + 超时进度条；
    - 结束时再平滑收缩消失。
    - 多条通知排队时显示最新一条，右上有 "+N" 角标提示还有几条在排队。

    动画用指数插值朝目标尺寸/透明度靠拢，每帧调用一次 draw_notifications()。
*/
void draw_notifications() {
    auto& q = queue();
    const auto now = std::chrono::steady_clock::now();

    // 过期清理
    q.erase(std::remove_if(q.begin(), q.end(), [&](const Toast& t) {
        return now >= t.expire;
    }), q.end());

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if(!dl)
        return;

    const float dt = std::min(std::max(io.DeltaTime, 1.0f / 240.0f), 0.1f);
    // 与 Host::load_font 一致的缩放。通知窗口是小窗（非全屏），不能用它自己的
    // DisplaySize 高度去估缩放，要用 Overlay 按显示器高度设定的 interface_scale()。
    const float scale = interface_scale() > 0.0f
        ? interface_scale()
        : std::min(std::max(io.DisplaySize.y / 1080.0f, 0.75f), 2.0f);
    ImFont* font = ui_font(FontRole::Body);
    // imgui 1.92 字体按字号烘焙：必须传与 AddFontFromFileTTF 一致的正文号（21*scale）
    const float body_size = 21.0f * scale;
    const float small_size = 15.0f * scale;

    // ---- 动画状态（跨帧保留）----
    IslandState& st = island();
    float& cur_w = st.cur_w;
    float& cur_h = st.cur_h;
    float& cur_a = st.cur_a;
    Toast& shown = st.shown;

    const bool has = !q.empty();
    if(has)
        shown = q.back();        // 淡出时用最后一次的内容

    // ---- 目标尺寸 ----
    float target_w = cur_w;
    float target_h = cur_h;
    if(has) {
        const float pad_x = 20.0f * scale;
        const float pad_y = 13.0f * scale;
        const float icon_d = 26.0f * scale;
        const float gap = 12.0f * scale;
        const float max_text_w = std::min(520.0f * scale, io.DisplaySize.x * 0.55f);
        const ImVec2 ts = font
            ? font->CalcTextSizeA(body_size, FLT_MAX, max_text_w, shown.text.c_str())
            : ImVec2(0.0f, body_size);
        const float text_w = std::min(ts.x, max_text_w);
        target_w = pad_x + icon_d + gap + text_w + pad_x;
        target_h = pad_y * 2.0f + std::max(ts.y, icon_d);
    }

    // ---- 指数插值（展开快、收起稍慢更有弹性）----
    const float ease = std::min(1.0f, dt * 12.0f);
    cur_w += (target_w - cur_w) * ease;
    cur_h += (target_h - cur_h) * ease;
    cur_a += ((has ? 1.0f : 0.0f) - cur_a) * std::min(1.0f, dt * (has ? 14.0f : 7.0f));

    if(cur_a <= 0.01f) {
        cur_a = 0.0f;
        return;                  // 完全隐藏：不提交任何绘制
    }
    if(cur_w < 8.0f * scale)
        cur_w = 8.0f * scale;
    if(cur_h < 8.0f * scale)
        cur_h = 8.0f * scale;

    const uint8_t A = (uint8_t)(255.0f * std::min(std::max(cur_a, 0.0f), 1.0f));
    const float rounding = cur_h * 0.5f;

    // 胶囊：顶部居中
    const float top_y = 16.0f * scale;
    const float x0 = (io.DisplaySize.x - cur_w) * 0.5f;
    const ImVec2 p0(x0, top_y);
    const ImVec2 p1(x0 + cur_w, top_y + cur_h);

    // 背景：近黑胶囊；错误类通知（bg_color 偏红等）稍微染色
    const ImU32 dark = IM_COL32(16, 17, 19, 255);
    const ImU32 tinted = to_imcolor(shown.bg_color, 255);
    const ImU32 bg = lerp_rgb(dark, tinted, 0.30f);
    dl->AddRectFilled(p0, p1, (bg & 0x00FFFFFF) | ((uint32_t)A << 24), rounding);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, (int)(26 * A / 255)), rounding, ImDrawFlags_None, 1.0f);

    if(!has)
        return;                  // 淡出阶段：只画空胶囊收缩

    const float pad_x = 20.0f * scale;
    const float icon_d = 26.0f * scale;
    const float gap = 12.0f * scale;
    const float max_text_w = std::min(520.0f * scale, io.DisplaySize.x * 0.55f);

    // ---- 图标（icon_color 圆 + 类型符号）----
    const float icon_cx = p0.x + pad_x + icon_d * 0.5f;
    const float icon_cy = p0.y + cur_h * 0.5f;
    const ImVec2 icon_c(icon_cx, icon_cy);
    dl->AddCircleFilled(icon_c, icon_d * 0.5f, to_imcolor(shown.icon_color, A));

    const float sym = icon_d * 0.26f;
    const ImU32 white = IM_COL32(255, 255, 255, A);
    const char* type = shown.icon_type.c_str();
    if(type[0] == 'r' && type[1] == 'e' && type[2] == 'c') {
        dl->AddCircleFilled(icon_c, sym, white);                                  // record：实心圆点
    } else if(type[0] == 'r' && type[1] == 'e' && type[2] == 'p') {
        dl->AddCircle(icon_c, sym, white, 12, 2.0f * scale);                      // replay：圆环
    } else if(type[0] == 's' && type[1] == 't') {
        dl->AddTriangleFilled(                                                     // stream：上三角
            ImVec2(icon_c.x, icon_c.y - sym),
            ImVec2(icon_c.x - sym, icon_c.y + sym),
            ImVec2(icon_c.x + sym, icon_c.y + sym), white);
    } else if(type[0] == 's' && type[1] == 'c') {
        dl->AddRect(                                                               // screenshot：方框
            ImVec2(icon_c.x - sym, icon_c.y - sym),
            ImVec2(icon_c.x + sym, icon_c.y + sym), white, 0.0f, ImDrawFlags_None, 2.0f * scale);
    } else {
        dl->AddCircleFilled(icon_c, sym * 0.8f, white);                            // notice：小圆点
    }

    // ---- 文本 ----
    const ImVec2 text_pos(p0.x + pad_x + icon_d + gap,
                          p0.y + (cur_h - std::max(font ? font->CalcTextSizeA(body_size, FLT_MAX, max_text_w, shown.text.c_str()).y : body_size, 0.0f)) * 0.5f);
    if(font)
        dl->AddText(font, body_size, text_pos, IM_COL32(255, 255, 255, A),
                    shown.text.c_str(), shown.text.c_str() + shown.text.size(), max_text_w, nullptr);

    // ---- 超时进度条（胶囊底部内侧的细线）----
    const double remain = std::chrono::duration<double>(shown.expire - now).count();
    const double frac = shown.timeout > 0.0 ? std::min(std::max(remain / shown.timeout, 0.0), 1.0) : 0.0;
    const float bar_y = p1.y - 4.0f * scale;
    const float bar_x0 = p0.x + rounding;
    const float bar_x1 = p1.x - rounding;
    if(bar_x1 > bar_x0) {
        dl->AddRectFilled(ImVec2(bar_x0, bar_y), ImVec2(bar_x0 + (bar_x1 - bar_x0) * (float)frac, bar_y + 3.0f * scale),
                          IM_COL32(255, 255, 255, (int)(110 * A / 255)), 1.5f * scale);
    }

    // ---- 排队角标 "+N" ----
    if(q.size() > 1) {
        const int pending = (int)q.size() - 1;
        const float br = 11.0f * scale;
        const ImVec2 bc(p1.x - br * 0.2f, p0.y + br * 0.2f);
        dl->AddCircleFilled(bc, br, IM_COL32(70, 76, 84, A));
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", pending);
        if(font) {
            const ImVec2 ts = font->CalcTextSizeA(small_size, FLT_MAX, 0.0f, buf);
            dl->AddText(font, small_size,
                        ImVec2(bc.x - ts.x * 0.5f, bc.y - ts.y * 0.5f),
                        IM_COL32(255, 255, 255, A), buf);
        }
    }
}

bool has_pending_notifications() {
    if(!queue().empty())
        return true;
    // 队列空了但胶囊还在收缩/淡出：也算"仍在显示"，
    // 否则 Overlay 会立刻销毁窗口，把淡出动画截断（表现为通知一闪就没）。
    return island().cur_a > 0.01f;
}

double notifications_remaining_seconds() {
    auto& q = queue();
    if(q.empty())
        return 0.0;
    const auto now = std::chrono::steady_clock::now();
    double max_remain = 0.0;
    for(const Toast& t : q) {
        const double r = std::chrono::duration<double>(t.expire - now).count();
        if(r > max_remain)
            max_remain = r;
    }
    return max_remain;
}

}
}
