#pragma once

#include <functional>
#include <memory>
#include <cstdint>

namespace mgl {
    class Event;
}

struct ImFont;   // 完整定义在 <imgui.h>

namespace gsr {
namespace imgui_ui {

    class HomePage;

    // 界面统一使用预烘好的三档字号（legacy 图集不允许运行期增量栅格化新字号）
    enum class FontRole { Small, Body, Title };
    ImFont* ui_font(FontRole role);   // 需要包含 <imgui.h>；未加载时返回 nullptr

    // 界面缩放（= 显示器高度 / 1080，夹在 [0.75, 2.0]）。字号/图标/内边距都按它算。
    // 通知窗口是小窗（非全屏），但缩放必须跟显示器走，所以显示尺寸和缩放要分开：
    // 由 Overlay 用显示器高度显式设定，避免"通知小窗"把字号按错误比例烘死。
    float interface_scale();
    void set_ui_scale(float scale);

    // 热键捕获：SettingsUi 开启捕获后，下一次键盘按键会被 Host 记录到这里。
    // 这样无需在 ImGui 层做 ImGuiKey->mgl key 的反向映射（mgl 事件里直接有原始按键）。
    struct HotkeyCapture {
        bool active = false;       // SettingsUi 设为 true 表示正在等待捕获
        bool cancelled = false;    // 用户按 Esc 取消
        int64_t key = 0;           // 捕获到的 mgl 按键（mgl::Keyboard::Key）
        uint32_t modifiers = 0;    // gsr::HotkeyModifier 位
    };
    HotkeyCapture& hotkey_capture();

    /*
        Dear ImGui 宿主。

        职责：把 ImGui 接到 gsr-ui 现有的 mgl 事件循环与 OpenGL 上下文上。

        为什么用 imgui_impl_opengl2 而不是 opengl3：
          mgl 创建的是 OpenGL 2.x 兼容上下文（EGL_CONTEXT_CLIENT_VERSION=2 + EGL_OPENGL_API），
          只加载了固定管线为主的符号表。opengl3 后端需要 VAO 与 GL3 着色器，因此用固定管线的
          opengl2 后端（客户端数组 + 矩阵栈），与本项目的 GL 能力匹配。

        相比声明式 UI 方案少了一层：ImGui 的界面是 C++ 代码直接绘制，
          所以不需要文件格式、不需要渲染接口实现、不需要翻译文件注入——
          文案直接用项目现有的 TR() 宏，切换语言立即生效。

        注意：mgl 的 GL 状态只在上下文创建时设置一次（glBlendFuncSeparate 等），
        而 ImGui 的 GL2 后端不会还原混合模式，因此 end_frame() 里必须 restore_mgl_state()。
    */
    class Host {
    public:
        Host();
        Host(const Host&) = delete;
        Host& operator=(const Host&) = delete;
        ~Host();

        bool is_initialized() const;

        // 窗口尺寸变化时调用（同时更新 ImGui 的 DisplaySize）
        void set_display_size(int width, int height);

        // 把 mgl 事件喂给 ImGui。返回 true 表示事件被界面消费
        bool handle_event(mgl::Event& event);

        // 一帧：begin_frame() -> 你自己调 ImGui::* 画界面 -> end_frame()
        void begin_frame();
        void end_frame();

        // 应用 gsr 的配色（GNOME Adwaita 蓝 + 深色底 + 圆角），与现有自绘 UI 保持一致
        void apply_theme(unsigned int tint_color_rgba, unsigned int page_bg_color_rgba);

        // 单独喂鼠标位置（Overlay 打开时会主动同步一次，避免鼠标位置残留）
        void set_mouse_position(float x, float y);

    private:
        void restore_mgl_state();
        void load_font();
        // legacy 字体图集：构建图集并自己上传纹理（绘制命令直接引用该纹理）
        void upload_font_texture();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}
}
