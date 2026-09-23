#pragma once

#include <functional>
#include <cstdint>

#include "../Config.hpp"

namespace gsr {
    struct Config;
    struct GsrInfo;
}

namespace gsr {
namespace imgui_ui {

    /*
        ImGui 设置页框架。

        设计：
        - Overlay 持有一个 SettingsUi；主页"设置"卡打开它（默认进全局页），
          各模块的齿轮打开对应的子页。ESC/返回按钮回到主页。
        - 设置页打开期间 ImGui 继续接管整屏（page_stack 不动，旧页面仅在未移植的模块使用）。
        - 所有控件直接读写 config，改动后 save_config + 回调（与旧设置页行为一致）。
    */
    struct SettingsCallbacks {
        std::function<void()> on_config_changed;              // 配置发生变化（Overlay 里重新应用主题等）
        std::function<void(int)> on_language_changed;         // 语言切换（参数为旧 UI 的滚动位置，ImGui 下传 0）
        std::function<void(bool enable, int exit_status)> on_startup_changed;
        std::function<void(int speed)> on_notification_speed;  // 0=正常 1=快速
        std::function<void()> on_exit_program;
    };

    class SettingsUi {
    public:
        enum class Page { Global, Record, Replay, Stream, Screenshot };

        void open() { open(Page::Global); }
        void open(Page page) { open_ = true; page_ = page; }
        void close() { open_ = false; }
        bool is_open() const { return open_; }

        // 在 ImGui 帧内绘制（begin_frame 与 end_frame 之间）。返回是否仍处于设置页。
        bool draw(Config& config, GsrInfo& gsr_info, const SettingsCallbacks& callbacks);

    private:
        bool open_ = false;
        Page page_ = Page::Global;

        // 热键捕获：非 null 表示正在等待为该 ConfigHotkey 捕获下一按键
        ConfigHotkey* capture_target_ = nullptr;
    };

}
}
