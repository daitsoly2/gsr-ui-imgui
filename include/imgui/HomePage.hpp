#pragma once

#include <functional>
#include <string>
#include <vector>

namespace gsr {
namespace imgui_ui {

    // 主页要显示的运行时状态。所有文本都已经过 TR() 翻译（ImGui 界面是代码绘制的，
    // 因此直接调用项目现有的翻译宏即可，切换语言下一帧就生效）。
    struct HomeState {
        std::string target;             // 显示器 / 游戏名，可为空

        std::string record_status;      // TR("Not recording") / TR("Recording") / TR("Paused")
        std::string record_action;      // TR("Start") / TR("Stop and save")
        std::string record_hotkey;
        bool record_active = false;
        bool record_paused = false;

        std::string replay_status;      // TR("On") / TR("Off")
        std::string replay_action;      // TR("Turn on") / TR("Turn off")
        std::string replay_hotkey;
        std::string replay_save_hotkey;
        bool replay_active = false;
        bool replay_save_enabled = false;

        std::string stream_status;      // TR("Streaming") / TR("Not streaming")
        std::string stream_action;      // TR("Start") / TR("Stop")
        std::string stream_hotkey;
        bool stream_active = false;

        std::string screenshot_hotkey;
    };

    struct HomeCallbacks {
        std::function<void()> record_toggle;
        std::function<void()> record_pause;
        std::function<void()> record_settings;
        std::function<void()> replay_toggle;
        std::function<void()> replay_save;
        std::function<void()> replay_settings;
        std::function<void()> stream_toggle;
        std::function<void()> stream_settings;
        std::function<void()> screenshot_take;
        std::function<void()> screenshot_settings;
        std::function<void()> open_settings;   // 左下角“设置”方块
    };

    // 在 ImGui 帧内绘制主页（必须在 Host::begin_frame() 与 end_frame() 之间调用）
    void draw_home_page(const HomeState& state, const HomeCallbacks& callbacks);

}
}
