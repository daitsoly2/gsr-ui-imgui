#pragma once

#include <string>
#include <mglpp/graphics/Color.hpp>

namespace gsr {
namespace imgui_ui {

// 内部通知队列：替代外部 gsr-notify 进程。
//
// 调用方（Overlay::show_notification）把一条通知 push 进来，
// 由 draw_notifications() 在 ImGui 前景绘制层（GetForegroundDrawList）渲染。
// 即使覆盖层 UI 处于隐藏态（透明 + 点击穿透），只要 Overlay 短暂唤起窗口，
// 这里就会照常画出 toast —— 因此无需再 spawn 一个独立进程。
//
// 注意：本头文件不能 #include <imgui.h>（Overlay 也不行），绘制逻辑全部在 .cpp 里。
void push_notification(
    const std::string& text,
    double timeout_seconds,
    mgl::Color icon_color,
    mgl::Color bg_color,
    const char* icon_type);  // 来自 notification_type_to_string 的结果，可为 nullptr

// 在 ImGui 帧内调用（begin_frame 之后、end_frame 之前）。
// 自行处理过期与淡入淡出；队列为空时不绘制任何内容。
void draw_notifications();

// 清空所有待显示通知（例如退出前）。
void clear_notifications();

// 是否还有未过期的通知。
// 覆盖层在"隐藏 UI"时用它判断：若有待显示通知，就不要立刻销毁窗口，
// 否则内嵌 toast 会跟着一起消失（旧方案是独立进程 gsr-notify，不受影响）。
bool has_pending_notifications();

// 队列中最长的剩余显示时间（秒）；无通知返回 0。
double notifications_remaining_seconds();

}
}
