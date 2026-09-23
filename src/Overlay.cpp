#include "../include/Overlay.hpp"
#include "../include/Theme.hpp"
#include "../include/Config.hpp"
#include "../include/Process.hpp"
#include "../include/Utils.hpp"
#include "../include/gui/StaticPage.hpp"
#include "../include/gui/DropdownButton.hpp"
#include "../include/gui/CustomRendererWidget.hpp"
#include "../include/gui/Image.hpp"
#include "../include/gui/Label.hpp"
#include "../include/gui/SettingsPage.hpp"
#include "../include/gui/ScreenshotSettingsPage.hpp"
#include "../include/gui/GlobalSettingsPage.hpp"
#include "../include/gui/Utils.hpp"
#include "../include/Translation.hpp"
#include "../include/DesktopEnvironment/DesktopEnvironmentX11.hpp"
#include "../include/DesktopEnvironment/DesktopEnvironmentWlroots.hpp"
#include "../include/DesktopEnvironment/DesktopEnvironmentKde.hpp"
#include "../include/DesktopEnvironment/DesktopEnvironmentGnome.hpp"
#include "../include/gui/PageStack.hpp"
#include "../include/WindowUtils.hpp"
#include "../include/GlobalHotkeys/GlobalHotkeys.hpp"
#include "../include/GlobalHotkeys/GlobalHotkeysLinux.hpp"
#include "../include/CursorTracker/CursorTrackerX11.hpp"
#include "../include/CursorTracker/CursorTrackerDrm.hpp"
#include "../include/CursorTracker/CursorTrackerHyprland.hpp"
#include "../include/CursorTracker/CursorTrackerNiri.hpp"
#include "../include/CursorTracker/CursorTrackerSway.hpp"
#include "../include/Clipboard/ClipboardX11.hpp"
#include "../include/Clipboard/ClipboardWayland.hpp"
#include "../include/RegionSelector/RegionSelectorX11.hpp"
#include "../include/RegionSelector/RegionSelectorWayland.hpp"


#ifdef GSR_IMGUI
#include "../include/imgui/Host.hpp"
#include "../include/imgui/HomePage.hpp"
#include "../include/imgui/NotificationToast.hpp"
// 注意：这里绝不能 #include <imgui.h>。
// X11 的 Xlib.h 用 "#define Status int" 定义 Status，而 imgui.h 里有 "#undef Status"，
// 一旦 imgui.h 先于 X11 头被包含，下面的 X11/Xutil.h、XKBlib.h 就会因 Status 未定义而全部报错。
// 因此颜色打包在下面用 pack_color() 手工完成，不依赖 IM_COL32。
#endif

#include <iomanip>
#include <string.h>
#include <assert.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <poll.h>
#include <malloc.h>
#include <stdexcept>
#include <algorithm>
#include <sstream> // std::stringstream
#include <inttypes.h>
#include <math.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/shapeconst.h>
#include <X11/Xcursor/Xcursor.h>

#include <wayland-client.h>

#include <mglpp/system/Rect.hpp>
#include <mglpp/window/Event.hpp>
#include <mglpp/system/Utf8.hpp>

extern "C" {
#include <mgl/mgl.h>
}

namespace gsr {
    static const mgl::Color bg_color(0, 0, 0, 100);
    static const double force_window_on_top_timeout_seconds = 1.0;
    static const double replay_status_update_check_timeout_seconds = 1.5;
    static const double replay_saving_notification_timeout_seconds = 0.5;
    static const double short_notification_timeout_seconds = 2.0;
    static const double notification_timeout_seconds = 3.0;
    static const double notification_error_timeout_seconds = 5.0;
    static const double cursor_tracker_update_timeout_sec = 0.1;

    static mgl::Texture texture_from_ximage(XImage *img) {
        uint8_t *texture_data = (uint8_t*)malloc(img->width * img->height * 3);
        // TODO:

        for(int y = 0; y < img->height; ++y) {
            for(int x = 0; x < img->width; ++x) {
                unsigned long pixel = XGetPixel(img, x, y);
                unsigned char red = (pixel & img->red_mask) >> 16;
                unsigned char green = (pixel & img->green_mask) >> 8;
                unsigned char blue = pixel & img->blue_mask;

                const size_t texture_data_index = (x + y * img->width) * 3;
                texture_data[texture_data_index + 0] = red;
                texture_data[texture_data_index + 1] = green;
                texture_data[texture_data_index + 2] = blue;
            }
        }

        mgl::Texture texture;
        // TODO:
        texture.load_from_memory(texture_data, img->width, img->height, MGL_IMAGE_FORMAT_RGB);
        free(texture_data);
        return texture;
    }

    static bool texture_from_x11_cursor(XcursorImage *x11_cursor_image, bool *visible, mgl::vec2i *hotspot, mgl::Texture &texture) {
        uint8_t *cursor_data = NULL;
        uint8_t *out = NULL;
        const unsigned int *pixels = NULL;
        *visible = false;

        if(!x11_cursor_image)
            return false;

        if(!x11_cursor_image->pixels)
            return false;

        hotspot->x = x11_cursor_image->xhot;
        hotspot->y = x11_cursor_image->yhot;

        pixels = x11_cursor_image->pixels;
        cursor_data = (uint8_t*)malloc((int)x11_cursor_image->width * (int)x11_cursor_image->height * 4);
        if(!cursor_data)
            return false;

        out = cursor_data;
        /* Un-premultiply alpha */
        for(uint32_t y = 0; y < x11_cursor_image->height; ++y) {
            for(uint32_t x = 0; x < x11_cursor_image->width; ++x) {
                uint32_t pixel = *pixels++;
                uint8_t *in = (uint8_t*)&pixel;
                uint8_t alpha = in[3];
                if(alpha == 0) {
                    alpha = 1;
                } else {
                    *visible = true;
                }

                out[0] = (float)in[2] * 255.0/(float)alpha;
                out[1] = (float)in[1] * 255.0/(float)alpha;
                out[2] = (float)in[0] * 255.0/(float)alpha;
                out[3] = in[3];
                out += 4;
                in += 4;
            }
        }

        texture.load_from_memory(cursor_data, x11_cursor_image->width, x11_cursor_image->height, MGL_IMAGE_FORMAT_RGBA);
        free(cursor_data);
        return true;
    }

    static char hex_value_to_str(uint8_t v) {
        if(v <= 9)
            return '0' + v;
        else if(v >= 10 && v <= 15)
            return 'A' + (v - 10);
        else
            return '0';
    }

    // Excludes alpha
    [[maybe_unused]] static std::string color_to_hex_str(mgl::Color color) {
        std::string result;
        result.resize(6);

        result[0] = hex_value_to_str((color.r & 0xF0) >> 4);
        result[1] = hex_value_to_str(color.r & 0x0F);

        result[2] = hex_value_to_str((color.g & 0xF0) >> 4);
        result[3] = hex_value_to_str(color.g & 0x0F);

        result[4] = hex_value_to_str((color.b & 0xF0) >> 4);
        result[5] = hex_value_to_str(color.b & 0x0F);

        return result;
    }

    static bool diff_int(int a, int b, int difference) {
        return std::abs(a - b) <= difference;
    }

    static bool is_window_fullscreen_on_monitor(Display *display, Window window, const Monitor &monitor) {
        if(!window)
            return false;

        DrawableGeometry geometry;
        if(!get_drawable_geometry(display, window, &geometry))
            return false;

        const int margin = 2;
        return diff_int(geometry.x, monitor.position.x, margin) && diff_int(geometry.y, monitor.position.y, margin)
            && diff_int(geometry.width, monitor.size.x, margin) && diff_int(geometry.height, monitor.size.y, margin);
    }

    /*static bool is_window_fullscreen_on_monitor(Display *display, Window window, const mgl_monitor *monitors, int num_monitors) {
        if(!window)
            return false;

        DrawableGeometry geometry;
        if(!get_drawable_geometry(display, window, &geometry))
            return false;

        const int margin = 2;
        for(int i = 0; i < num_monitors; ++i) {
            const mgl_monitor *mon = &monitors[i];
            if(diff_int(geometry.x, mon->pos.x, margin) && diff_int(geometry.y, mon->pos.y, margin)
                && diff_int(geometry.width, mon->size.x, margin) && diff_int(geometry.height, mon->size.y, margin))
            {
                return true;
            }
        }

        return false;
    }*/

    static void wait_until_window_viewable(Display *display, Window window, double timeout_seconds) {
        mgl::Clock clock;
        while(clock.get_elapsed_time_seconds() < timeout_seconds) {
            XWindowAttributes window_attr;
            window_attr.map_state = IsUnmapped;
            XGetWindowAttributes(display, window, &window_attr);
            if(window_attr.map_state == IsViewable)
                return;
            usleep(1000);
        }
    }

    static const Monitor* find_monitor_at_position(const std::vector<Monitor> &monitors, mgl::vec2i pos) {
        for(const Monitor &monitor : monitors) {
            if(mgl::IntRect(monitor.position, monitor.size).contains(pos))
                return &monitor;
        }
        return nullptr;
    }

    static const Monitor* find_monitor_by_name(const std::vector<Monitor> &monitors, const std::string &name) {
        for(const Monitor &monitor : monitors) {
            if(monitor.name == name)
                return &monitor;
        }
        return nullptr;
    }

    static std::string get_power_supply_online_filepath() {
        std::string result;
        const char *paths[] = {
            "/sys/class/power_supply/ADP0/online",
            "/sys/class/power_supply/ADP1/online",
            "/sys/class/power_supply/AC/online",
            "/sys/class/power_supply/ACAD/online"
        };
        for(const char *power_supply_online_filepath : paths) {
            if(access(power_supply_online_filepath, F_OK) == 0) {
                result = power_supply_online_filepath;
                break;
            }
        }
        return result;
    }

    static bool power_supply_is_connected(const char *power_supply_online_filepath) {
        int fd = open(power_supply_online_filepath, O_RDONLY);
        if(fd == -1)
            return false;

        char buf[1];
        const bool is_connected = read(fd, buf, 1) == 1 && buf[0] == '1';
        close(fd);
        return is_connected;
    }

    static bool xinput_is_supported(Display *dpy, int *xi_opcode) {
        *xi_opcode = 0;
        int query_event = 0;
        int query_error = 0;
        if(!XQueryExtension(dpy, "XInputExtension", xi_opcode, &query_event, &query_error)) {
            fprintf(stderr, "gsr-ui error: X Input extension not available\n");
            return false;
        }

        int major = 2;
        int minor = 1;
        int retval = XIQueryVersion(dpy, &major, &minor);
        if (retval != Success) {
            fprintf(stderr, "gsr-ui error: XInput 2.1 is not supported\n");
            return false;
        }

        return true;
    }

    static bool are_all_audio_tracks_available_to_capture(const std::vector<AudioTrack> &audio_tracks) {
        const auto audio_devices = get_audio_devices();
        for(const AudioTrack &audio_track : audio_tracks) {
            for(const std::string &audio_input : audio_track.audio_inputs) {
                std::string_view audio_track_name(audio_input.c_str());
                const bool is_app_audio = starts_with(audio_track_name, "app:");
                if(is_app_audio)
                    continue;

                if(starts_with(audio_track_name, "device:"))
                    audio_track_name.remove_prefix(7);

                auto it = std::find_if(audio_devices.begin(), audio_devices.end(), [&](const auto &audio_device) {
                    return audio_device.name == audio_track_name;
                });
                if(it == audio_devices.end()) {
                    //fprintf(stderr, "Audio not ready\n");
                    return false;
                }
            }
        }
        return true;
    }

    static bool is_webcam_available_to_capture(const RecordOptions &record_options) {
        if(record_options.webcam_source.empty())
            return true;

        const auto cameras = get_v4l2_devices();
        for(const GsrCamera &camera : cameras) {
            if(camera.path == record_options.webcam_source)
                return true;
        }
        return false;
    }

    static Hotkey config_hotkey_to_hotkey(ConfigHotkey config_hotkey) {
        return {
            (uint32_t)mgl::Keyboard::key_to_x11_keysym((mgl::Keyboard::Key)config_hotkey.key),
            config_hotkey.modifiers
        };
    }

    static void bind_linux_hotkeys(GlobalHotkeysLinux *global_hotkeys, Overlay *overlay, bool enable_region_exit) {
        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().main_config.show_hide_hotkey),
            "toggle_show", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_show();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().record_config.start_stop_hotkey),
            "record", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_record(RecordForceType::NONE);
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().record_config.pause_unpause_hotkey),
            "pause", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_pause();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().record_config.start_stop_region_hotkey),
            "record_region", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_record(RecordForceType::REGION);
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().record_config.start_stop_window_hotkey),
            "record_window", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_record(RecordForceType::WINDOW);
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().streaming_config.start_stop_hotkey),
            "stream", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_stream();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().replay_config.start_stop_hotkey),
            "replay_start", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_replay();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().replay_config.save_hotkey),
            "replay_save", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->save_replay();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().replay_config.save_1_min_hotkey),
            "replay_save_1_min", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->save_replay_1_min();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().replay_config.save_10_min_hotkey),
            "replay_save_10_min", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->save_replay_10_min();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().screenshot_config.take_screenshot_hotkey),
            "take_screenshot", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->take_screenshot();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().screenshot_config.take_screenshot_region_hotkey),
            "take_screenshot_region", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->take_screenshot_region();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().screenshot_config.take_screenshot_window_hotkey),
            "take_screenshot_window", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->take_screenshot_window();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(ConfigHotkey{ mgl::Keyboard::Key::Escape, HOTKEY_MOD_LCTRL | HOTKEY_MOD_LSHIFT | HOTKEY_MOD_LALT }),
            "exit", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->go_back_to_old_ui();
            });

        if(enable_region_exit) {
            global_hotkeys->bind_key_press(
                config_hotkey_to_hotkey(ConfigHotkey{ mgl::Keyboard::Key::Escape }),
                "cancel_region_selection", [overlay](const std::string &id) {
                    fprintf(stderr, "pressed %s\n", id.c_str());
                    overlay->cancel_region_selection();
                });
        }
    }

    static std::unique_ptr<GlobalHotkeysLinux> register_linux_hotkeys(Overlay *overlay, Display *x11_dpy, GlobalHotkeysLinux::GrabType grab_type, bool enable_region_exit) {
        auto global_hotkeys = std::make_unique<GlobalHotkeysLinux>(x11_dpy, grab_type);
        if(!global_hotkeys->start())
            fprintf(stderr, "error: failed to start global hotkeys\n");

        bind_linux_hotkeys(global_hotkeys.get(), overlay, enable_region_exit);
        global_hotkeys->on_gsr_ui_virtual_keyboard_grabbed = [overlay]() {
            overlay->global_hotkeys_ungrab_keyboard = true;
        };
        return global_hotkeys;
    }

    static std::unique_ptr<GlobalHotkeysJoystick> register_joystick_hotkeys(Overlay *overlay) {
        auto global_hotkeys_js = std::make_unique<GlobalHotkeysJoystick>();
        if(!global_hotkeys_js->start())
            fprintf(stderr, "Warning: failed to start joystick hotkeys\n");

        global_hotkeys_js->bind_action("toggle_show", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->toggle_show();
        });

        global_hotkeys_js->bind_action("save_replay", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->save_replay();
        });

        global_hotkeys_js->bind_action("save_1_min_replay", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->save_replay_1_min();
        });

        global_hotkeys_js->bind_action("save_10_min_replay", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->save_replay_10_min();
        });

        global_hotkeys_js->bind_action("take_screenshot", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->take_screenshot();
        });

        global_hotkeys_js->bind_action("toggle_record", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->toggle_record(RecordForceType::NONE);
        });

        global_hotkeys_js->bind_action("toggle_replay", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->toggle_replay();
        });

        return global_hotkeys_js;
    }

    static NotificationSpeed to_notification_speed(const std::string &notification_speed_str) {
        if(notification_speed_str == "normal")
            return NotificationSpeed::NORMAL;
        else if(notification_speed_str == "fast")
            return NotificationSpeed::FAST;
        else {
            assert(false);
            return NotificationSpeed::NORMAL;
        }
    }

    static pid_t launch_gsr_game_tracker(int *stdout_fd) {
        const bool is_flatpak = getenv("FLATPAK_ID") != nullptr;
        if(is_flatpak) {
            const char *args[] = { "flatpak-spawn", "--host", "--", "/var/lib/flatpak/app/com.dec05eba.gpu_screen_recorder/current/active/files/bin/gsr-game-tracker", NULL };
            return exec_program(args, stdout_fd, false);
        } else {
            const char *args[] = { "gsr-game-tracker", NULL };
            return exec_program(args, stdout_fd, false);
        }
    }

    static int mgl_x_error_handler(Display *display, XErrorEvent *ee) {
        (void)display;
        (void)ee;
        return 0;
    }

    static int mgl_x_io_error_handler(Display *display) {
        (void)display;
        return 0;
    }

    Overlay::Overlay(std::string resources_path, GsrInfo gsr_info, SupportedCaptureOptions capture_options, egl_functions egl_funcs, struct wl_display *wayland_dpy) :
        resources_path(std::move(resources_path)),
        gsr_info(std::move(gsr_info)),
        egl_funcs(egl_funcs),
        config(capture_options),
        current_recording_config(capture_options),
        bg_screenshot_overlay({0.0f, 0.0f}),
        top_bar_background({0.0f, 0.0f}),
        close_button_widget({0.0f, 0.0f})
    {
        // wayland_dpy is borrowed from main(); never disconnected here.
        this->wayland_dpy = wayland_dpy;

        gsr_icon_path = this->resources_path + "images/gpu_screen_recorder_logo.png";


#ifdef GSR_IMGUI
        // ImGui 界面层：主页由 ImGui 绘制（设置页在移植完成前仍是旧的 Widget 实现）
        imgui_host = std::make_unique<imgui_ui::Host>();
        if(imgui_host->is_initialized())
            fprintf(stderr, "Info: 使用 ImGui 界面（主页由 ImGui 绘制）\n");
        else
            fprintf(stderr, "Warning: 初始化 ImGui 失败，回退到原界面\n");
#else
        fprintf(stderr, "Info: 使用原界面（本次构建未启用 ImGui，如需新界面请用 -Dimgui=true 重新构建）\n");
#endif

        key_bindings[0].key_event.code = mgl::Keyboard::Escape;
        key_bindings[0].key_event.key_states.alt = false;
        key_bindings[0].key_event.key_states.control = false;
        key_bindings[0].key_event.key_states.shift = false;
        key_bindings[0].key_event.key_states.system = false;
        key_bindings[0].callback = [this]() {
            page_stack.pop();
        };

        memset(&window_texture, 0, sizeof(window_texture));

        std::optional<Config> new_config = read_config(capture_options);
        if(new_config)
            config = std::move(new_config.value());

        init_color_theme(config, this->gsr_info);

        power_supply_online_filepath = get_power_supply_online_filepath();
        replay_startup_mode = replay_startup_string_to_type(config.replay_config.turn_on_replay_automatically_mode.c_str());
        set_notification_speed(to_notification_speed(config.main_config.notification_speed));

        x11_dpy = XOpenDisplay(nullptr);
        if(x11_dpy) {
            XKeysymToKeycode(x11_dpy, XK_F1); // If we dont call we will never get a MappingNotify
        } else {
            fprintf(stderr, "Warning: XOpenDisplay failed to mapping notify\n");
        }

        if(x11_dpy && mgl_get_context()->display_server_is_wayland) {
            XSetErrorHandler(mgl_x_error_handler);
            XSetIOErrorHandler(mgl_x_io_error_handler);
        }

        if(config.main_config.hotkeys_enable_option == "enable_hotkeys")
            global_hotkeys = register_linux_hotkeys(this, x11_dpy, GlobalHotkeysLinux::GrabType::ALL, on_region_selected != nullptr);
        else if(config.main_config.hotkeys_enable_option == "enable_hotkeys_virtual_devices")
            global_hotkeys = register_linux_hotkeys(this, x11_dpy, GlobalHotkeysLinux::GrabType::VIRTUAL, on_region_selected != nullptr);
        else if(config.main_config.hotkeys_enable_option == "enable_hotkeys_no_grab")
            global_hotkeys = register_linux_hotkeys(this, x11_dpy, GlobalHotkeysLinux::GrabType::NO_GRAB, on_region_selected != nullptr);

        if(config.main_config.joystick_hotkeys_enable_option == "enable_hotkeys")
            global_hotkeys_js = register_joystick_hotkeys(this);

#ifdef GSR_IMGUI
        // ImGui 设置页的回调。必须接线，否则设置页里的"退出程序"等按钮全是空操作。
        // 行为与旧的 settings_page->on_* 保持一致。
        imgui_settings_callbacks.on_exit_program = [this]() {
            do_exit = true;
            exit_reason = "exit";
        };
        imgui_settings_callbacks.on_language_changed = [this](int scroll_y) {
            pending_settings_scroll_y = scroll_y;
            reload_ui = true;
            reopen_settings_after_reload = true;
        };
        imgui_settings_callbacks.on_startup_changed = [this](bool enable, int exit_status) {
            if(exit_status == 0)
                return;

            if(exit_status == 67) {
                const bool is_flatpak = getenv("FLATPAK_ID") != nullptr;
                const char *startup_command = is_flatpak ? "flatpak run com.dec05eba.gpu_screen_recorder gsr-ui" : "gsr-ui launch-daemon";
                show_notification(
                    TRF("To enable autorun: install and configure 'dex' (recommended), or manually add '%s' to your desktop autostart entries.", startup_command).c_str(),
                    10.0, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0),
                    NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
                return;
            }

            if(enable)
                show_notification(TR("Failed to add GPU Screen Recorder to system startup"), notification_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
            else
                show_notification(TR("Failed to remove GPU Screen Recorder from system startup"), notification_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
        };
        imgui_settings_callbacks.on_notification_speed = [this](int speed) {
            set_notification_speed(speed == 1 ? NotificationSpeed::FAST : NotificationSpeed::NORMAL);
        };
        imgui_settings_callbacks.on_config_changed = [this]() {
            update_led_indicator_after_settings_change();
        };
#endif

        if(this->gsr_info.system_info.display_server == DisplayServer::X11) {
            cursor_tracker = std::make_unique<CursorTrackerX11>(x11_dpy);
            desktop_environment = std::make_unique<DesktopEnvironmentX11>(x11_dpy);
            supports_window_title = true;
        } else if(this->gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
            const char *xdg_current_desktop = getenv("XDG_CURRENT_DESKTOP");

            const std::string wm_name = x11_dpy ? get_window_manager_name(x11_dpy) : "";
            const bool is_kwin = wm_name == "KWin";
            const bool is_mutter = wm_name.find("GNOME") != std::string::npos;
            const bool is_hyprland = xdg_current_desktop && strstr(xdg_current_desktop, "Hyprland");
            const bool is_niri = xdg_current_desktop && strstr(xdg_current_desktop, "niri");
            const bool is_sway = xdg_current_desktop && strstr(xdg_current_desktop, "sway");

            if(is_hyprland && CursorTrackerHyprland::is_supported())
                cursor_tracker = std::make_unique<CursorTrackerHyprland>();
            else if(is_niri && CursorTrackerNiri::is_supported())
                cursor_tracker = std::make_unique<CursorTrackerNiri>();
            else if(is_sway && CursorTrackerSway::is_supported())
                cursor_tracker = std::make_unique<CursorTrackerSway>();
            else
                cursor_tracker = std::make_unique<CursorTrackerDrm>(wayland_dpy);

            if(is_kwin) {
                desktop_environment = std::make_unique<DesktopEnvironmentKde>();
                supports_window_title = true;
            } else if(is_mutter) {
                desktop_environment = std::make_unique<DesktopEnvironmentGnome>(x11_dpy);
                supports_window_title = true;
            } else if(DesktopEnvironmentWlroots::is_supported(wayland_dpy)) {
                desktop_environment = std::make_unique<DesktopEnvironmentWlroots>(wayland_dpy);
                supports_window_title = true;
            } else {
                desktop_environment = std::make_unique<DesktopEnvironmentX11>(x11_dpy);
            }

            if(!config.main_config.wayland_warning_shown) {
                config.main_config.wayland_warning_shown = true;
                save_config(config);
                show_notification(TR("Wayland doesn't support GPU Screen Recorder UI properly, things may not work as expected. Use X11 if you experience issues."), notification_error_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
            }
        }

        if(wayland_dpy && RegionSelectorWayland::is_supported(wayland_dpy))
            region_selector = std::make_unique<RegionSelectorWayland>(wayland_dpy);
        else
            region_selector = std::make_unique<RegionSelectorX11>(x11_dpy);

        if(wayland_dpy && ClipboardWayland::is_supported(wayland_dpy)) {
            clipboard = std::make_unique<ClipboardWayland>(wayland_dpy);
            wayland_native_clipboard = true;
        } else {
            clipboard = std::make_unique<ClipboardX11>();
        }

        desktop_environment->start();
        update_led_indicator_after_settings_change();

        gsr_game_tracker_process_id = launch_gsr_game_tracker(&gsr_game_tracker_process_output_fd);
        if(gsr_game_tracker_process_id > 0) {
            const int fdl = fcntl(gsr_game_tracker_process_output_fd, F_GETFL);
            fcntl(gsr_game_tracker_process_output_fd, F_SETFL, fdl | O_NONBLOCK);
            gsr_game_tracker_process_output_file = fdopen(gsr_game_tracker_process_output_fd, "r");
            if(gsr_game_tracker_process_output_file)
                gsr_game_tracker_process_output_fd = -1;
        } else {
            fprintf(stderr, "Warning: failed to launch gsr-game-tracker. The feature to start replay when a game starts will not work\n");
        }
    }

    Overlay::~Overlay() {
        hide();

        if(notification_process > 0) {
            kill(notification_process, SIGINT);
            int status;
            if(waitpid(notification_process, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            }
            notification_process = -1;
        }

        if(gpu_screen_recorder_process > 0) {
            kill(gpu_screen_recorder_process, SIGINT);
            int status;
            if(waitpid(gpu_screen_recorder_process, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            }
            gpu_screen_recorder_process = -1;
        }

        if(gpu_screen_recorder_screenshot_process > 0) {
            kill(gpu_screen_recorder_screenshot_process, SIGINT);
            int status;
            if(waitpid(gpu_screen_recorder_screenshot_process, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            }
            gpu_screen_recorder_screenshot_process = -1;
        }

        if(gsr_game_tracker_process_id > 0) {
            kill(gsr_game_tracker_process_id, SIGINT);
            int status;
            if(waitpid(gsr_game_tracker_process_id, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            }
            gsr_game_tracker_process_id = -1;
        }

        led_indicator.reset();
        region_selector.reset();
        desktop_environment.reset();

        close_gsr_game_tracker_output();
        close_gpu_screen_recorder_output();
        deinit_color_theme();

        if(x11_dpy)
            XCloseDisplay(x11_dpy);

        // wayland_dpy is borrowed from main() — do not disconnect.
    }

    void Overlay::xi_setup() {
        xi_display = XOpenDisplay(nullptr);
        if(!xi_display) {
            fprintf(stderr, "gsr-ui error: failed to setup XI connection\n");
            return;
        }

        if(!xinput_is_supported(xi_display, &xi_opcode))
            goto error;

        xi_input_xev = (XEvent*)calloc(1, sizeof(XEvent));
        if(!xi_input_xev)
            throw std::runtime_error("gsr-ui error: failed to allocate XEvent data");

        xi_output_xev = (XEvent*)calloc(1, sizeof(XEvent));
        if(!xi_output_xev)
            throw std::runtime_error("gsr-ui error: failed to allocate XEvent data");

        unsigned char mask[XIMaskLen(XI_LASTEVENT)];
        memset(mask, 0, sizeof(mask));
        XISetMask(mask, XI_Motion);
        //XISetMask(mask, XI_RawMotion);
        XISetMask(mask, XI_ButtonPress);
        XISetMask(mask, XI_ButtonRelease);
        XISetMask(mask, XI_KeyPress);
        XISetMask(mask, XI_KeyRelease);

        XIEventMask xi_masks;
        xi_masks.deviceid = XIAllMasterDevices;
        xi_masks.mask_len = sizeof(mask);
        xi_masks.mask = mask;
        if(XISelectEvents(xi_display, DefaultRootWindow(xi_display), &xi_masks, 1) != Success) {
            fprintf(stderr, "gsr-ui error: XISelectEvents failed\n");
            goto error;
        }

        XFlush(xi_display);
        return;

        error:
        free(xi_input_xev);
        xi_input_xev = nullptr;
        free(xi_output_xev);
        xi_output_xev = nullptr;
        if(xi_display) {
            XCloseDisplay(xi_display);
            xi_display = nullptr;
        }
    }

    void Overlay::close_gsr_game_tracker_output() {
        if(gsr_game_tracker_process_output_file) {
            fclose(gsr_game_tracker_process_output_file);
            gsr_game_tracker_process_output_file = nullptr;
        }

        if(gsr_game_tracker_process_output_fd > 0) {
            close(gsr_game_tracker_process_output_fd);
            gsr_game_tracker_process_output_fd = -1;
        }
    }

    void Overlay::close_gpu_screen_recorder_output() {
        if(gpu_screen_recorder_process_output_file) {
            fclose(gpu_screen_recorder_process_output_file);
            gpu_screen_recorder_process_output_file = nullptr;
        }

        if(gpu_screen_recorder_process_output_fd > 0) {
            close(gpu_screen_recorder_process_output_fd);
            gpu_screen_recorder_process_output_fd = -1;
        }
    }

    void Overlay::handle_xi_events() {
        if(!xi_display)
            return;
        // The XInput2 path injects synthesized XEvents into the mgl X11 window. On the
        // native Wayland overlay path the system handle is a wl_egl_window, not a Window,
        // and input arrives through the mgl Wayland event loop directly — skip XI here.
        if(wayland_native_overlay)
            return;

        // The synthesized XEvents target the mgl window, so the display field
        // must be mgl's X11 connection (the one that owns that window).
        Display *display = (Display*)mgl_get_context()->connection;

        while(XPending(xi_display)) {
            XNextEvent(xi_display, xi_input_xev);
            XGenericEventCookie *cookie = &xi_input_xev->xcookie;
            if(cookie->type == GenericEvent && cookie->extension == xi_opcode && XGetEventData(xi_display, cookie)) {
                const XIDeviceEvent *de = (XIDeviceEvent*)cookie->data;
                if(cookie->evtype == XI_Motion) {
                    memset(xi_output_xev, 0, sizeof(*xi_output_xev));
                    xi_output_xev->type = MotionNotify;
                    xi_output_xev->xmotion.display = display;
                    xi_output_xev->xmotion.window = (Window)window->get_system_handle();
                    xi_output_xev->xmotion.subwindow = (Window)window->get_system_handle();
                    xi_output_xev->xmotion.x = de->root_x - window_pos.x;
                    xi_output_xev->xmotion.y = de->root_y - window_pos.y;
                    xi_output_xev->xmotion.x_root = de->root_x;
                    xi_output_xev->xmotion.y_root = de->root_y;
                    //xi_output_xev->xmotion.state = // modifiers // TODO:
                    if(window->inject_x11_event(xi_output_xev, event))
                        on_event(event);
                } else if(cookie->evtype == XI_ButtonPress || cookie->evtype == XI_ButtonRelease) {
                    memset(xi_output_xev, 0, sizeof(*xi_output_xev));
                    xi_output_xev->type = cookie->evtype == XI_ButtonPress ? ButtonPress : ButtonRelease;
                    xi_output_xev->xbutton.display = display;
                    xi_output_xev->xbutton.window = (Window)window->get_system_handle();
                    xi_output_xev->xbutton.subwindow = (Window)window->get_system_handle();
                    xi_output_xev->xbutton.x = de->root_x - window_pos.x;
                    xi_output_xev->xbutton.y = de->root_y - window_pos.y;
                    xi_output_xev->xbutton.x_root = de->root_x;
                    xi_output_xev->xbutton.y_root = de->root_y;
                    //xi_output_xev->xbutton.state = // modifiers // TODO:
                    xi_output_xev->xbutton.button = de->detail;
                    if(window->inject_x11_event(xi_output_xev, event))
                        on_event(event);
                } else if(cookie->evtype == XI_KeyPress || cookie->evtype == XI_KeyRelease) {
                    memset(xi_output_xev, 0, sizeof(*xi_output_xev));
                    xi_output_xev->type = cookie->evtype == XI_KeyPress ? KeyPress : KeyRelease;
                    xi_output_xev->xkey.display = display;
                    xi_output_xev->xkey.window = (Window)window->get_system_handle();
                    xi_output_xev->xkey.subwindow = (Window)window->get_system_handle();
                    xi_output_xev->xkey.x = de->root_x - window_pos.x;
                    xi_output_xev->xkey.y = de->root_y - window_pos.y;
                    xi_output_xev->xkey.x_root = de->root_x;
                    xi_output_xev->xkey.y_root = de->root_y;
                    xi_output_xev->xkey.state = de->mods.effective;
                    xi_output_xev->xkey.keycode = de->detail;
                    if(window->inject_x11_event(xi_output_xev, event))
                        on_event(event);
                }
                //fprintf(stderr, "got xi event: %d\n", cookie->evtype);
                XFreeEventData(xi_display, cookie);
            }
        }
    }

    static uint32_t key_event_to_bitmask(mgl::Event::KeyEvent key_event) {
        return ((uint32_t)key_event.key_states.alt     << (uint32_t)0)
            |  ((uint32_t)key_event.key_states.control << (uint32_t)1)
            |  ((uint32_t)key_event.key_states.shift   << (uint32_t)2)
            |  ((uint32_t)key_event.key_states.system  << (uint32_t)3);
    }

    void Overlay::process_key_bindings(mgl::Event &event) {
        if(event.type != mgl::Event::KeyReleased)
            return;

        const uint32_t event_key_bitmask = key_event_to_bitmask(event.key);
        for(const KeyBinding &key_binding : key_bindings) {
            if(event.key.code == key_binding.key_event.code && event_key_bitmask == key_event_to_bitmask(key_binding.key_event))
                key_binding.callback();
        }
    }

    void Overlay::stop_region_selection() {
        if(on_region_selected) {
            on_region_selected = nullptr;
            rebind_all_keyboard_hotkeys();
        }
    }

    void Overlay::handle_x11_events() {
        if(!x11_dpy)
            return;

        bool mapping_updated = false;
        while(XPending(x11_dpy)) {
            XNextEvent(x11_dpy, &x11_xev);
            switch(x11_xev.type) {
                case MappingNotify: {
                    XRefreshKeyboardMapping(&x11_xev.xmapping);
                    mapping_updated = true;
                    break;
                }
            }
            region_selector->handle_event(&x11_xev);
        }

        if(mapping_updated)
            rebind_all_keyboard_hotkeys();
    }

    void Overlay::handle_wayland_events() {
        if(!wayland_dpy)
            return;

        while(wl_display_prepare_read(wayland_dpy) != 0) {
            wl_display_dispatch_pending(wayland_dpy);
        }
        wl_display_flush(wayland_dpy);

        struct pollfd pfd = { wl_display_get_fd(wayland_dpy), POLLIN, 0 };
        if(poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            wl_display_read_events(wayland_dpy);
            wl_display_dispatch_pending(wayland_dpy);
        } else {
            wl_display_cancel_read(wayland_dpy);
        }

        region_selector->handle_event(nullptr);
    }

    void Overlay::handle_events() {
        if(led_indicator)
            led_indicator->update();

        if(global_hotkeys_ungrab_keyboard) {
            global_hotkeys_ungrab_keyboard = false;
            show_notification(
                TR("Some keyboard remapping software conflicts with GPU Screen Recorder on your system.\n"
                "Keyboards have been ungrabbed, applications will now receive the hotkeys you press.")
                , 7.0, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);

            config.main_config.hotkeys_enable_option = "enable_hotkeys_no_grab";
            save_config(config);
            recreate_global_hotkeys("enable_hotkeys_no_grab");
        }

        if(global_hotkeys)
            global_hotkeys->poll_events();

        if(global_hotkeys_js)
            global_hotkeys_js->poll_events();

        if(cursor_tracker_update_clock.get_elapsed_time_seconds() >= cursor_tracker_update_timeout_sec) {
            cursor_tracker_update_clock.restart();
            if(cursor_tracker)
                cursor_tracker->update();
        }

        desktop_environment->update();
        handle_x11_events();
        handle_wayland_events();

        if(region_selector->take_canceled()) {
            stop_region_selection();
        } else if(region_selector->take_selection() && on_region_selected) {
            switch(region_selector->get_selection_type()) {
                case RegionSelector::SelectionType::NONE: {
                    break;
                }
                case RegionSelector::SelectionType::REGION: {
                    on_region_selected();
                    break;
                }
                case RegionSelector::SelectionType::WINDOW: {
                    Display *display = x11_dpy;

                    const Window selected_window = region_selector->get_window_selection();
                    if(selected_window && selected_window != DefaultRootWindow(display)) {
                        on_region_selected();
                    } else {
                        show_notification(TR("No window selected"), notification_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
                    }
                    break;
                }
            }
            stop_region_selection();
        }

        if(!visible || !window)
            return;

        handle_xi_events();

        while(window->poll_event(event)) {
            if(global_hotkeys) {
                if(!global_hotkeys->on_event(event))
                    continue;
            }
            on_event(event);
        }
    }

    void Overlay::on_event(mgl::Event &event) {
        if(!visible || !window)
            return;


#ifdef GSR_IMGUI
        if(imgui_owns_screen() && imgui_host && imgui_host->is_initialized()) {
            imgui_host->set_display_size(window->get_size().x, window->get_size().y);
            imgui_host->handle_event(event);

            // 鼠标/文本输入由 ImGui 独占，避免同时触发下面看不见的旧控件；
            // 键盘继续放行，保证 Esc、Alt+Z 等既有快捷键仍然生效。
            if(event.type == mgl::Event::MouseMoved || event.type == mgl::Event::MouseButtonPressed ||
                    event.type == mgl::Event::MouseButtonReleased || event.type == mgl::Event::MouseWheelScrolled ||
                    event.type == mgl::Event::TextEntered)
                return;
        }
#endif

        if(!close_button_widget.on_event(event, *window, mgl::vec2f(0.0f, 0.0f)))
            return;

        if(!page_stack.on_event(event, *window, mgl::vec2f(0.0f, 0.0f)))
            return;

        process_key_bindings(event);
    }


#ifdef GSR_IMGUI
    // 把 mgl::Color 打包成 ImGui 的 ImU32（ABGR 字节序，等价于 IM_COL32）
    static unsigned int pack_color(const mgl::Color& color) {
        return (unsigned int)color.r | ((unsigned int)color.g << 8) |
            ((unsigned int)color.b << 16) | ((unsigned int)color.a << 24);
    }

    // 只有停留在最外层主页时才由新界面接管；设置页等仍由旧的 Widget 栈处理
    bool Overlay::imgui_owns_screen() const {
        return imgui_host && imgui_host->is_initialized() && page_stack.size() <= 1;
    }

    void Overlay::draw_imgui_home_page() {
        if(!imgui_host || !imgui_host->is_initialized() || !window)
            return;

        // 只打印一次，方便确认到底是哪套界面在渲染
        static bool reported = false;
        if(!reported) {
            reported = true;
            fprintf(stderr, "Info: ImGui 已接管主页渲染（%.0f x %.0f）\n",
                (double)window->get_size().x, (double)window->get_size().y);
        }

        if(!imgui_theme_applied) {
            // 用现有主题配色，保证与旧界面的观感一致（GNOME 蓝 + 深色底）
            imgui_host->apply_theme(pack_color(get_color_theme().tint_color),
                pack_color(get_color_theme().page_bg_color));
            imgui_theme_applied = true;
        }

        imgui_host->set_display_size(window->get_size().x, window->get_size().y);

        const bool record_active = recording_status == RecordingStatus::RECORD;
        const bool replay_active = recording_status == RecordingStatus::REPLAY;
        const bool stream_active = recording_status == RecordingStatus::STREAM;

        imgui_ui::HomeState state;
        state.target = std::to_string(config.record_config.record_options.fps) + " FPS";

        // 状态与动作文案与旧界面保持一致
        state.record_status = record_active ? (paused ? TR("Paused") : TR("Recording")) : TR("Not recording");
        state.record_action = record_active ? TR("Stop and save") : TR("Start");
        state.record_hotkey = config.record_config.start_stop_hotkey.to_string(false, false);
        state.record_active = record_active;

        state.replay_status = replay_active ? TR("On") : TR("Off");
        state.replay_action = replay_active ? TR("Turn off") : TR("Turn on");
        state.replay_hotkey = config.replay_config.start_stop_hotkey.to_string(false, false);
        state.replay_save_hotkey = config.replay_config.save_hotkey.to_string(false, false);
        state.replay_active = replay_active;

        state.stream_status = stream_active ? TR("Streaming") : TR("Not streaming");
        state.stream_action = stream_active ? TR("Stop") : TR("Start");
        state.stream_hotkey = config.streaming_config.start_stop_hotkey.to_string(false, false);
        state.stream_active = stream_active;

        state.screenshot_hotkey = config.screenshot_config.take_screenshot_hotkey.to_string(false, false);

        imgui_ui::HomeCallbacks callbacks;
        callbacks.record_toggle = [this]() { toggle_record(RecordForceType::NONE); };
        callbacks.record_pause = [this]() { toggle_pause(); };
        callbacks.replay_toggle = [this]() { toggle_replay(); };
        callbacks.replay_save = [this]() { on_press_save_replay(); };
        callbacks.stream_toggle = [this]() { toggle_stream(); };
        callbacks.screenshot_take = [this]() { take_screenshot(); };
        callbacks.open_settings = [this]() { imgui_settings.open(); };

        // 每个模块的独立设置页（录制分辨率/码率等就在这些页面里）—— 走 ImGui 设置框架
        callbacks.record_settings = [this]() {
            imgui_settings.open(gsr::imgui_ui::SettingsUi::Page::Record);
        };
        callbacks.replay_settings = [this]() {
            imgui_settings.open(gsr::imgui_ui::SettingsUi::Page::Replay);
        };
        callbacks.stream_settings = [this]() {
            imgui_settings.open(gsr::imgui_ui::SettingsUi::Page::Stream);
        };
        callbacks.screenshot_settings = [this]() {
            imgui_settings.open(gsr::imgui_ui::SettingsUi::Page::Screenshot);
        };

        // 注意：本函数不再自己开/收 ImGui 帧，帧的所有权由 Overlay::draw() 统一掌管，
        // 这样 toast（draw_notifications）才能和主页画在同一帧、且始终位于最上层。
        // 若以后有人把它单独调用，记得在外面包 begin_frame/end_frame。
        imgui_ui::draw_home_page(state, callbacks);
    }
#endif

    // 前置声明：定义在 grab_mouse_and_keyboard 附近，但 draw() 里要用到。
    static void release_input_grabs(Display *mgl_display, Display *xi_dpy);

    bool Overlay::draw() {
        remove_widgets_to_be_removed();

        if(reload_ui) {
            const bool reopen_settings = reopen_settings_after_reload;
            const int settings_scroll_y = pending_settings_scroll_y;
            reload_ui = false;
            reopen_settings_after_reload = false;
            pending_settings_scroll_y = 0;
            if(visible) {
                recreate_frontpage_ui_components();
                if(reopen_settings)
                    open_settings_page(settings_scroll_y);
            }
        }

        update_notification_process_status();
        process_gsr_game_tracker_output();
        process_gsr_output();
        update_gsr_process_status();
        update_gsr_screenshot_process_status();
        replay_status_update_status();

        update_gsr_game_tracker_replay_status();

        if(hide_ui) {
            hide_ui = false;
#ifdef GSR_IMGUI
            // 隐藏 UI 时若还有待显示/正在淡出的通知，先别销毁窗口 —— 否则内嵌 toast 会一起消失
            // （旧的 gsr-notify 是独立进程，不受影响）。保留窗口把通知画完，再由通知到期逻辑收尾。
            if(gsr::imgui_ui::has_pending_notifications()) {
                notification_only_ = true;
                // 关键：切到通知-only 后不再抓取输入，但之前完整 UI 帧留下的键盘抓取还在，
                // 必须现在释放，否则会一直霸占键盘 → 全局热键失效。
                release_input_grabs((Display*)mgl_get_context()->connection, xi_display);
                // 这个窗口原本是按完整 UI 建的（Wayland 下没做点击穿透），现在只当通知层用，
                // 补上点击穿透，避免 toast 期间挡住游戏点击。
                if(!wayland_native_overlay && window)
                    make_window_click_through((Display*)mgl_get_context()->connection, (Window)window->get_system_handle());
                const auto until = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds((long)((gsr::imgui_ui::notifications_remaining_seconds() + 0.8) * 1000.0));
                if(until > notification_wake_until_)
                    notification_wake_until_ = until;
            } else {
                hide();
                return false;
            }
#else
            hide();
            return false;
#endif
        }

        if(start_region_capture) {
            start_region_capture = false;
            hide();
            rebind_all_keyboard_hotkeys();
            if(!region_selector->start(RegionSelector::SelectionType::REGION, get_color_theme().tint_color)) {
                show_notification(TR("Failed to start region capture"), notification_error_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
                stop_region_selection();
            }
        }

        if(start_window_capture) {
            start_window_capture = false;
            hide();
            rebind_all_keyboard_hotkeys();
            if(!region_selector->start(RegionSelector::SelectionType::WINDOW, get_color_theme().tint_color)) {
                show_notification(TR("Failed to start window capture"), notification_error_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
                stop_region_selection();
            }
        }

        if(region_selector->is_started()) {
            usleep(5 * 1000); // 5 ms
            return true;
        }

        if(!visible)
            return false;

        // 通知-only 窗口没有 page_stack（show(false) 不创建主页），
        // 所以这里不能让"page_stack 为空"直接 hide —— 否则 toast 还没画就被销毁。
        // 另外：即便 UI 被收起（page_stack 空），只要还有待显示/淡出中的通知也先留着窗口。
        if(page_stack.empty() && !notification_only_) {
#ifdef GSR_IMGUI
            if(gsr::imgui_ui::has_pending_notifications()) {
                notification_only_ = true;
                // 关键：切到通知-only 后不再抓取输入，但之前完整 UI 帧留下的键盘抓取还在，
                // 必须现在释放，否则会一直霸占键盘 → 全局热键失效。
                release_input_grabs((Display*)mgl_get_context()->connection, xi_display);
                // 这个窗口原本是按完整 UI 建的（Wayland 下没做点击穿透），现在只当通知层用，
                // 补上点击穿透，避免 toast 期间挡住游戏点击。
                if(!wayland_native_overlay && window)
                    make_window_click_through((Display*)mgl_get_context()->connection, (Window)window->get_system_handle());
                const auto until = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds((long)((gsr::imgui_ui::notifications_remaining_seconds() + 0.8) * 1000.0));
                if(until > notification_wake_until_)
                    notification_wake_until_ = until;
            } else {
                hide();
                return false;
            }
#else
            hide();
            return false;
#endif
        }

        if(!window)
            return false;

        // 通知-only 窗口保持点击穿透，不要抓取输入（否则会抢走游戏的鼠标/键盘）。
        if(!notification_only_)
            grab_mouse_and_keyboard();

        //force_window_on_top();

        const bool draw_ui = show_overlay_clock.get_elapsed_time_seconds() >= show_overlay_timeout_seconds;

        // 通知-only 窗口要保持透明（点击穿透），不要铺底色，也不要画背景截图。
        const mgl::Color clear_color = (notification_only_) ? mgl::Color(0, 0, 0, 0)
                                                             : (draw_ui ? bg_color : mgl::Color(0, 0, 0, 0));
        window->clear(clear_color);

        if(draw_ui) {
            // 通知-only 窗口不要画背景截图（保持透明）
            if(!notification_only_) {
                if(window_texture_sprite.get_texture() && window_texture.texture_id) {
                    window->draw(window_texture_sprite);
                    window->draw(bg_screenshot_overlay);
                } else if(screenshot_texture.is_valid()) {
                    window->draw(screenshot_sprite);
                    window->draw(bg_screenshot_overlay);
                }
            }

            /* 隐藏顶部横幅 */
            // window->draw(top_bar_background);
            // window->draw(top_bar_text);
            // window->draw(logo_sprite);
            // close_button_widget.draw(*window, mgl::vec2f(0.0f, 0.0f));
            // 新的界面层接管最外层主页的绘制；设置页等仍由旧的 Widget 栈绘制
            bool page_drawn = false;
#ifdef GSR_IMGUI
            if(imgui_owns_screen()) {
                imgui_host->set_display_size(window->get_size().x, window->get_size().y);
                imgui_host->begin_frame();
                if(notification_only_) {
                    // 通知-only 模式：只画 toast，不画主界面（保持透明 + 点击穿透）
                } else if(imgui_settings.is_open()) {
                    imgui_settings.draw(config, gsr_info, imgui_settings_callbacks);
                } else {
                    draw_imgui_home_page();
                }
                // 无论哪种模式，toast 都画在最上层（前景绘制层）
                gsr::imgui_ui::draw_notifications();
                imgui_host->end_frame();
                page_drawn = true;
            } else if(imgui_host && imgui_host->is_initialized()) {
                static bool reported_block = false;
                if(!reported_block) {
                    reported_block = true;
                    fprintf(stderr, "Info: 新界面暂不接管（page_stack 深度 %zu > 1，当前显示的是旧界面）\n",
                        page_stack.size());
                }
            }
#endif
            if(!page_drawn)
                page_stack.draw(*window, mgl::vec2f(0.0f, 0.0f));

            draw_tooltip(*window);

            if(cursor_texture.is_valid()) {
                cursor_sprite.set_position((window->get_mouse_position() - cursor_hotspot).to_vec2f());
                window->draw(cursor_sprite);
            }


            if(!drawn_first_frame) {
                drawn_first_frame = true;
                mgl::Event event;
                event.type = mgl::Event::MouseMoved;
                event.mouse_move.x = window->get_mouse_position().x;
                event.mouse_move.y = window->get_mouse_position().y;
                on_event(event);
            }
        }

        // 通知-only 窗口：等 toast 到期、并且灵动岛淡出动画也播完（has_pending 变 false）后，
        // 才收起覆盖层，回到隐藏（透明 + 点击穿透）态。
        if(notification_only_ && std::chrono::steady_clock::now() >= notification_wake_until_
            && !gsr::imgui_ui::has_pending_notifications()) {
            notification_only_ = false;
            hide();
            return false;
        }

        window->display();

        return true;
    }

    // 文件存在且非空（用于判断"产物确实生成了"）。
    static bool file_exists_nonempty(const std::string &path) {
        if(path.empty())
            return false;
        struct stat st;
        return stat(path.c_str(), &st) == 0 && st.st_size > 0;
    }

    // gsr 明确报错的退出码（其余非 0 码多为杂项/被信号终止，产物在就按成功处理）。
    static bool is_explicit_capture_error(int code) {
        return (code >= 50 && code <= 54) || code == 60;
    }

    // 释放键盘/鼠标抓取（但保留窗口）。XGrabKeyboard/XGrabPointer 是在 mgl 的连接上做的，
    // 必须用同一个连接去 ungrab；XI 设备抓取在 xi_display 上。
    // 用途：切到"通知-only"时不能再霸占键盘，否则全局热键会失效。
    static void release_input_grabs(Display *mgl_display, Display *xi_dpy) {
        if(mgl_display) {
            XUngrabKeyboard(mgl_display, CurrentTime);
            XUngrabPointer(mgl_display, CurrentTime);
            XFlush(mgl_display);
        }
        if(xi_dpy)
            xi_ungrab_all_mouse_devices(xi_dpy);
    }

    void Overlay::grab_mouse_and_keyboard() {
        // TODO: Remove these grabs when debugging with a debugger, or your X11 session will appear frozen.
        // There should be a debug mode to not use these
        // Layer-shell takes care of input routing via keyboard_interactivity; X11 grabs
        // don't apply to a wl_egl_window.
        if(wayland_native_overlay)
            return;
        // XGrabPointer/XGrabKeyboard need mgl's X11 connection — the one that owns
        // the mgl window — otherwise the X server rejects with BadWindow.
        Display *display = (Display*)mgl_get_context()->connection;
        XGrabPointer(display, (Window)window->get_system_handle(), True,
            ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
            Button1MotionMask | Button2MotionMask | Button3MotionMask | Button4MotionMask | Button5MotionMask |
            ButtonMotionMask,
            GrabModeAsync, GrabModeAsync, None, default_cursor, CurrentTime);
        // TODO: This breaks global hotkeys (when using x11 global hotkeys)
        XGrabKeyboard(display, (Window)window->get_system_handle(), True, GrabModeAsync, GrabModeAsync, CurrentTime);
        XFlush(display);
    }

    void Overlay::xi_setup_fake_cursor() {
        if(!xi_display)
            return;

        XFixesHideCursor(xi_display, DefaultRootWindow(xi_display));
        XFlush(xi_display);

        // TODO: XCURSOR_SIZE and XCURSOR_THEME environment variables
        const char *cursor_theme = XcursorGetTheme(xi_display);
        if(!cursor_theme) {
            //fprintf(stderr, "Warning: failed to get cursor theme, using \"default\" theme instead\n");
            cursor_theme = "default";
        }

        int cursor_size = XcursorGetDefaultSize(xi_display);
        if(cursor_size <= 1)
            cursor_size = 24;

        XcursorImage *cursor_image = nullptr;
        for(int cursor_size_test : {cursor_size, 24}) {
            for(const char *cursor_theme_test : {cursor_theme, "default", "Adwaita"}) {
                for(unsigned int shape : {XC_left_ptr, XC_arrow}) {
                    cursor_image = XcursorShapeLoadImage(shape, cursor_theme_test, cursor_size_test);
                    if(cursor_image)
                        goto done;
                }
            }
        }

        done:
        if(!cursor_image) {
            fprintf(stderr, "Error: failed to get cursor, loading bundled default cursor instead\n");
            const std::string default_cursor_path = resources_path + "images/default.cur";
            for(int cursor_size_test : {cursor_size, 24}) {
                cursor_image = XcursorFilenameLoadImage(default_cursor_path.c_str(), cursor_size_test);
                if(cursor_image)
                    break;
            }
        }

        if(!cursor_image) {
            fprintf(stderr, "Error: failed to get cursor\n");
            XFixesShowCursor(xi_display, DefaultRootWindow(xi_display));
            XFlush(xi_display);
            return;
        }

        bool cursor_visible = false;
        texture_from_x11_cursor(cursor_image, &cursor_visible, &cursor_hotspot, cursor_texture);
        if(cursor_texture.is_valid())
            cursor_sprite.set_texture(&cursor_texture);

        XcursorImageDestroy(cursor_image);
    }

    void Overlay::show(bool full_ui) {
        if(visible) {
            // 已经显示（可能是通知-only 窗口）。若要求完整 UI，则先收掉通知窗口再完整打开。
            if(full_ui && notification_only_) {
                notification_only_ = false;
                hide();
                // 继续走下面的完整创建流程
            } else {
                return;
            }
        }

        if(region_selector->is_started())
            return;

        drawn_first_frame = false;
        window.reset();
        window = std::make_unique<mgl::Window>();
        deinit_theme();

        Display *display = x11_dpy;

        const std::string wm_name = get_window_manager_name(display);
        const bool is_kwin = wm_name == "KWin";
        const bool is_wlroots = wm_name.find("wlroots") != std::string::npos;
        // On compositors where override-redirect X11 doesn't work and that advertise wlr-layer-shell
        wayland_native_overlay = is_wayland_layer_shell_overlay_session();

        const std::vector<Monitor> monitors = wayland_native_overlay ? get_monitors_wayland(wayland_dpy) : get_monitors(display);
        if(monitors.empty()) {
            fprintf(stderr, "gsr warning: no monitors found, not showing overlay\n");
            window.reset();
            return;
        }

        std::optional<CursorInfo> cursor_info;
        if(cursor_tracker) {
            cursor_tracker->update();
            cursor_info = cursor_tracker->get_latest_cursor_info();
        }

        // The cursor position is wrong on wayland if an x11 window is not focused. On wayland we instead create a window and get the position where the wayland compositor puts it
        Window x11_cursor_window = None;
        mgl::vec2i cursor_position = get_cursor_position(display, &x11_cursor_window);
        const Monitor *focused_monitor = nullptr;
        if(cursor_info) {
            focused_monitor = find_monitor_by_name(monitors, cursor_info->monitor_name);
            if(!focused_monitor)
                focused_monitor = &monitors.front();
        } else {
            const mgl::vec2i monitor_position_query_value = (x11_cursor_window || gsr_info.system_info.display_server != DisplayServer::WAYLAND) ? cursor_position : create_window_get_center_position(display);
            focused_monitor = find_monitor_at_position(monitors, monitor_position_query_value);
            if(!focused_monitor)
                focused_monitor = &monitors.front();
        }

        // Wayland doesn't allow XGrabPointer/XGrabKeyboard when a wayland application is focused.
        // If the focused window is a wayland application then don't use override redirect and instead create
        // a fullscreen window for the ui.
        const Window x11_focused_window = get_focused_window(display, WindowCaptureType::FOCUSED, false);
        const bool prevent_game_minimizing = gsr_info.system_info.display_server != DisplayServer::WAYLAND
            || (x11_focused_window && is_window_fullscreen_on_monitor(display, x11_focused_window, *focused_monitor))
            || is_wlroots
            || wayland_native_overlay;

        const bool drm_cursor_pos = (!prevent_game_minimizing || is_wlroots) && cursor_info;
        if(drm_cursor_pos)
            cursor_position = cursor_info->position;

        if(prevent_game_minimizing || wayland_native_overlay) {
            window_pos = focused_monitor->position;
            window_size = focused_monitor->size;
        } else {
            window_pos = {0, 0};
            window_size = focused_monitor->size / 2;
        }

        // 界面缩放按"显示器高度"定，与窗口尺寸解耦（通知窗口是小窗，不能拿它的 DisplaySize 估）。
        const float monitor_ui_scale = std::min(std::max((float)focused_monitor->size.y / 1080.0f, 0.75f), 2.0f);
        gsr::imgui_ui::set_ui_scale(monitor_ui_scale);

        // 通知-only：不要铺满全屏，只在显示器顶部中间开一条小窗来画灵动岛。
        const bool notification_window = !full_ui && !wayland_native_overlay;
        if(notification_window) {
            const int nw = std::min(focused_monitor->size.x, (int)(760.0f * monitor_ui_scale));
            const int nh = (int)(220.0f * monitor_ui_scale);   // 留足空间容纳多行换行文本
            window_size = mgl::vec2i(nw, nh);
            window_pos = mgl::vec2i(focused_monitor->position.x + (focused_monitor->size.x - nw) / 2,
                                    focused_monitor->position.y + (int)(14.0f * monitor_ui_scale));
        }

        mgl::Window::CreateParams window_create_params;
        window_create_params.size = wayland_native_overlay ? mgl::vec2i(0, 0) : window_size;
        if(prevent_game_minimizing) {
            window_create_params.min_size = window_size;
            window_create_params.max_size = window_size;
        }
        window_create_params.position = focused_monitor->position + focused_monitor->size / 2 - window_size / 2;
        window_create_params.hidden = true;
        window_create_params.override_redirect = prevent_game_minimizing;
        window_create_params.background_color = mgl::Color(0, 0, 0, 0);
        window_create_params.support_alpha = true;
        window_create_params.hide_decorations = true;

        if(notification_window) {
            // 小窗：固定尺寸、定位到顶部中间、override-redirect（浮在游戏上、不抢焦点、不进任务栏）
            window_create_params.size = window_size;
            window_create_params.min_size = window_size;
            window_create_params.max_size = window_size;
            window_create_params.position = window_pos;
            window_create_params.override_redirect = true;
        }
        if(wayland_native_overlay) {
            // wlr-layer-shell OVERLAY surface — full-monitor, no decorations, native Wayland.
            window_create_params.window_type = MGL_WINDOW_TYPE_OVERLAY;
            window_create_params.layer_shell_options.keyboard_interactivity = MGL_LAYER_SHELL_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
            window_create_params.layer_shell_options.exclusive_zone = -1;
        } else {
            // MGL_WINDOW_TYPE_DIALOG is needed for kde plasma wayland in some cases, otherwise the window will pop up on another activity
            // or may not be visible at all
            window_create_params.window_type = (is_kwin && gsr_info.system_info.display_server == DisplayServer::WAYLAND) ? MGL_WINDOW_TYPE_DIALOG : MGL_WINDOW_TYPE_NORMAL;
        }
        // Nvidia + Wayland + Egl doesn't work on some systems properly and it instead falls back to software rendering.
        // Use Glx on Wayland to workaround this issue. This is fine since Egl is only needed for x11 to reliably get the texture of the fullscreen window on Nvidia
        // when a compositor isn't running.
        // Layer-shell requires a native Wayland surface, which means EGL — the GLX workaround above doesn't apply to it.
        if(wayland_native_overlay)
            window_create_params.graphics_api = MGL_GRAPHICS_API_EGL;
        else
            window_create_params.graphics_api = gsr_info.system_info.display_server == DisplayServer::WAYLAND ? MGL_GRAPHICS_API_GLX : MGL_GRAPHICS_API_EGL;
        window_create_params.class_name = "gsr-ui";

        if(!window->create("gsr ui", window_create_params)) {
            fprintf(stderr, "error: failed to create window\n");
            window.reset();
            return;
        }
        //window->set_low_latency(true);

        if(!wayland_native_overlay) {
            // Properties set on the mgl window must use mgl's X11 connection.
            Display *mgl_display = (Display*)mgl_get_context()->connection;
            unsigned char data = 2; // Prefer being composed to allow transparency
            XChangeProperty(mgl_display, (Window)window->get_system_handle(), XInternAtom(mgl_display, "_NET_WM_BYPASS_COMPOSITOR", False), XA_CARDINAL, 32, PropModeReplace, &data, 1);

            data = 1;
            XChangeProperty(mgl_display, (Window)window->get_system_handle(), XInternAtom(mgl_display, "GAMESCOPE_EXTERNAL_OVERLAY", False), XA_CARDINAL, 32, PropModeReplace, &data, 1);
        }

        const auto original_window_size = window_size;
        window_pos = focused_monitor->position;
        window_size = wayland_native_overlay ? window->get_size() : focused_monitor->size;
        if(!init_theme(resources_path)) {
            fprintf(stderr, "Error: failed to load theme\n");
            window.reset();
            return;
        }
        get_theme().set_window_size(window_size);

        // 通知小窗要保持自己的尺寸/位置，别被"全屏/居中"逻辑覆盖。
        if(full_ui && prevent_game_minimizing && !wayland_native_overlay) {
            window->set_size(window_size);
            window->set_size_limits(window_size, window_size);
        }
        if(full_ui && !wayland_native_overlay)
            window->set_position(focused_monitor->position + focused_monitor->size / 2 - original_window_size / 2);

        mgl_window *win = window->internal_window();
        win->cursor_position.x = cursor_position.x - window_pos.x;
        win->cursor_position.y = cursor_position.y - window_pos.y;

        if(full_ui)
            update_compositor_texture(*focused_monitor);

        visible = true;
        if(full_ui)
            recreate_frontpage_ui_components();

        // The focused application can be an xwayland application but the cursor can hover over a wayland application.
        // This is even the case when hovering over the titlebar of the xwayland application.
        const bool fake_cursor = !wayland_native_overlay && (is_wlroots ? x11_cursor_window != None : prevent_game_minimizing);
        if(full_ui && fake_cursor)
            xi_setup();

        // 完整 UI 沿用原条件；通知-only 窗口是铺满整个显示器的透明窗口，必须点击穿透，
        // 否则在 Wayland/XWayland 下会挡住游戏点击 ~1.5s（XWayland 窗口同样支持 input shape）。
        if(!wayland_native_overlay
            && (gsr_info.system_info.display_server == DisplayServer::X11 || !full_ui))
            make_window_click_through((Display*)mgl_get_context()->connection, (Window)window->get_system_handle());

        if(full_ui && !is_wlroots)
            window->set_fullscreen(true);

        window->set_visible(true);
        if(!wayland_native_overlay && !prevent_game_minimizing)
            wait_until_window_viewable((Display*)mgl_get_context()->connection, (Window)window->get_system_handle(), 0.5);

        if(!wayland_native_overlay) {
            // All ops below operate on the mgl window — use mgl's X11 connection.
            Display *mgl_display = (Display*)mgl_get_context()->connection;
            make_window_sticky(mgl_display, (Window)window->get_system_handle());
            hide_window_from_taskbar(mgl_display, (Window)window->get_system_handle());

            if(default_cursor) {
                XFreeCursor(mgl_display, default_cursor);
                default_cursor = 0;
            }
            default_cursor = XCreateFontCursor(mgl_display, XC_left_ptr);
            XFlush(mgl_display);
        }

        if(full_ui)
            grab_mouse_and_keyboard();

        // The real cursor doesn't move when all devices are grabbed, so we create our own cursor and diplay that while grabbed
        cursor_hotspot = {0, 0};
        if(full_ui) {
            xi_setup_fake_cursor();
            if(drm_cursor_pos) {
                win->cursor_position.x += cursor_hotspot.x;
                win->cursor_position.y += cursor_hotspot.y;
            }

            // We want to grab all devices to prevent any other application below the UI from receiving events.
            // Owlboy seems to use xi events and XGrabPointer doesn't prevent owlboy from receiving events.
            xi_grab_all_mouse_devices(xi_display);
        }

        show_overlay_timeout_seconds = 0.0;
        show_overlay_clock.restart();

        // Dumb wayland glitch workaround when fullscreening window
        if(!wayland_native_overlay && !prevent_game_minimizing) {
            mgl::Clock resize_wait_clock;
            while(resize_wait_clock.get_elapsed_time_seconds() < 0.5) {
                const mgl::vec2i current_window_size = window->get_size();
                if(current_window_size.x == window_size.x && current_window_size.y == window_size.y)
                    break;
                handle_events();
                if(!visible || !window)
                    return;
                usleep(1000);
            }
        }

        draw();
    }

    void Overlay::recreate_global_hotkeys(std::string_view hotkey_option) {
        global_hotkeys.reset();
        if(hotkey_option == "enable_hotkeys")
            global_hotkeys = register_linux_hotkeys(this, x11_dpy, GlobalHotkeysLinux::GrabType::ALL, on_region_selected != nullptr);
        else if(hotkey_option == "enable_hotkeys_virtual_devices")
            global_hotkeys = register_linux_hotkeys(this, x11_dpy, GlobalHotkeysLinux::GrabType::VIRTUAL, on_region_selected != nullptr);
        else if(hotkey_option == "enable_hotkeys_no_grab")
            global_hotkeys = register_linux_hotkeys(this, x11_dpy, GlobalHotkeysLinux::GrabType::NO_GRAB, on_region_selected != nullptr);
        else if(hotkey_option == "disable_hotkeys")
            global_hotkeys.reset();
    }

    void Overlay::update_led_indicator_after_settings_change() {
        if(config.record_config.record_options.use_led_indicator || config.replay_config.record_options.use_led_indicator || config.streaming_config.record_options.use_led_indicator || config.screenshot_config.use_led_indicator) {
            if(!led_indicator)
                led_indicator = std::make_unique<LedIndicator>();
        } else {
            led_indicator.reset();
        }
    }

    static const char* get_first_usable_hardware_video_codec_name(const GsrInfo &gsr_info) {
        if(gsr_info.supported_video_codecs.h264)
            return "h264";
        else if(gsr_info.supported_video_codecs.hevc)
            return "hevc";
        else if(gsr_info.supported_video_codecs.av1)
            return "av1";
        else if(gsr_info.supported_video_codecs.vp8)
            return "vp8";
        else if(gsr_info.supported_video_codecs.vp9)
            return "vp9";
        return nullptr;
    }

    static std::unique_ptr<Widget> create_cpu_encoding_warning(const GsrInfo &gsr_info) {
        auto list = std::make_unique<List>(List::Orientation::HORIZONTAL);
        list->set_spacing(0.005f);
        list->set_visible(get_first_usable_hardware_video_codec_name(gsr_info) == nullptr);

        const int font_character_size = 2.0f*mgl::Text::get_font_size_from_font_description(get_theme().body_font_desc.c_str());
        list->add_widget(std::make_unique<Image>(&get_theme().warning_texture, mgl::vec2f(font_character_size, font_character_size), Image::ScaleBehavior::SCALE));
        auto label = std::make_unique<Label>(get_theme().body_font_desc.c_str(), TR("Only CPU encoding is available on your system. Either your system doesn't support GPU encoding or you haven't installed the proper video encoding drivers."), get_color_theme().text_color);
        label->set_wrap_width(label->get_font_size() * 55);
        list->add_widget(std::move(label));

        return list;
    }

    void Overlay::recreate_frontpage_ui_components() {
        bg_screenshot_overlay = mgl::Rectangle(mgl::vec2f(get_theme().window_width, get_theme().window_height));
        top_bar_background = mgl::Rectangle(mgl::vec2f(get_theme().window_width, get_theme().window_height*0.06f).floor());
        top_bar_text = mgl::Text("GPU Screen Recorder", get_theme().top_bar_font_desc.c_str());
        logo_sprite = mgl::Sprite(&get_theme().logo_texture);
        close_button_widget.set_size(mgl::vec2f(top_bar_background.get_size().y * 0.35f, top_bar_background.get_size().y * 0.35f).floor());

        bg_screenshot_overlay.set_color(bg_color);
        top_bar_background.set_color(mgl::Color(0, 0, 0, 180));
        //top_bar_text.set_color(get_color_theme().tint_color);
        top_bar_text.set_position((top_bar_background.get_position() + top_bar_background.get_size()*0.5f - top_bar_text.get_bounds().size*0.5f).floor());

        logo_sprite.set_height((int)(top_bar_background.get_size().y * 0.65f));
        logo_sprite.set_position(mgl::vec2f(
            (top_bar_background.get_size().y - logo_sprite.get_size().y) * 0.5f,
            top_bar_background.get_size().y * 0.5f - logo_sprite.get_size().y * 0.5f
        ).floor());

        close_button_widget.set_position(mgl::vec2f(get_theme().window_width - close_button_widget.get_size().x - logo_sprite.get_position().x, top_bar_background.get_size().y * 0.5f - close_button_widget.get_size().y * 0.5f).floor());

        while(!page_stack.empty()) {
            page_stack.pop();
        }

        auto front_page = std::make_unique<StaticPage>(window_size.to_vec2f());
        StaticPage *front_page_ptr = front_page.get();
        page_stack.push(std::move(front_page));

        const int button_height = window_size.y / 5.0f;
        const int button_width = button_height;

        auto main_buttons_list = std::make_unique<List>(List::Orientation::HORIZONTAL);
        List * main_buttons_list_ptr = main_buttons_list.get();
        main_buttons_list->set_spacing(0.0f);
        {
            auto button = std::make_unique<DropdownButton>(get_theme().title_font_desc.c_str(), get_theme().body_font_desc.c_str(), TR("Instant Replay"), TR("Off"), &get_theme().replay_button_texture,
                mgl::vec2f(button_width, button_height));
            replay_dropdown_button_ptr = button.get();
            button->add_item(TR("Turn on"), "start", config.replay_config.start_stop_hotkey.to_string(false, false));
            button->add_item(TR("Save"), "save", config.replay_config.save_hotkey.to_string(false, false));
            button->add_item(TR("Save 1 min"), "save_1_min", config.replay_config.save_1_min_hotkey.to_string(false, false));
            button->add_item(TR("Save 10 min"), "save_10_min", config.replay_config.save_10_min_hotkey.to_string(false, false));
            button->add_item(TR("Settings"), "settings");
            button->set_item_icon("start", &get_theme().play_texture);
            button->set_item_icon("save", &get_theme().save_texture);
            button->set_item_icon("save_1_min", &get_theme().save_texture);
            button->set_item_icon("save_10_min", &get_theme().save_texture);
            button->set_item_icon("settings", &get_theme().settings_extra_small_texture);
            button->on_click = [this](const std::string &id) {
                if(id == "settings") {
                    auto replay_settings_page = std::make_unique<SettingsPage>(SettingsPage::Type::REPLAY, &gsr_info, config, &page_stack, supports_window_title);
                    replay_settings_page->on_config_changed = [this]() {
                        replay_startup_mode = replay_startup_string_to_type(config.replay_config.turn_on_replay_automatically_mode.c_str());
                        if(recording_status == RecordingStatus::REPLAY)
                            show_notification(TR("Replay settings have been modified. You may need to restart replay to apply the changes."), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::REPLAY);
                    };
                    page_stack.push(std::move(replay_settings_page));
                } else if(id == "save") {
                    on_press_save_replay();
                } else if(id == "save_1_min") {
                    on_press_save_replay_1_min_replay();
                } else if(id == "save_10_min") {
                    on_press_save_replay_10_min_replay();
                } else if(id == "start") {
                    on_press_start_replay(false, false, true);
                }
            };
            button->set_item_enabled("save", false);
            button->set_item_enabled("save_1_min", false);
            button->set_item_enabled("save_10_min", false);
            main_buttons_list->add_widget(std::move(button));
        }
        {
            auto button = std::make_unique<DropdownButton>(get_theme().title_font_desc.c_str(), get_theme().body_font_desc.c_str(), TR("Record"), TR("Not recording"), &get_theme().record_button_texture,
                mgl::vec2f(button_width, button_height));
            record_dropdown_button_ptr = button.get();
            button->add_item(TR("Start"), "start", config.record_config.start_stop_hotkey.to_string(false, false));
            button->add_item(TR("Pause"), "pause", config.record_config.pause_unpause_hotkey.to_string(false, false));
            button->add_item(TR("Settings"), "settings");
            button->set_item_icon("start", &get_theme().play_texture);
            button->set_item_icon("pause", &get_theme().pause_texture);
            button->set_item_icon("settings", &get_theme().settings_extra_small_texture);
            button->on_click = [this](const std::string &id) {
                if(id == "settings") {
                    auto record_settings_page = std::make_unique<SettingsPage>(SettingsPage::Type::RECORD, &gsr_info, config, &page_stack, supports_window_title);
                    record_settings_page->on_config_changed = [this]() {
                        if(recording_status == RecordingStatus::RECORD)
                            show_notification(TR("Recording settings have been modified. You may need to restart recording to apply the changes."), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::RECORD);

                        update_led_indicator_after_settings_change();
                    };
                    page_stack.push(std::move(record_settings_page));
                } else if(id == "pause") {
                    toggle_pause();
                } else if(id == "start") {
                    on_press_start_record(false, RecordForceType::NONE);
                }
            };
            button->set_item_enabled("pause", false);
            main_buttons_list->add_widget(std::move(button));
        }
        {
            auto button = std::make_unique<DropdownButton>(get_theme().title_font_desc.c_str(), get_theme().body_font_desc.c_str(), TR("Livestream"), TR("Not streaming"), &get_theme().stream_button_texture,
                mgl::vec2f(button_width, button_height));
            stream_dropdown_button_ptr = button.get();
            button->add_item(TR("Start"), "start", config.streaming_config.start_stop_hotkey.to_string(false, false));
            button->add_item(TR("Settings"), "settings");
            button->set_item_icon("start", &get_theme().play_texture);
            button->set_item_icon("settings", &get_theme().settings_extra_small_texture);
            button->on_click = [this](const std::string &id) {
                if(id == "settings") {
                    auto stream_settings_page = std::make_unique<SettingsPage>(SettingsPage::Type::STREAM, &gsr_info, config, &page_stack, supports_window_title);
                    stream_settings_page->on_config_changed = [this]() {
                        if(recording_status == RecordingStatus::STREAM)
                            show_notification(TR("Streaming settings have been modified. You may need to restart streaming to apply the changes."), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::STREAM);

                        update_led_indicator_after_settings_change();
                    };
                    page_stack.push(std::move(stream_settings_page));
                } else if(id == "start") {
                    on_press_start_stream(false);
                }
            };
            main_buttons_list->add_widget(std::move(button));
        }

        const mgl::vec2f main_buttons_list_size = main_buttons_list->get_size();
        main_buttons_list->set_position((mgl::vec2f(window_size.x * 0.5f, window_size.y * 0.25f) - main_buttons_list_size * 0.5f).floor());
        front_page_ptr->add_widget(std::move(main_buttons_list));

        {
            const mgl::vec2f main_buttons_size = main_buttons_list_ptr->get_size();
            const int settings_button_size = main_buttons_size.y * 0.33f;
            auto button = std::make_unique<Button>(get_theme().title_font_desc.c_str(), "", mgl::vec2f(settings_button_size, settings_button_size), mgl::Color(0, 0, 0, 180));
            button->set_position((main_buttons_list_ptr->get_position() + main_buttons_size - mgl::vec2f(0.0f, settings_button_size) + mgl::vec2f(settings_button_size * 0.333f, 0.0f)).floor());
            button->set_bg_hover_color(mgl::Color(0, 0, 0, 255));
            button->set_icon(&get_theme().settings_small_texture);
            button->on_click = [this]() {
                open_settings_page();
            };
            front_page_ptr->add_widget(std::move(button));
        }

        {
            const mgl::vec2f main_buttons_size = main_buttons_list_ptr->get_size();
            const int settings_button_size = main_buttons_size.y * 0.33f;
            auto button = std::make_unique<Button>(get_theme().title_font_desc.c_str(), "", mgl::vec2f(settings_button_size, settings_button_size), mgl::Color(0, 0, 0, 180));
            button->set_position((main_buttons_list_ptr->get_position() + main_buttons_size - mgl::vec2f(0.0f, settings_button_size*2) + mgl::vec2f(settings_button_size * 0.333f, 0.0f)).floor());
            button->set_bg_hover_color(mgl::Color(0, 0, 0, 255));
            button->set_icon(&get_theme().screenshot_texture);
            button->set_icon_padding_scale(1.2f);
            button->on_click = [&]() {
                const bool properly_supports_clipboard_image = gsr_info.system_info.display_server == DisplayServer::X11 || wayland_native_clipboard;
                auto screenshot_settings_page = std::make_unique<ScreenshotSettingsPage>(&gsr_info, config, &page_stack, supports_window_title, properly_supports_clipboard_image);
                screenshot_settings_page->on_config_changed = [this]() {
                    update_led_indicator_after_settings_change();
                };
                page_stack.push(std::move(screenshot_settings_page));
            };
            front_page_ptr->add_widget(std::move(button));
        }

        close_button_widget.draw_handler = [&](mgl::Window &window, mgl::vec2f pos, mgl::vec2f size) {
            const int border_size = std::max(1.0f, 0.0015f * get_theme().window_height);
            const float padding_size = std::max(1.0f, 0.003f * get_theme().window_height);
            const mgl::vec2f padding(padding_size, padding_size);
            if(mgl::FloatRect(pos, size).contains(window.get_mouse_position().to_vec2f()))
                draw_rectangle_outline(window, pos.floor(), size.floor(), get_color_theme().tint_color, border_size);

            mgl::Sprite close_sprite(&get_theme().close_texture);
            close_sprite.set_position(pos + padding);
            close_sprite.set_size(size - padding * 2.0f);
            window.draw(close_sprite);
        };

        close_button_widget.event_handler = [&](mgl::Event &event, mgl::Window&, mgl::vec2f pos, mgl::vec2f size) {
            if(event.type == mgl::Event::MouseButtonPressed && event.mouse_button.button == mgl::Mouse::Left) {
                close_button_pressed_inside = mgl::FloatRect(pos, size).contains(mgl::vec2f(event.mouse_button.x, event.mouse_button.y));
            } else if(event.type == mgl::Event::MouseButtonReleased && event.mouse_button.button == mgl::Mouse::Left && close_button_pressed_inside) {
                if(mgl::FloatRect(pos, size).contains(mgl::vec2f(event.mouse_button.x, event.mouse_button.y))) {
                    while(!page_stack.empty()) {
                        page_stack.pop();
                    }
                    return false;
                }
            }
            return true;
        };

        auto cpu_encoding_warning_widget = create_cpu_encoding_warning(gsr_info);
        const auto cpu_encoding_warning_size = cpu_encoding_warning_widget->get_size();
        cpu_encoding_warning_widget->set_position(mgl::vec2f(window_size.x * 0.5f - cpu_encoding_warning_size.x * 0.5f, window_size.y - cpu_encoding_warning_size.y - 2.0f*mgl::Text::get_font_size_from_font_description(get_theme().body_font_desc.c_str())).floor());
        front_page_ptr->add_widget(std::move(cpu_encoding_warning_widget));

        if(gpu_screen_recorder_process > 0) {
            switch(recording_status) {
                case RecordingStatus::NONE:
                    break;
                case RecordingStatus::REPLAY:
                    update_ui_replay_started();
                    break;
                case RecordingStatus::RECORD:
                    update_ui_recording_started();
                    break;
                case RecordingStatus::STREAM:
                    update_ui_streaming_started();
                    break;
            }
        }

        if(paused)
            update_ui_recording_paused();

        if(replay_recording)
            update_ui_recording_started();
    }

    void Overlay::open_settings_page(int scroll_y) {
        auto settings_page = std::make_unique<GlobalSettingsPage>(this, &gsr_info, config, &page_stack);
        settings_page->set_scroll_y(scroll_y);

        settings_page->on_startup_changed = [this](bool enable, int exit_status) {
            if(exit_status == 0)
                return;

            if(exit_status == 67) {
                const bool is_flatpak = getenv("FLATPAK_ID") != nullptr;
                const char *startup_command = is_flatpak ? "flatpak run com.dec05eba.gpu_screen_recorder gsr-ui" : "gsr-ui launch-daemon";
                show_notification(
                    TRF("To enable autorun: install and configure 'dex' (recommended), or manually add '%s' to your desktop autostart entries.", startup_command).c_str(),
                    10.0,
                    mgl::Color(255, 255, 255),
                    mgl::Color(255, 0, 0),
                    NotificationType::NOTICE,
                    nullptr,
                    NotificationLevel::ERROR
                );
                return;
            }

            if(enable)
                show_notification(TR("Failed to add GPU Screen Recorder to system startup"), notification_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
            else
                show_notification(TR("Failed to remove GPU Screen Recorder from system startup"), notification_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
        };

        settings_page->on_click_exit_program_button = [this](std::string_view reason) {
            do_exit = true;
            exit_reason = reason;
        };

        settings_page->on_keyboard_hotkey_changed = [this](std::string_view hotkey_option) {
            recreate_global_hotkeys(hotkey_option);
        };

        settings_page->on_joystick_hotkey_changed = [this](std::string_view hotkey_option) {
            global_hotkeys_js.reset();
            if(hotkey_option == "enable_hotkeys")
                global_hotkeys_js = register_joystick_hotkeys(this);
            else if(hotkey_option == "disable_hotkeys")
                global_hotkeys_js.reset();
        };

        settings_page->on_page_closed = [this]() {
            replay_dropdown_button_ptr->set_item_description("start", config.replay_config.start_stop_hotkey.to_string(false, false));
            replay_dropdown_button_ptr->set_item_description("save", config.replay_config.save_hotkey.to_string(false, false));
            replay_dropdown_button_ptr->set_item_description("save_1_min", config.replay_config.save_1_min_hotkey.to_string(false, false));
            replay_dropdown_button_ptr->set_item_description("save_10_min", config.replay_config.save_10_min_hotkey.to_string(false, false));

            record_dropdown_button_ptr->set_item_description("start", config.record_config.start_stop_hotkey.to_string(false, false));
            record_dropdown_button_ptr->set_item_description("pause", config.record_config.pause_unpause_hotkey.to_string(false, false));

            stream_dropdown_button_ptr->set_item_description("start", config.streaming_config.start_stop_hotkey.to_string(false, false));
        };

        settings_page->on_language_changed = [this](int scroll_y) {
            pending_settings_scroll_y = scroll_y;
            reload_ui = true;
            reopen_settings_after_reload = true;
        };

        page_stack.push(std::move(settings_page));
    }

    void Overlay::hide() {
        if(!visible)
            return;

        hide_ui = false;
        reload_ui = false;
        reopen_settings_after_reload = false;
        pending_settings_scroll_y = 0;

        Display *display = x11_dpy;

        while(!page_stack.empty()) {
            page_stack.pop();
        }
        remove_widgets_to_be_removed();

        if(default_cursor) {
            XFreeCursor(display, default_cursor);
            default_cursor = 0;
        }

        XUngrabKeyboard(display, CurrentTime);
        XUngrabPointer(display, CurrentTime);
        XFlush(display);

        if(xi_display) {
            cursor_texture.clear();
            cursor_sprite.set_texture(nullptr);
        }

        window_texture_deinit(&window_texture);
        window_texture_sprite.set_texture(nullptr);
        screenshot_texture.clear();
        screenshot_sprite.set_texture(nullptr);

        visible = false;
        drawn_first_frame = false;
        start_region_capture = false;
        start_window_capture = false;

        if(xi_input_xev) {
            free(xi_input_xev);
            xi_input_xev = nullptr;
        }

        if(xi_output_xev) {
            free(xi_output_xev);
            xi_output_xev = nullptr;
        }

        if(xi_display) {
            if(window) {
                Display *display = x11_dpy;

                const mgl::vec2i new_cursor_position = mgl::vec2i(window->internal_window()->pos.x, window->internal_window()->pos.y) + window->get_mouse_position();
                XWarpPointer(display, DefaultRootWindow(display), DefaultRootWindow(display), 0, 0, 0, 0, new_cursor_position.x, new_cursor_position.y);
                xi_warp_all_mouse_devices(xi_display, new_cursor_position);
                XFlush(display);

                XFixesShowCursor(display, DefaultRootWindow(display));
                XFlush(display);
            }

            XCloseDisplay(xi_display);
            xi_display = nullptr;
        }

        if(window) {
            if(show_overlay_timeout_seconds > 0.0001) {
                window->clear(mgl::Color(0, 0, 0, 0));
                window->display();

                mgl_context *context = mgl_get_context();
                context->gl.glFlush();
                context->gl.glFinish();
                usleep(50 * 1000); // EGL doesn't do an immediate flush for some reason
            }

            window->set_visible(false);
            window.reset();
        }

        deinit_theme();
#ifdef __GLIBC__
        malloc_trim(0);
#endif
    }

    void Overlay::hide_next_frame() {
        hide_ui = true;
    }

    void Overlay::toggle_show() {
        if(visible) {
            // 若是通知-only 窗口（UI 其实并没有开），Alt+Z 应该把它升级成完整 UI，而不是"关掉"。
            if(notification_only_) {
                show(true);
                return;
            }
            //hide();
            // We dont want to hide immediately because hide is called in mgl event callback, in which it destroys the mgl window.
            // Instead remove all pages and wait until next iteration to close the UI (which happens when there are no pages to render).
            while(!page_stack.empty()) {
                page_stack.pop();
            }
        } else {
            show();
        }
    }

    void Overlay::toggle_record(RecordForceType force_type) {
        on_press_start_record(false, force_type);
    }

    void Overlay::toggle_pause() {
        if(recording_status != RecordingStatus::RECORD || gpu_screen_recorder_process <= 0)
            return;

        kill(gpu_screen_recorder_process, SIGUSR2);
        paused = !paused;

        if(paused) {
            paused_clock.restart();
            update_ui_recording_paused();
            if(config.record_config.record_options.show_notifications)
                show_notification(TR("Recording has been paused"), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::RECORD);
        } else {
            paused_total_time_seconds += paused_clock.get_elapsed_time_seconds();
            update_ui_recording_unpaused();
            if(config.record_config.record_options.show_notifications)
                show_notification(TR("Recording has been unpaused"), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::RECORD);
        }

        if(led_indicator && config.record_config.record_options.use_led_indicator)
            led_indicator->blink();
    }

    void Overlay::update_upause_status() {
        paused = false;
        paused_clock.restart();
        paused_total_time_seconds = 0.0;
    }

    void Overlay::toggle_stream() {
        on_press_start_stream(false);
    }

    void Overlay::toggle_replay() {
        on_press_start_replay(false, false, true);
    }

    void Overlay::save_replay() {
        on_press_save_replay();
    }

    void Overlay::save_replay_1_min() {
        on_press_save_replay_1_min_replay();
    }

    void Overlay::save_replay_10_min() {
        on_press_save_replay_10_min_replay();
    }

    void Overlay::take_screenshot() {
        on_press_take_screenshot(false, ScreenshotForceType::NONE);
    }

    void Overlay::take_screenshot_region() {
        on_press_take_screenshot(false, ScreenshotForceType::REGION);
    }

    void Overlay::take_screenshot_window() {
        on_press_take_screenshot(false, ScreenshotForceType::WINDOW);
    }

    const char* Overlay::notification_type_to_string(NotificationType notification_type) {
        switch(notification_type) {
            case NotificationType::NONE:       return nullptr;
            case NotificationType::RECORD:     return "record";
            case NotificationType::REPLAY:     return "replay";
            case NotificationType::STREAM:     return "stream";
            case NotificationType::SCREENSHOT: return "screenshot";
            case NotificationType::NOTICE:     return gsr_icon_path.c_str();
        }
        return nullptr;
    }

    static void truncate_string(std::string &str, int max_length) {
        int index = 0;
        size_t byte_index = 0;

        while(index < max_length && byte_index < str.size()) {
            uint32_t codepoint = 0;
            size_t codepoint_length = 0;
            mgl::utf8_decode((const unsigned char*)str.c_str() + byte_index, str.size() - byte_index, &codepoint, &codepoint_length);
            if(codepoint_length == 0)
                codepoint_length = 1;

            index += 1;
            byte_index += codepoint_length;
        }

        if(byte_index < str.size()) {
            str.erase(byte_index);
            str += "...";
        }
    }

    static const size_t max_game_name_size_bytes = 200;

    static void truncate_string_bytes(std::string &str, size_t max_size_bytes) {
        if(str.size() <= max_size_bytes)
            return;

        size_t byte_index = 0;
        while(byte_index < str.size()) {
            uint32_t codepoint = 0;
            size_t codepoint_length = 0;
            mgl::utf8_decode((const unsigned char*)str.c_str() + byte_index, str.size() - byte_index, &codepoint, &codepoint_length);
            if(codepoint_length == 0)
                codepoint_length = 1;

            if(byte_index + codepoint_length > max_size_bytes)
                break;

            byte_index += codepoint_length;
        }

        str.erase(byte_index);
    }

    static const char* get_filename_game_name_suffix(const std::string &filename) {
        static const char *prefixes[] = { "Video", "Replay", "Screenshot" };
        for(const char *prefix : prefixes) {
            const size_t prefix_size = strlen(prefix);
            if(filename.size() > prefix_size && filename.compare(0, prefix_size, prefix) == 0 && filename[prefix_size] == '_')
                return filename.c_str() + prefix_size;
        }
        return nullptr;
    }

    static bool is_hex_num(char c) {
        return (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
    }

    static bool contains_non_hex_number(const char *str) {
        bool hex_start = false;
        size_t len = strlen(str);
        if(len >= 2 && memcmp(str, "0x", 2) == 0) {
            str += 2;
            len -= 2;
            hex_start = true;
        }

        bool is_hex = false;
        for(size_t i = 0; i < len; ++i) {
            char c = str[i];
            if(c == '\0')
                return false;
            if(!is_hex_num(c))
                return true;
            if((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
                is_hex = true;
        }

        return is_hex && !hex_start;
    }

    static bool is_number(const char *str) {
        const char *p = str;
        while(*p) {
            char c = *p;
            if(c < '0' || c > '9')
                return false;
            ++p;
        }
        return true;
    }

    static bool is_capture_target_monitor(const char *capture_target) {
        return strcmp(capture_target, "window") != 0 && strcmp(capture_target, "focused") != 0 && strcmp(capture_target, "region") != 0 && strcmp(capture_target, "portal") != 0 && contains_non_hex_number(capture_target);
    }

    static std::string capture_target_get_notification_name(Display *x11_dpy, const char *capture_target, bool save) {
        std::string result;
        if(is_capture_target_monitor(capture_target)) {
            result = TR("this monitor");
        } else if(is_number(capture_target)) {
            int64_t window_id = None;
            sscanf(capture_target, "%" PRIi64, &window_id);

            const std::optional<std::string> window_title = x11_dpy ? get_window_title(x11_dpy, window_id) : std::optional<std::string>();
            if(save) {
                result = TR("window");
            } else if(window_title) {
                result = strip(window_title.value());
                truncate_string(result, 30);
                result = TRF("window \"%s\"", result.c_str());
            } else {
                result = TRF("window %s", capture_target);
            }
        } else {
            result = TR(capture_target);
        }
        return result;
    }

    [[maybe_unused]] static std::string get_valid_monitor_x11(const std::string &target_monitor_name, const std::vector<Monitor> &monitors) {
        std::string target_monitor_name_clean = target_monitor_name;
        if(starts_with(target_monitor_name_clean, "HDMI-A"))
            target_monitor_name_clean.replace(0, 6, "HDMI");

        for(const Monitor &monitor : monitors) {
            std::string monitor_name_clean = monitor.name;
            if(starts_with(monitor_name_clean, "HDMI-A"))
                monitor_name_clean.replace(0, 6, "HDMI");

            if(target_monitor_name_clean == monitor_name_clean)
                return monitor.name;
        }

        return "";
    }

    static std::string get_focused_monitor_by_cursor(Display *x11_dpy, CursorTracker *cursor_tracker, const GsrInfo &gsr_info, const std::vector<Monitor> &x11_monitors) {
        std::optional<CursorInfo> cursor_info;
        if(cursor_tracker) {
            cursor_tracker->update();
            cursor_info = cursor_tracker->get_latest_cursor_info();
        }

        std::string focused_monitor_name;
        if(cursor_info) {
            focused_monitor_name = std::move(cursor_info->monitor_name);
        } else if(x11_dpy) {
            Window x11_cursor_window = None;
            mgl::vec2i cursor_position = get_cursor_position(x11_dpy, &x11_cursor_window);

            const mgl::vec2i monitor_position_query_value = (x11_cursor_window || gsr_info.system_info.display_server != DisplayServer::WAYLAND) ? cursor_position : create_window_get_center_position(x11_dpy);
            const Monitor *focused_monitor = find_monitor_at_position(x11_monitors, monitor_position_query_value);
            if(focused_monitor)
                focused_monitor_name = focused_monitor->name;
        }

        return focused_monitor_name;
    }

    void Overlay::show_notification(const char *str, double timeout_seconds, mgl::Color icon_color, mgl::Color bg_color, NotificationType notification_type, const char *capture_target, NotificationLevel notification_level) {
        if(notification_level != NotificationLevel::ERROR)
            timeout_seconds *= notification_duration_multiplier;

        (void)capture_target;  // 仅在非 ImGui（gsr-notify）回退路径中使用

#ifdef GSR_IMGUI
        // 内嵌通知：推入 ImGui 绘制队列，由 draw() 在 ImGui 帧内显示。
        // 即便 UI 处于隐藏（透明 + 点击穿透）态，也会短暂唤起覆盖层窗口画出 toast，
        // 从而替代原先 spawn 的 gsr-notify 外部进程。
        const double eff = timeout_seconds > 0.0 ? timeout_seconds : 5.0;
        const auto wake = std::chrono::steady_clock::now()
            + std::chrono::milliseconds((long)((eff + 0.8) * 1000));
        if(wake > notification_wake_until_)
            notification_wake_until_ = wake;

        if(!visible) {
            // 覆盖层当前隐藏：重建窗口但不抓取输入（保持点击穿透），只用于显示通知。
            notification_only_ = true;
            show(false);
        }

        gsr::imgui_ui::push_notification(
            str ? str : "",
            eff,
            icon_color,
            bg_color,
            notification_type_to_string(notification_type));
        return;
#else
        char timeout_seconds_str[32];
        snprintf(timeout_seconds_str, sizeof(timeout_seconds_str), "%f", timeout_seconds);

        const std::string icon_color_str = color_to_hex_str(icon_color);
        const std::string bg_color_str = color_to_hex_str(bg_color);
        const char *notification_args[14] = {
            "gsr-notify", "--text", str, "--timeout", timeout_seconds_str,
            "--icon-color", icon_color_str.c_str(), "--bg-color", bg_color_str.c_str(),
        };

        int arg_index = 9;
        const char *notification_type_str = notification_type_to_string(notification_type);
        if(notification_type_str) {
            notification_args[arg_index++] = "--icon";
            notification_args[arg_index++] = notification_type_str;
        }

        Display *display = x11_dpy;

        std::string monitor_name;
        const auto monitors = get_monitors(display);

        if(capture_target && is_capture_target_monitor(capture_target))
            monitor_name = capture_target;
        else
            monitor_name = get_focused_monitor_by_cursor(x11_dpy, cursor_tracker.get(), gsr_info, monitors);

        monitor_name = get_valid_monitor_x11(monitor_name, monitors);
        if(!monitor_name.empty()) {
            notification_args[arg_index++] = "--monitor";
            notification_args[arg_index++] = monitor_name.c_str();
        } else if(!monitors.empty()) {
            notification_args[arg_index++] = "--monitor";
            notification_args[arg_index++] = monitors.front().name.c_str();
        }

        notification_args[arg_index++] = nullptr;

        if(notification_process > 0) {
            kill(notification_process, SIGINT);
            int status = 0;
            waitpid(notification_process, &status, 0);
        }

        notification_process = exec_program(notification_args, NULL);
#endif
    }

    bool Overlay::is_open() const {
        return visible;
    }

    bool Overlay::should_exit(std::string &reason) const {
        reason.clear();
        if(do_exit)
            reason = exit_reason;
        return do_exit;
    }

    void Overlay::exit() {
        do_exit = true;
    }

    void Overlay::go_back_to_old_ui() {
        const bool inside_flatpak = getenv("FLATPAK_ID") != NULL;
        if(inside_flatpak)
            exit_reason = "back-to-old-ui";
        else
            exit_reason = "exit";
        exit();
    }

    void Overlay::cancel_region_selection() {
        if(region_selector)
            region_selector->cancel();
    }

    const Config& Overlay::get_config() const {
        return config;
    }

    void Overlay::reload_config() {
        const SupportedCaptureOptions capture_options = get_supported_capture_options(gsr_info);
        std::optional<Config> new_config = read_config(capture_options);
        if(new_config)
            config = std::move(new_config.value());
    }

    void Overlay::unbind_all_keyboard_hotkeys() {
        if(global_hotkeys)
            global_hotkeys->unbind_all_keys();
    }

    void Overlay::rebind_all_keyboard_hotkeys() {
        unbind_all_keyboard_hotkeys();
        // TODO: Check if type is GlobalHotkeysLinux
        if(global_hotkeys)
            bind_linux_hotkeys(static_cast<GlobalHotkeysLinux*>(global_hotkeys.get()), this, on_region_selected != nullptr);
    }

    void Overlay::set_notification_speed(NotificationSpeed notification_speed) {
        switch(notification_speed) {
            case NotificationSpeed::NORMAL:
                notification_duration_multiplier = 1.0;
                break;
            case NotificationSpeed::FAST:
                notification_duration_multiplier = 0.3;
                break;
        }
    }

    void Overlay::update_notification_process_status() {
        if(notification_process <= 0)
            return;

        int status;
        if(waitpid(notification_process, &status, WNOHANG) == 0) {
            // Still running
            return;
        }

        notification_process = -1;
    }

    static void string_replace_characters(char *str, const char *characters_to_replace, char new_character) {
        for(; *str != '\0'; ++str) {
            for(const char *p = characters_to_replace; *p != '\0'; ++p) {
                if(*str == *p)
                    *str = new_character;
            }
        }
    }

    static std::string filepath_get_directory(const char *filepath) {
        std::string result = filepath;
        const size_t last_slash_index = result.rfind('/');
        if(last_slash_index == std::string::npos)
            result = ".";
        else
            result.erase(last_slash_index);
        return result;
    }

    static std::string filepath_get_filename(const char *filepath) {
        std::string result = filepath;
        const size_t last_slash_index = result.rfind('/');
        if(last_slash_index != std::string::npos)
            result.erase(0, last_slash_index + 1);
        return result;
    }

    static std::string to_duration_string(double duration_sec) {
        int seconds = ceil(duration_sec);

        const int hours = seconds / 60 / 60;
        seconds -= (hours * 60 * 60);

        const int minutes = seconds / 60;
        seconds -= (minutes * 60);

        std::string result;
        if(hours > 0) {
            if (Translation::instance().plural_numbers_are_complex()) {
                result += TRPF("%d hour", hours, hours);
            }
            else {
                if(hours == 1)
                    result += TRF("%d hour", hours);
                else
                    result += TRF("%d hours", hours);
            }
        }

        if(minutes > 0) {
            if(!result.empty())
                result += " ";

            if (Translation::instance().plural_numbers_are_complex()) {
                result += TRPF("%d minute", minutes, minutes);
            }
            else {
                if(minutes == 1)
                    result += TRF("%d minute", minutes);
                else
                    result += TRF("%d minutes", minutes);
            }
        }

        if(seconds > 0 || (hours == 0 && minutes == 0)) {
            if(!result.empty())
                result += " ";

            if (Translation::instance().plural_numbers_are_complex()) {
                result += TRPF("%d second", seconds, seconds);
            }
            else {
                if(seconds == 1)
                    result += TRF("%d second", seconds);
                else
                    result += TRF("%d seconds", seconds);
            }
        }

        return result;
    }

    double Overlay::get_time_passed_in_replay_buffer_seconds() {
        double replay_duration_sec = replay_saved_duration_sec;
        if(replay_duration_sec > current_recording_config.replay_config.replay_time)
            replay_duration_sec = current_recording_config.replay_config.replay_time;
        if(replay_save_duration_min > 0 && replay_duration_sec > replay_save_duration_min * 60)
            replay_duration_sec = replay_save_duration_min * 60;
        return replay_duration_sec;
    }

    static Clipboard::FileType filename_to_clipboard_file_type(const std::string &filename) {
        if(ends_with(filename, ".jpg") || ends_with(filename, ".jpeg"))
            return Clipboard::FileType::JPG;
        else if(ends_with(filename, ".png"))
            return Clipboard::FileType::PNG;
        assert(false);
        return Clipboard::FileType::PNG;
    }

    std::string Overlay::get_current_game_name() {
        std::string focused_window_name = desktop_environment->get_focused_window_title();
        if(focused_window_name.empty())
            focused_window_name = "Game";

        focused_window_name = window_title_utf8_sanitize(focused_window_name.c_str(), focused_window_name.size());
        focused_window_name = strip(focused_window_name);
        // Windows doesn't like these characters, and linux doesn't like "/", which is included there as well
        string_replace_characters(focused_window_name.data(), "<>:\"/\\|?*", ' ');
        truncate_string_bytes(focused_window_name, max_game_name_size_bytes);
        focused_window_name = strip(focused_window_name);
        if(focused_window_name.empty())
            focused_window_name = "Game";
        return focused_window_name;
    }

    void Overlay::rename_file_to_game_name(std::string &filepath, const std::string &game_name) {
        const std::string filename = filepath_get_filename(filepath.c_str());
        const char *filename_suffix = get_filename_game_name_suffix(filename);
        if(!filename_suffix)
            return;

        std::string new_filepath = filepath_get_directory(filepath.c_str()) + "/" + game_name + filename_suffix;
        if(rename(filepath.c_str(), new_filepath.c_str()) == 0)
            filepath = std::move(new_filepath);
    }

    void Overlay::save_video_in_current_game_directory(std::string &video_filepath, const std::string &game_name, NotificationType notification_type) {
        const std::string video_filename = filepath_get_filename(video_filepath.c_str());
        std::string focused_window_name = game_name;

        std::string video_directory = filepath_get_directory(video_filepath.c_str()) + "/" + focused_window_name;
        create_directory_recursive(video_directory.data());

        const std::string new_video_filepath = video_directory + "/" + video_filename;
        rename(video_filepath.c_str(), new_video_filepath.c_str());
        video_filepath = new_video_filepath;

        truncate_string(focused_window_name, 40);
        const char *capture_target = nullptr;
        char msg[512];

        switch(notification_type) {
            case NotificationType::RECORD: {
                if(!config.record_config.record_options.show_notifications)
                    return;

                const std::string duration_str = to_duration_string(recording_duration_clock.get_elapsed_time_seconds() - paused_total_time_seconds - (paused ? paused_clock.get_elapsed_time_seconds() : 0.0));
                snprintf(msg, sizeof(msg), TR("Saved a %s recording of %s to \"%s\""),
                    duration_str.c_str(),
                    capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), true).c_str(), focused_window_name.c_str());
                capture_target = recording_capture_target.c_str();
                break;
            }
            case NotificationType::REPLAY: {
                if(!config.replay_config.record_options.show_notifications)
                    return;

                const std::string duration_str = to_duration_string(get_time_passed_in_replay_buffer_seconds());
                snprintf(msg, sizeof(msg), TR("Saved a %s replay of %s to \"%s\""),
                    duration_str.c_str(),
                    capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), true).c_str(), focused_window_name.c_str());
                capture_target = recording_capture_target.c_str();
                break;
            }
            case NotificationType::SCREENSHOT: {
                if(!config.screenshot_config.show_notifications)
                    return;

                snprintf(msg, sizeof(msg), TR("Saved a screenshot of %s to \"%s\""),
                    capture_target_get_notification_name(x11_dpy, screenshot_capture_target.c_str(), true).c_str(), focused_window_name.c_str());
                capture_target = screenshot_capture_target.c_str();
                break;
            }
            case NotificationType::NONE:
            case NotificationType::STREAM:
            case NotificationType::NOTICE:
                break;
        }
        show_notification(msg, notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, notification_type, capture_target);
    }

    void Overlay::on_replay_saved(const char *replay_saved_filepath) {
        replay_save_show_notification = false;

        std::string filepath = replay_saved_filepath;
        std::string game_name;
        if(config.replay_config.name_video_file_after_game || config.replay_config.save_video_in_game_folder)
            game_name = get_current_game_name();

        if(config.replay_config.name_video_file_after_game)
            rename_file_to_game_name(filepath, game_name);

        if(config.replay_config.save_video_in_game_folder) {
            save_video_in_current_game_directory(filepath, game_name, NotificationType::REPLAY);
        } else if(config.replay_config.record_options.show_notifications) {
            const std::string duration_str = to_duration_string(get_time_passed_in_replay_buffer_seconds());

            char msg[512];
            snprintf(msg, sizeof(msg), TR("Saved a %s replay of %s"),
                duration_str.c_str(),
                capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), true).c_str());
            show_notification(msg, notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::REPLAY, recording_capture_target.c_str());
        }

        if(led_indicator && config.replay_config.record_options.use_led_indicator)
            led_indicator->blink();
    }

    void Overlay::process_gsr_game_tracker_output() {
        char buffer[1024];
        if(gsr_game_tracker_process_output_file) {
            char *line = fgets(buffer, sizeof(buffer), gsr_game_tracker_process_output_file);
            if(!line || line[0] == '\0')
                return;

            if(strncmp(line, "Game launched", 13) == 0) {
                game_replay_action = GameReplayAction::START;
            } else if(strncmp(line, "Game exited", 11) == 0) {
                game_replay_action = GameReplayAction::STOP;
            }

        } else if(gsr_game_tracker_process_output_fd > 0) {
            read(gsr_game_tracker_process_output_fd, buffer, sizeof(buffer));
        }
    }

    void Overlay::process_gsr_output() {
        if(replay_save_show_notification && replay_save_clock.get_elapsed_time_seconds() >= replay_saving_notification_timeout_seconds) {
            replay_save_show_notification = false;
            if(config.replay_config.record_options.show_notifications)
                show_notification(TR("Saving replay, this might take some time"), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::REPLAY);
        }

        char buffer[1024];
        if(gpu_screen_recorder_process_output_file) {
            char *line = fgets(buffer, sizeof(buffer), gpu_screen_recorder_process_output_file);
            if(!line || line[0] == '\0')
                return;

            int line_len = strlen(line);
            if(line[line_len - 1] == '\n') {
                line[line_len - 1] = '\0';
                line_len -= 1;
            }

            const std::string_view line_view{line, (size_t)line_len};
            if(starts_with(line_view, "gsr error: ")) {
                //show_notification(line + 11, notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), recording_status_to_notification_type(recording_status), nullptr, NotificationLevel::ERROR);
                if(replay_recording && ends_with(line_view, "recording")) {
                    std::string dummy;
                    on_stop_recording(1, dummy);
                } else if(recording_status == RecordingStatus::REPLAY && ends_with(line_view, "replay")) {
                    show_notification(TR("Failed to save replay, make sure the replay save directory is mounted and writable"), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::REPLAY, nullptr, NotificationLevel::ERROR);
                    replay_save_show_notification = false;
                }
                return;
            }

            const std::string video_filepath = filepath_get_filename(line);
            if(starts_with(video_filepath, "Video_")) {
                record_filepath = line;
                on_stop_recording(0, record_filepath);
                return;
            }

            switch(recording_status) {
                case RecordingStatus::NONE:
                    break;
                case RecordingStatus::REPLAY:
                    on_replay_saved(line);
                    break;
                case RecordingStatus::RECORD:
                    break;
                case RecordingStatus::STREAM:
                    break;
            }
        } else if(gpu_screen_recorder_process_output_fd > 0) {
            read(gpu_screen_recorder_process_output_fd, buffer, sizeof(buffer));
        }
    }

    void Overlay::on_gsr_process_error(int exit_code, NotificationType notification_type) {
        fprintf(stderr, "Warning: gpu-screen-recorder (%d) exited with exit status %d\n", (int)gpu_screen_recorder_process, exit_code);
        if(exit_code == 50) {
            show_notification(TR("Desktop portal capture failed. Either you canceled the desktop portal or your Wayland compositor doesn't support desktop portal capture or it's incorrectly setup on your system."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        } else if(exit_code == 51) {
            show_notification(TR("Monitor capture failed. The monitor you are trying to capture is invalid. Please validate your capture settings."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        } else if(exit_code == 52) {
            show_notification(TR("Capture failed. Neither H264, HEVC nor AV1 video codecs are supported on your system or you are trying to capture at a resolution higher than your system supports for each video codec."), 10.0, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        } else if(exit_code == 53) {
            show_notification(TR("Capture failed. Your system doesn't support the resolution you are trying to record at with the video codec you have chosen. Change capture resolution or video codec and try again. Note: AV1 supports the highest resolution, then HEVC and then H264."), 10.0, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        } else if(exit_code == 54) {
            show_notification(TR("Capture failed. Your system doesn't support the video codec you have chosen. Change video codec and try again."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        } else if(exit_code == 60) {
            show_notification(TR("Stopped capture because the user canceled the desktop portal"), notification_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        } else {
            const char *prefix = "";
            switch(notification_type) {
                case NotificationType::NONE:
                case NotificationType::NOTICE:
                    break;
                case NotificationType::SCREENSHOT:
                    prefix = TR("Failed to take a screenshot");
                    break;
                case NotificationType::RECORD:
                    prefix = TR("Failed to start/save recording");
                    break;
                case NotificationType::REPLAY:
                    prefix = TR("Replay stopped because of an error");
                    break;
                case NotificationType::STREAM:
                    prefix = TR("Streaming stopped because of an error");
                    break;
            }

            char msg[256];
            snprintf(msg, sizeof(msg), TR("%s. Verify if settings are correct"), prefix);
            show_notification(msg, notification_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        }
    }

    void Overlay::update_gsr_process_status() {
        if(gpu_screen_recorder_process <= 0)
            return;

        int status;
        if(waitpid(gpu_screen_recorder_process, &status, WNOHANG) == 0) {
            // Still running
            return;
        }

        close_gpu_screen_recorder_output();

        int exit_code = -1;
        if(WIFEXITED(status))
            exit_code = WEXITSTATUS(status);

        switch(recording_status) {
            case RecordingStatus::NONE:
                break;
            case RecordingStatus::REPLAY: {
                replay_save_duration_min = 0;
                update_ui_replay_stopped();
                if(exit_code == 0) {
                    if(config.replay_config.record_options.show_notifications)
                        show_notification(TR("Replay stopped"), short_notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::REPLAY);
                } else {
                    on_gsr_process_error(exit_code, NotificationType::REPLAY);
                }

                if(led_indicator)
                    led_indicator->set_led(false);

                break;
            }
            case RecordingStatus::RECORD: {
                update_ui_recording_stopped();
                on_stop_recording(exit_code, record_filepath);

                if(led_indicator)
                    led_indicator->set_led(false);
                break;
            }
            case RecordingStatus::STREAM: {
                update_ui_streaming_stopped();
                if(exit_code == 0) {
                    if(config.streaming_config.record_options.show_notifications)
                        show_notification(TR("Streaming has stopped"), short_notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::STREAM);
                } else {
                    on_gsr_process_error(exit_code, NotificationType::STREAM);
                }

                if(led_indicator)
                    led_indicator->set_led(false);
                break;
            }
        }

        gpu_screen_recorder_process = -1;
        recording_status = RecordingStatus::NONE;
        replay_launched_manually = true;
    }

    void Overlay::update_gsr_screenshot_process_status() {
        if(gpu_screen_recorder_screenshot_process <= 0)
            return;

        int status;
        if(waitpid(gpu_screen_recorder_screenshot_process, &status, WNOHANG) == 0) {
            // Still running
            return;
        }

        int exit_code = -1;
        if(WIFEXITED(status))
            exit_code = WEXITSTATUS(status);
        else if(WIFSIGNALED(status))
            fprintf(stderr, "Warning: gpu-screen-recorder (screenshot) was terminated by signal %d\n", (int)WTERMSIG(status));

        // gsr 反馈非 0（含被信号终止）但截图文件确实存在时，按成功处理，避免"明明截到了却提示失败"。
        if(exit_code != 0 && !is_explicit_capture_error(exit_code) && file_exists_nonempty(screenshot_filepath)) {
            fprintf(stderr, "Warning: gpu-screen-recorder (screenshot) exited with status %d, but the file exists (%s); treating as success\n",
                exit_code, screenshot_filepath.c_str());
            exit_code = 0;
        }

        if(exit_code == 0) {
            const bool name_screenshot_after_game = config.screenshot_config.name_screenshot_file_after_game && config.screenshot_config.save_screenshot_to_disk;
            const bool save_screenshot_in_game_folder = config.screenshot_config.save_screenshot_in_game_folder && config.screenshot_config.save_screenshot_to_disk;

            std::string game_name;
            if(name_screenshot_after_game || save_screenshot_in_game_folder)
                game_name = get_current_game_name();

            if(name_screenshot_after_game)
                rename_file_to_game_name(screenshot_filepath, game_name);

            if(save_screenshot_in_game_folder) {
                save_video_in_current_game_directory(screenshot_filepath, game_name, NotificationType::SCREENSHOT);
            } else if(config.screenshot_config.show_notifications) {
                char msg[512];
                snprintf(msg, sizeof(msg), TR("Saved a screenshot of %s"), capture_target_get_notification_name(x11_dpy, screenshot_capture_target.c_str(), true).c_str());
                show_notification(msg, notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::SCREENSHOT, screenshot_capture_target.c_str());
            }

            if(config.screenshot_config.save_screenshot_to_clipboard && clipboard)
                clipboard->set_current_file(screenshot_filepath, filename_to_clipboard_file_type(screenshot_filepath));

            if(led_indicator && config.screenshot_config.use_led_indicator)
                led_indicator->blink();

            if(!strip(config.screenshot_config.custom_script).empty()) {
                std::stringstream ss;
                ss << config.screenshot_config.custom_script << " " << std::quoted(screenshot_filepath);
                const std::string command = ss.str();
                const char *args[] = { "/bin/sh", "-c", command.c_str(), nullptr };
                exec_program_on_host_daemonized(args);
            }
        } else {
            fprintf(stderr, "Warning: gpu-screen-recorder (%d) exited with exit status %d\n", (int)gpu_screen_recorder_screenshot_process, exit_code);
            show_notification(TR("Failed to take a screenshot. Verify if settings are correct"), notification_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::SCREENSHOT, nullptr, NotificationLevel::ERROR);
        }

        gpu_screen_recorder_screenshot_process = -1;
    }

    void Overlay::replay_status_update_status() {
        if(replay_status_update_clock.get_elapsed_time_seconds() < replay_status_update_check_timeout_seconds)
            return;

        replay_status_update_clock.restart();
        update_power_supply_status();
        update_replay_program_startup_status();
    }

    // TODO: Instead of checking power supply status periodically listen to power supply event
    void Overlay::update_power_supply_status() {
        if(replay_startup_mode == ReplayStartupMode::DONT_TURN_ON_AUTOMATICALLY)
            return;

        power_supply_connected = power_supply_online_filepath.empty() || power_supply_is_connected(power_supply_online_filepath.c_str());
    }

    bool Overlay::replay_program_autostart_capture_options_available() {
        return are_all_audio_tracks_available_to_capture(config.replay_config.record_options.audio_tracks_list) && is_webcam_available_to_capture(config.replay_config.record_options);
    }

    void Overlay::update_replay_program_startup_status() {
        if(replay_startup_mode != ReplayStartupMode::TURN_ON_AT_SYSTEM_STARTUP)
            return;

        if(config.replay_config.only_start_replay_if_power_supply_connected) {
            if(recording_status == RecordingStatus::NONE) {
                if(replay_program_autostart_capture_options_available() || replay_launched_once) {
                    const bool power_supply_connected_status_changed = power_supply_connected != replay_program_startup_power_supply_connected;
                    replay_program_startup_power_supply_connected = power_supply_connected;

                    if(power_supply_connected_status_changed && power_supply_connected)
                        on_press_start_replay(false, false);
                }
            } else if(recording_status == RecordingStatus::REPLAY) {
                const bool power_supply_connected_status_changed = power_supply_connected != replay_program_startup_power_supply_connected;
                replay_program_startup_power_supply_connected = power_supply_connected;

                if(power_supply_connected_status_changed && !power_supply_connected && !replay_launched_manually)
                    on_press_start_replay(false, false);
            }
        } else {
            if(recording_status == RecordingStatus::NONE && !replay_launched_once) {
                if(replay_program_autostart_capture_options_available())
                    on_press_start_replay(false, false);
            }
        }
    }

    void Overlay::update_gsr_game_tracker_replay_status() {
        if(replay_startup_mode != ReplayStartupMode::TURN_ON_AT_GAME_LAUNCH)
            return;

        const bool power_supply_allows_start = !config.replay_config.only_start_replay_if_power_supply_connected || power_supply_connected;
        const bool power_supply_disconnected = config.replay_config.only_start_replay_if_power_supply_connected && !power_supply_connected;

        if(replay_launched_manually)
            game_replay_action = GameReplayAction::IDLE;

        if(recording_status == RecordingStatus::NONE && game_replay_action == GameReplayAction::START && power_supply_allows_start) {
            on_press_start_replay(false, false);
        } else if(recording_status == RecordingStatus::REPLAY && (game_replay_action == GameReplayAction::STOP || power_supply_disconnected)) {
            on_press_start_replay(false, false);
        }

        game_replay_action = GameReplayAction::IDLE;
    }

    void Overlay::on_stop_recording(int exit_code, std::string &video_filepath) {
        if(recording_status != RecordingStatus::RECORD && recording_status != RecordingStatus::REPLAY)
            return;

        // gsr 反馈非 0（含被信号终止）但产物文件确实存在时，按成功处理，
        // 避免"明明录成功了却提示失败"（Wayland 下 gsr 有时会返回杂项错误码）。
        if(exit_code != 0 && !is_explicit_capture_error(exit_code) && file_exists_nonempty(video_filepath)) {
            fprintf(stderr, "Warning: gpu-screen-recorder exited with status %d, but the recording file exists (%s); treating as success\n",
                exit_code, video_filepath.c_str());
            exit_code = 0;
        }

        if(exit_code == 0) {
            std::string game_name;
            if(config.record_config.name_video_file_after_game || config.record_config.save_video_in_game_folder)
                game_name = get_current_game_name();

            if(config.record_config.name_video_file_after_game)
                rename_file_to_game_name(video_filepath, game_name);

            if(config.record_config.save_video_in_game_folder) {
                save_video_in_current_game_directory(video_filepath, game_name, NotificationType::RECORD);
            } else if(config.record_config.record_options.show_notifications) {
                const std::string duration_str = to_duration_string(recording_duration_clock.get_elapsed_time_seconds() - paused_total_time_seconds - (paused ? paused_clock.get_elapsed_time_seconds() : 0.0));

                char msg[512];
                snprintf(msg, sizeof(msg), TR("Saved a %s recording of %s"),
                    duration_str.c_str(),
                    capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), true).c_str());
                show_notification(msg, notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::RECORD, recording_capture_target.c_str());
            }

            if(led_indicator) {
                if(recording_status == RecordingStatus::REPLAY && !current_recording_config.replay_config.record_options.use_led_indicator)
                    led_indicator->set_led(false);
                else if(recording_status == RecordingStatus::STREAM && !current_recording_config.streaming_config.record_options.use_led_indicator)
                    led_indicator->set_led(false);
                else if(config.record_config.record_options.use_led_indicator)
                    led_indicator->blink();
            }
        } else {
            on_gsr_process_error(exit_code, NotificationType::RECORD);
        }

        update_ui_recording_stopped();
        replay_recording = false;
    }

    void Overlay::update_ui_recording_paused() {
        if(!visible || notification_only_ || !record_dropdown_button_ptr || recording_status != RecordingStatus::RECORD)
            return;

        record_dropdown_button_ptr->set_description(TR("Paused"));
        record_dropdown_button_ptr->set_item_label("pause", TR("Unpause"));
        record_dropdown_button_ptr->set_item_icon("pause", &get_theme().play_texture);
    }

    void Overlay::update_ui_recording_unpaused() {
        if(!visible || notification_only_ || !record_dropdown_button_ptr || recording_status != RecordingStatus::RECORD)
            return;

        record_dropdown_button_ptr->set_description(TR("Recording"));
        record_dropdown_button_ptr->set_item_label("pause", TR("Pause"));
        record_dropdown_button_ptr->set_item_icon("pause", &get_theme().pause_texture);
    }

    void Overlay::update_ui_recording_started() {
        if(!visible || notification_only_ || !record_dropdown_button_ptr)
            return;

        record_dropdown_button_ptr->set_item_label("start", TR("Stop and save"));
        record_dropdown_button_ptr->set_activated(true);
        record_dropdown_button_ptr->set_description(TR("Recording"));
        record_dropdown_button_ptr->set_item_icon("start", &get_theme().stop_texture);
        record_dropdown_button_ptr->set_item_enabled("pause", recording_status == RecordingStatus::RECORD);
    }

    void Overlay::update_ui_recording_stopped() {
        if(!visible || notification_only_ || !record_dropdown_button_ptr)
            return;

        record_dropdown_button_ptr->set_item_label("start", TR("Start"));
        record_dropdown_button_ptr->set_activated(false);
        record_dropdown_button_ptr->set_description(TR("Not recording"));
        record_dropdown_button_ptr->set_item_icon("start", &get_theme().play_texture);

        record_dropdown_button_ptr->set_item_label("pause", TR("Pause"));
        record_dropdown_button_ptr->set_item_icon("pause", &get_theme().pause_texture);
        record_dropdown_button_ptr->set_item_enabled("pause", false);
        update_upause_status();
        replay_recording = false;
    }

    void Overlay::update_ui_streaming_started() {
        if(!visible || notification_only_ || !stream_dropdown_button_ptr)
            return;

        stream_dropdown_button_ptr->set_item_label("start", TR("Stop"));
        stream_dropdown_button_ptr->set_activated(true);
        stream_dropdown_button_ptr->set_description(TR("Streaming"));
        stream_dropdown_button_ptr->set_item_icon("start", &get_theme().stop_texture);
    }

    void Overlay::update_ui_streaming_stopped() {
        if(!visible || notification_only_ || !stream_dropdown_button_ptr)
            return;

        stream_dropdown_button_ptr->set_item_label("start", TR("Start"));
        stream_dropdown_button_ptr->set_activated(false);
        stream_dropdown_button_ptr->set_description(TR("Not streaming"));
        stream_dropdown_button_ptr->set_item_icon("start", &get_theme().play_texture);
        update_ui_recording_stopped();
    }

    void Overlay::update_ui_replay_started() {
        if(!visible || notification_only_ || !replay_dropdown_button_ptr)
            return;

        replay_dropdown_button_ptr->set_item_label("start", TR("Turn off"));
        replay_dropdown_button_ptr->set_activated(true);
        replay_dropdown_button_ptr->set_description(TR("On"));
        replay_dropdown_button_ptr->set_item_icon("start", &get_theme().stop_texture);
        replay_dropdown_button_ptr->set_item_enabled("save", true);
        replay_dropdown_button_ptr->set_item_enabled("save_1_min", current_recording_config.replay_config.replay_time >= 60);
        replay_dropdown_button_ptr->set_item_enabled("save_10_min", current_recording_config.replay_config.replay_time >= 60 * 10);
    }

    void Overlay::update_ui_replay_stopped() {
        if(!visible || notification_only_ || !replay_dropdown_button_ptr)
            return;

        replay_dropdown_button_ptr->set_item_label("start", TR("Turn on"));
        replay_dropdown_button_ptr->set_activated(false);
        replay_dropdown_button_ptr->set_description(TR("Off"));
        replay_dropdown_button_ptr->set_item_icon("start", &get_theme().play_texture);
        replay_dropdown_button_ptr->set_item_enabled("save", false);
        replay_dropdown_button_ptr->set_item_enabled("save_1_min", false);
        replay_dropdown_button_ptr->set_item_enabled("save_10_min", false);
        update_ui_recording_stopped();
    }

    static std::string get_date_str() {
        char str[128];
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        strftime(str, sizeof(str)-1, "%Y-%m-%d_%H-%M-%S", t);
        return str;
    }

    static std::string container_to_file_extension(const std::string &container) {
        if(container == "matroska")
            return "mkv";
        else if(container == "mpegts")
            return "ts";
        else if(container == "hls")
            return "m3u8";
        else
            return container;
    }

    static std::vector<std::string> create_audio_tracks_cli_args(const std::vector<AudioTrack> &audio_tracks, const GsrInfo &gsr_info) {
        std::vector<std::string> result;
        result.reserve(audio_tracks.size());

        for(const AudioTrack &audio_track : audio_tracks) {
            bool has_audio = false;
            std::string audio_track_merged;
            int num_app_audio = 0;

            if(!audio_track.name.empty())
                audio_track_merged = "name:" + audio_track.name;

            for(const std::string &audio_input_name : audio_track.audio_inputs) {
                std::string new_audio_input_name = audio_input_name;
                const bool is_app_audio = starts_with(new_audio_input_name, "app:");
                if(is_app_audio && !gsr_info.system_info.supports_app_audio)
                    continue;

                if(is_app_audio && audio_track.application_audio_invert)
                    new_audio_input_name.replace(0, 4, "app-inverse:");

                if(is_app_audio)
                    ++num_app_audio;

                if(!audio_track_merged.empty())
                    audio_track_merged += "|";

                audio_track_merged += new_audio_input_name;
                has_audio = true;
            }

            if(num_app_audio == 0 && audio_track.application_audio_invert) {
                if(!audio_track_merged.empty())
                    audio_track_merged += "|";

                audio_track_merged += "app-inverse:";
                has_audio = true;
            }

            if(has_audio)
                result.push_back(std::move(audio_track_merged));
        }

        return result;
    }

    void Overlay::add_region_command(std::vector<const char*> &args, char *region_str, int region_str_size) {
        Region region = region_selector->get_region_selection(x11_dpy, wayland_dpy);
        if(region.size.x <= 32 && region.size.y <= 32) {
            region.size.x = 0;
            region.size.y = 0;
        }
        snprintf(region_str, region_str_size, "%dx%d+%d+%d", region.size.x, region.size.y, region.pos.x, region.pos.y);
        args.push_back("-region");
        args.push_back(region_str);
    }

    void Overlay::add_common_gpu_screen_recorder_args(
        std::vector<const char*> &args,
        const MainConfig &main_config,
        const RecordOptions &record_options,
        const std::vector<std::string> &audio_tracks,
        const std::string &video_bitrate,
        const char *region,
        char *region_str,
        int region_str_size,
        const std::string &region_area_option,
        RecordForceType force_type)
    {
        if(record_options.video_quality == "custom") {
            args.push_back("-bm");
            args.push_back("cbr");
            args.push_back("-q");
            args.push_back(video_bitrate.c_str());
        } else {
            args.push_back("-q");
            args.push_back(record_options.video_quality.c_str());
        }

        if(region_area_option == "focused" || record_options.change_video_resolution) {
            args.push_back("-s");
            args.push_back(region);
        }

        for(const std::string &audio_track : audio_tracks) {
            args.push_back("-a");
            args.push_back(audio_track.c_str());
        }

        if(record_options.restore_portal_session && force_type != RecordForceType::WINDOW) {
            args.push_back("-restore-portal-session");
            args.push_back("yes");
        }

        if(record_options.low_power_mode) {
            args.push_back("-low-power");
            args.push_back("yes");
        }

        if(main_config.exclude_metadata) {
            args.push_back("-exclude-metadata");
            args.push_back("yes");
        }

        if(region_area_option == "region")
            add_region_command(args, region_str, region_str_size);
    }

    static bool validate_capture_target(const std::string &capture_target, const SupportedCaptureOptions &capture_options) {
        if(capture_target == "window") {
            return capture_options.window;
        } else if(capture_target == "focused") {
            return capture_options.focused;
        } else if(capture_target == "region") {
            return capture_options.region;
        } else if(capture_target == "portal") {
            return capture_options.portal;
        } else if(capture_target == "focused_monitor") {
            return !capture_options.monitors.empty();
        } else {
            for(const GsrMonitor &monitor : capture_options.monitors) {
                if(capture_target == monitor.name)
                    return true;
            }
            return false;
        }
    }

    // 本机可用的捕获目标回退顺序：整屏(聚焦显示器) > 区域 > 跟随焦点窗口 > 窗口 > portal。
    static std::string fallback_capture_target(const SupportedCaptureOptions &capture_options) {
        if(!capture_options.monitors.empty())
            return "focused_monitor";
        if(capture_options.region)
            return "region";
        if(capture_options.focused)
            return "focused";
        if(capture_options.window)
            return "window";
        if(capture_options.portal)
            return "portal";
        return "";
    }

    static std::string get_valid_capture_target(const std::string &capture_target, const SupportedCaptureOptions &capture_options) {
        std::string capture_target_clean = capture_target;
        if(starts_with(capture_target_clean, "HDMI-A"))
            capture_target_clean.replace(0, 6, "HDMI");

        for(const GsrMonitor &monitor : capture_options.monitors) {
            std::string monitor_name_clean = monitor.name;
            if(starts_with(monitor_name_clean, "HDMI-A"))
                monitor_name_clean.replace(0, 6, "HDMI");

            if(capture_target_clean == monitor_name_clean)
                return monitor.name;
        }

        return "";
    }

    std::string Overlay::get_capture_target(const std::string &capture_target, const SupportedCaptureOptions &capture_options) {
        if(capture_target == "window") {
            return std::to_string(region_selector->get_window_selection());
        } else if(capture_target == "focused_monitor") {
            std::optional<CursorInfo> cursor_info;
            if(cursor_tracker) {
                cursor_tracker->update();
                cursor_info = cursor_tracker->get_latest_cursor_info();
            }

            std::string focused_monitor_name;
            if(cursor_info) {
                focused_monitor_name = std::move(cursor_info->monitor_name);
            } else {
                Display *display = x11_dpy;
                focused_monitor_name = get_focused_monitor_by_cursor(x11_dpy, cursor_tracker.get(), gsr_info, get_monitors(display));
            }

            focused_monitor_name = get_valid_capture_target(focused_monitor_name, capture_options);
            if(!focused_monitor_name.empty())
                return focused_monitor_name;
            else if(!capture_options.monitors.empty())
                return capture_options.monitors.front().name;
            else
                return "";
        } else {
            return capture_target;
        }
    }

    void Overlay::prepare_gsr_output_for_reading() {
        if(gpu_screen_recorder_process_output_fd <= 0)
            return;

        const int fdl = fcntl(gpu_screen_recorder_process_output_fd, F_GETFL);
        fcntl(gpu_screen_recorder_process_output_fd, F_SETFL, fdl | O_NONBLOCK);
        gpu_screen_recorder_process_output_file = fdopen(gpu_screen_recorder_process_output_fd, "r");
        if(gpu_screen_recorder_process_output_file)
            gpu_screen_recorder_process_output_fd = -1;
    }

    void Overlay::on_press_save_replay() {
        if(recording_status != RecordingStatus::REPLAY || gpu_screen_recorder_process <= 0)
            return;

        replay_save_duration_min = 0;
        replay_save_show_notification = true;
        replay_save_clock.restart();
        replay_saved_duration_sec = replay_duration_clock.get_elapsed_time_seconds();
        if(replay_restart_on_save)
            replay_duration_clock.restart();

        kill(gpu_screen_recorder_process, SIGUSR1);
    }

    void Overlay::on_press_save_replay_1_min_replay() {
        if(recording_status != RecordingStatus::REPLAY || gpu_screen_recorder_process <= 0)
            return;

        if(current_recording_config.replay_config.replay_time < 60)
            return;

        replay_save_duration_min = 1;
        replay_save_show_notification = true;
        replay_save_clock.restart();
        replay_saved_duration_sec = replay_duration_clock.get_elapsed_time_seconds();
        kill(gpu_screen_recorder_process, SIGRTMIN+3);
    }

    void Overlay::on_press_save_replay_10_min_replay() {
        if(recording_status != RecordingStatus::REPLAY || gpu_screen_recorder_process <= 0)
            return;

        if(current_recording_config.replay_config.replay_time < 60 * 10)
            return;

        replay_save_duration_min = 10;
        replay_save_show_notification = true;
        replay_save_clock.restart();
        replay_saved_duration_sec = replay_duration_clock.get_elapsed_time_seconds();
        kill(gpu_screen_recorder_process, SIGRTMIN+5);
    }

    static const char* change_container_if_codec_not_supported(const char *video_codec, const char *container) {
        // gpu-screen-recorder falls back to a whip (webrtc) compatible video codec automatically
        if(strcmp(container, "whip") == 0)
            return container;

        if(strcmp(video_codec, "vp8") == 0 || strcmp(video_codec, "vp9") == 0) {
            if(strcmp(container, "webm") != 0 && strcmp(container, "matroska") != 0) {
                fprintf(stderr, "Warning: container '%s' is not compatible with video codec '%s', using webm container instead\n", container, video_codec);
                return "webm";
            }
        } else if(strcmp(container, "webm") == 0) {
            fprintf(stderr, "Warning: container webm is not compatible with video codec '%s', using mp4 container instead\n",  video_codec);
            return "mp4";
        }
        return container;
    }

    static void choose_video_codec_and_container_with_fallback(const GsrInfo &gsr_info, const RecordOptions &record_options, const char **video_codec, const char **container, const char **encoder) {
        *encoder = "gpu";
        if(strcmp(*video_codec, "h264_software") == 0) {
            *video_codec = "h264";
            *encoder = "cpu";
        } else if(strcmp(*video_codec, "auto") == 0) {
            if(!get_first_usable_hardware_video_codec_name(gsr_info)) {
                *video_codec = "h264";
                *encoder = "cpu";
            }
        }
        *container = change_container_if_codec_not_supported(*video_codec, *container);

        // if(record_options.enable_vulkan_video_encoding && strcmp(*encoder, "gpu") == 0) {
        //     if(strcmp(*video_codec, "auto") == 0)
        //         *video_codec = "h264_vulkan";
        //     else if(strcmp(*video_codec, "h264") == 0)
        //         *video_codec = "h264_vulkan";
        //     else if(strcmp(*video_codec, "hevc") == 0)
        //         *video_codec = "hevc_vulkan";
        //     else if(strcmp(*video_codec, "hevc_hdr") == 0)
        //         *video_codec = "hevc_hdr_vulkan";
        //     else if(strcmp(*video_codec, "hevc_10bit") == 0)
        //         *video_codec = "hevc_10bit_vulkan";
        //     else if(strcmp(*video_codec, "av1") == 0)
        //         *video_codec = "av1_vulkan";
        //     else if(strcmp(*video_codec, "av1_hdr") == 0)
        //         *video_codec = "av1_hdr_vulkan";
        //     else if(strcmp(*video_codec, "av1_10bit") == 0)
        //         *video_codec = "av1_10bit_vulkan";
        // }
    }

    static std::string get_framerate_mode_validate(const RecordOptions &record_options, const GsrInfo &gsr_info) {
        (void)gsr_info;
        std::string framerate_mode = record_options.framerate_mode;
        if(framerate_mode == "auto")
            framerate_mode = "vfr";
        return framerate_mode;
    }

    struct CameraAlignment {
        std::string halign;
        std::string valign;
        mgl::vec2i pos;
    };

    static CameraAlignment position_to_alignment(mgl::vec2i pos, mgl::vec2i size) {
        const mgl::vec2i pos_overflow = mgl::vec2i(100, 100) - (pos + size);
        if(pos_overflow.x < 0)
            pos.x += pos_overflow.x;
        if(pos_overflow.y < 0)
            pos.y += pos_overflow.y;

        if(pos.x < 0)
            pos.x = 0;
        if(pos.y < 0)
            pos.y = 0;

        CameraAlignment camera_alignment;
        const mgl::vec2i center = pos + size/2;

        if(center.x < 50) {
            camera_alignment.halign = "start";
            camera_alignment.pos.x = pos.x;
        } else {
            camera_alignment.halign = "end";
            camera_alignment.pos.x = -(100 - (pos.x + size.x));
        }

        if(center.y < 50) {
            camera_alignment.valign = "start";
            camera_alignment.pos.y = pos.y;
        } else {
            camera_alignment.valign = "end";
            camera_alignment.pos.y = -(100 - (pos.y + size.y));
        }

        return camera_alignment;
    }

    static std::string compose_capture_source_arg(const std::string &capture_target, const RecordOptions &record_options) {
        std::string capture_source_arg = capture_target;
        if(!record_options.webcam_source.empty()) {
            const mgl::vec2i webcam_size(record_options.webcam_width, record_options.webcam_height);
            const CameraAlignment camera_alignment = position_to_alignment(mgl::vec2i(record_options.webcam_x, record_options.webcam_y), webcam_size);

            capture_source_arg += "|" + record_options.webcam_source;
            capture_source_arg += ";halign=" + camera_alignment.halign;
            capture_source_arg += ";valign=" + camera_alignment.valign;
            capture_source_arg += ";x=" + std::to_string(camera_alignment.pos.x) + "%";
            capture_source_arg += ";y=" + std::to_string(camera_alignment.pos.y) + "%";
            capture_source_arg += ";width=" + std::to_string(webcam_size.x) + "%";
            capture_source_arg += ";height=" + std::to_string(webcam_size.y) + "%";
            capture_source_arg += ";pixfmt=" + record_options.webcam_video_format;
            capture_source_arg += ";camera_width=" + std::to_string(record_options.webcam_camera_width);
            capture_source_arg += ";camera_height=" + std::to_string(record_options.webcam_camera_height);
            capture_source_arg += ";camera_fps=" + std::to_string(record_options.webcam_camera_fps);
            if(record_options.webcam_flip_horizontally)
                capture_source_arg += ";hflip=true";
        }
        return capture_source_arg;
    }

    bool Overlay::on_press_start_replay(bool disable_notification, bool finished_selection, bool launched_manually) {
        if(region_selector->is_started())
            return false;

        switch(recording_status) {
            case RecordingStatus::NONE:
            case RecordingStatus::REPLAY:
                break;
            case RecordingStatus::RECORD:
                show_notification(TR("Unable to start replay when recording. Stop recording before starting replay."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::RECORD, nullptr, NotificationLevel::ERROR);
                return false;
            case RecordingStatus::STREAM:
                show_notification(TR("Unable to start replay when streaming. Stop streaming before starting replay."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::STREAM, nullptr, NotificationLevel::ERROR);
                return false;
        }

        update_upause_status();
        replay_launched_manually = launched_manually;
        replay_launched_once = true;

        close_gpu_screen_recorder_output();

        if(gpu_screen_recorder_process > 0) {
            kill(gpu_screen_recorder_process, SIGINT);
            int status;
            if(waitpid(gpu_screen_recorder_process, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            }

            gpu_screen_recorder_process = -1;
            recording_status = RecordingStatus::NONE;
            replay_save_duration_min = 0;
            update_ui_replay_stopped();
            replay_launched_manually = false;

            if(led_indicator)
                led_indicator->set_led(false);

            // TODO: Show this with a slight delay to make sure it doesn't show up in the video
            if(!disable_notification && config.replay_config.record_options.show_notifications)
                show_notification(TR("Replay stopped"), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::REPLAY);

            return true;
        }

        // 先校验再进入区域/窗口选择叠加层（原因同录制：无效目标会抓取全局输入、卡死/崩溃游戏）
        const SupportedCaptureOptions capture_options = get_supported_capture_options(gsr_info);
        if(!validate_capture_target(config.replay_config.record_options.record_area_option, capture_options)) {
            const std::string fallback = fallback_capture_target(capture_options);
            if(!fallback.empty()) {
                fprintf(stderr, "Warning: replay capture target '%s' is not supported on this system, falling back to '%s'\n",
                    config.replay_config.record_options.record_area_option.c_str(), fallback.c_str());
                config.replay_config.record_options.record_area_option = fallback;
            } else {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), TR("Failed to start replay, capture target \"%s\" is invalid. Please change capture target in settings"), config.replay_config.record_options.record_area_option.c_str());
                show_notification(err_msg, notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::REPLAY, nullptr, NotificationLevel::ERROR);
                return false;
            }
        }

        if(config.replay_config.record_options.record_area_option == "region" && !finished_selection) {
            start_region_capture = true;
            on_region_selected = [disable_notification, launched_manually, this]() {
                on_press_start_replay(disable_notification, true, launched_manually);
            };
            return false;
        }

        if(config.replay_config.record_options.record_area_option == "window" && !finished_selection) {
            start_window_capture = true;
            on_region_selected = [disable_notification, launched_manually, this]() {
                on_press_start_replay(disable_notification, true, launched_manually);
            };
            return false;
        }

        recording_capture_target = get_capture_target(config.replay_config.record_options.record_area_option, capture_options);

        // TODO: Validate input, fallback to valid values
        const std::string fps = std::to_string(config.replay_config.record_options.fps);
        const std::string video_bitrate = std::to_string(config.replay_config.record_options.video_bitrate);
        const std::string output_directory = config.replay_config.save_directory;
        const std::vector<std::string> audio_tracks = create_audio_tracks_cli_args(config.replay_config.record_options.audio_tracks_list, gsr_info);
        const std::string framerate_mode = get_framerate_mode_validate(config.replay_config.record_options, gsr_info);
        const std::string replay_time = std::to_string(config.replay_config.replay_time);
        const char *container = config.replay_config.container.c_str();
        const char *video_codec = config.replay_config.record_options.video_codec.c_str();
        const char *encoder = "gpu";
        choose_video_codec_and_container_with_fallback(gsr_info, config.replay_config.record_options, &video_codec, &container, &encoder);

        char size[64];
        size[0] = '\0';
        if(config.replay_config.record_options.record_area_option == "focused")
            snprintf(size, sizeof(size), "%dx%d", (int)config.replay_config.record_options.record_area_width, (int)config.replay_config.record_options.record_area_height);

        if(config.replay_config.record_options.record_area_option != "focused" && config.replay_config.record_options.change_video_resolution)
            snprintf(size, sizeof(size), "%dx%d", (int)config.replay_config.record_options.video_width, (int)config.replay_config.record_options.video_height);

        const std::string capture_source_arg = compose_capture_source_arg(recording_capture_target, config.replay_config.record_options);

        std::vector<const char*> args = {
            "gpu-screen-recorder", "-w", capture_source_arg.c_str(),
            "-c", container,
            "-ac", config.replay_config.record_options.audio_codec.c_str(),
            "-cursor", config.replay_config.record_options.record_cursor ? "yes" : "no",
            "-cr", config.replay_config.record_options.color_range.c_str(),
            "-fm", framerate_mode.c_str(),
            "-k", video_codec,
            "-encoder", encoder,
            "-f", fps.c_str(),
            "-r", replay_time.c_str(),
            "-v", "no",
            "-o", output_directory.c_str()
        };

        if(config.replay_config.restart_replay_on_save) {
            args.push_back("-restart-replay-on-save");
            args.push_back("yes");
            replay_restart_on_save = true;
        } else {
            replay_restart_on_save = false;
        }

        args.push_back("-replay-storage");
        args.push_back(config.replay_config.replay_storage.c_str());

        char region_str[128];
        add_common_gpu_screen_recorder_args(args, config.main_config, config.replay_config.record_options, audio_tracks, video_bitrate, size, region_str, sizeof(region_str), config.replay_config.record_options.record_area_option);

        args.push_back("-ro");
        args.push_back(config.record_config.save_directory.c_str());

        args.push_back(nullptr);

        current_recording_config = config;

        gpu_screen_recorder_process = exec_program(args.data(), &gpu_screen_recorder_process_output_fd);
        if(gpu_screen_recorder_process == -1) {
            show_notification(TR("Failed to launch gpu-screen-recorder to start replay"), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::REPLAY, nullptr, NotificationLevel::ERROR);
            return false;
        } else {
            recording_status = RecordingStatus::REPLAY;
            update_ui_replay_started();

            if(led_indicator && config.replay_config.record_options.use_led_indicator)
                led_indicator->set_led(true);
        }

        prepare_gsr_output_for_reading();

        // TODO: Start recording after this notification has disappeared to make sure it doesn't show up in the video.
        // Make clear to the user that the recording starts after the notification is gone.
        // Maybe have the option in notification to show timer until its getting hidden, then the notification can say:
        // Starting recording in 3...
        // 2...
        // 1...
        // TODO: Do not run this is a daemon. Instead get the pid and when launching another notification close the current notification
        // program and start another one. This can also be used to check when the notification has finished by checking with waitpid NOWAIT
        // to see when the program has exit.
        if(!disable_notification && config.replay_config.record_options.show_notifications) {
            char msg[256];
            snprintf(msg, sizeof(msg), TR("Started replaying %s"), capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), false).c_str());
            show_notification(msg, short_notification_timeout_seconds, get_color_theme().tint_color, get_color_theme().tint_color, NotificationType::REPLAY, recording_capture_target.c_str());
        }

        if(config.replay_config.record_options.record_area_option == "portal")
            hide_ui = true;

        // TODO: This will be incorrect if the user uses portal capture, as capture wont start until the user has
        // selected what to capture and accepted it.
        replay_duration_clock.restart();
        return true;
    }

    void Overlay::on_press_start_record(bool finished_selection, RecordForceType force_type) {
        if(region_selector->is_started())
            return;

        switch(recording_status) {
            case RecordingStatus::NONE:
            case RecordingStatus::RECORD:
                break;
            case RecordingStatus::REPLAY: {
                if(gpu_screen_recorder_process <= 0)
                    return;

                if(!replay_recording) {
                    if(config.record_config.record_options.show_notifications)
                        show_notification(TR("Started recording in the replay session"), short_notification_timeout_seconds, get_color_theme().tint_color, get_color_theme().tint_color, NotificationType::RECORD);
                    update_ui_recording_started();

                    // TODO: This will be incorrect if the user uses portal capture, as capture wont start until the user has
                    // selected what to capture and accepted it.
                    recording_duration_clock.restart();
                    update_upause_status();

                    if(led_indicator) {
                        if(config.record_config.record_options.use_led_indicator) {
                            if(!current_recording_config.replay_config.record_options.use_led_indicator)
                                led_indicator->set_led(true);
                            else
                                led_indicator->blink();
                        }
                    }
                }

                replay_recording = true;
                kill(gpu_screen_recorder_process, SIGRTMIN);
                return;
            }
            case RecordingStatus::STREAM: {
                if(gpu_screen_recorder_process <= 0)
                    return;

                if(!replay_recording) {
                    if(config.record_config.record_options.show_notifications)
                        show_notification(TR("Started recording in the streaming session"), short_notification_timeout_seconds, get_color_theme().tint_color, get_color_theme().tint_color, NotificationType::RECORD);
                    update_ui_recording_started();

                    // TODO: This will be incorrect if the user uses portal capture, as capture wont start until the user has
                    // selected what to capture and accepted it.
                    recording_duration_clock.restart();
                    update_upause_status();

                    if(led_indicator) {
                        if(config.record_config.record_options.use_led_indicator) {
                            if(!current_recording_config.streaming_config.record_options.use_led_indicator)
                                led_indicator->set_led(true);
                            else
                                led_indicator->blink();
                        }
                    }
                }

                replay_recording = true;
                kill(gpu_screen_recorder_process, SIGRTMIN);
                return;
            }
        }

        close_gpu_screen_recorder_output();

        if(gpu_screen_recorder_process > 0) {
            kill(gpu_screen_recorder_process, SIGINT);
            int status;
            if(waitpid(gpu_screen_recorder_process, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            } else {
                int exit_code = -1;
                if(WIFEXITED(status))
                    exit_code = WEXITSTATUS(status);
                on_stop_recording(exit_code, record_filepath);
            }

            gpu_screen_recorder_process = -1;
            recording_status = RecordingStatus::NONE;
            update_ui_recording_stopped();
            update_upause_status();
            record_filepath.clear();

            if(led_indicator)
                led_indicator->set_led(false);
            return;
        }

        update_upause_status();

        std::string record_area_option;
        switch(force_type) {
            case RecordForceType::NONE:
                record_area_option = config.record_config.record_options.record_area_option;
                break;
            case RecordForceType::REGION:
                record_area_option = "region";
                break;
            case RecordForceType::WINDOW:
                record_area_option = gsr_info.system_info.display_server == DisplayServer::X11 ? "window" : "portal";
                break;
        }

        // 必须先校验捕获目标，再做区域/窗口选择。
        // 关键：无效目标绝不能进入区域/窗口选择的叠加层 —— 那个叠加层会全屏覆盖并抓取
        // 全局输入，在 Wayland/XWayland 下会把游戏卡死甚至搞崩（用户实测选 "window" 会崩游戏）。
        // 无效时自动回退到本机支持的整屏目标，让"录制"依然能用，而不是报错什么都不做。
        const SupportedCaptureOptions capture_options = get_supported_capture_options(gsr_info);
        if(!validate_capture_target(record_area_option, capture_options)) {
            const std::string fallback = fallback_capture_target(capture_options);
            if(!fallback.empty()) {
                fprintf(stderr, "Warning: capture target '%s' is not supported on this system, falling back to '%s'\n",
                    record_area_option.c_str(), fallback.c_str());
                record_area_option = fallback;
            } else {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), TR("Failed to start recording, capture target \"%s\" is invalid. Please change capture target in settings"), record_area_option.c_str());
                show_notification(err_msg, notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::RECORD, nullptr, NotificationLevel::ERROR);
                return;
            }
        }

        if(record_area_option == "region" && !finished_selection) {
            start_region_capture = true;
            on_region_selected = [this, force_type]() {
                on_press_start_record(true, force_type);
            };
            return;
        }

        if(record_area_option == "window" && !finished_selection) {
            start_window_capture = true;
            on_region_selected = [this, force_type]() {
                on_press_start_record(true, force_type);
            };
            return;
        }

        recording_capture_target = get_capture_target(record_area_option, capture_options);

        record_filepath.clear();

        // TODO: Validate input, fallback to valid values
        const std::string fps = std::to_string(config.record_config.record_options.fps);
        const std::string video_bitrate = std::to_string(config.record_config.record_options.video_bitrate);
        const std::string output_file = config.record_config.save_directory + "/Video_" + get_date_str() + "." + container_to_file_extension(config.record_config.container.c_str());
        const std::vector<std::string> audio_tracks = create_audio_tracks_cli_args(config.record_config.record_options.audio_tracks_list, gsr_info);
        const std::string framerate_mode = get_framerate_mode_validate(config.record_config.record_options, gsr_info);
        const char *container = config.record_config.container.c_str();
        const char *video_codec = config.record_config.record_options.video_codec.c_str();
        const char *encoder = "gpu";
        choose_video_codec_and_container_with_fallback(gsr_info, config.record_config.record_options, &video_codec, &container, &encoder);

        char size[64];
        size[0] = '\0';
        if(record_area_option == "focused")
            snprintf(size, sizeof(size), "%dx%d", (int)config.record_config.record_options.record_area_width, (int)config.record_config.record_options.record_area_height);

        if(record_area_option != "focused" && config.record_config.record_options.change_video_resolution)
            snprintf(size, sizeof(size), "%dx%d", (int)config.record_config.record_options.video_width, (int)config.record_config.record_options.video_height);

        const std::string capture_source_arg = compose_capture_source_arg(recording_capture_target, config.record_config.record_options);

        std::vector<const char*> args = {
            "gpu-screen-recorder", "-w", capture_source_arg.c_str(),
            "-c", container,
            "-ac", config.record_config.record_options.audio_codec.c_str(),
            "-cursor", config.record_config.record_options.record_cursor ? "yes" : "no",
            "-cr", config.record_config.record_options.color_range.c_str(),
            "-fm", framerate_mode.c_str(),
            "-k", video_codec,
            "-encoder", encoder,
            "-f", fps.c_str(),
            "-v", "no",
            "-o", output_file.c_str()
        };

        const std::string hotkey_window_capture_portal_session_token_filepath = get_config_dir() + "/gsr-ui-window-capture-token";
        if(record_area_option == "portal") {
            hide_ui = true;
            if(force_type == RecordForceType::WINDOW) {
                args.push_back("-portal-session-token-filepath");
                args.push_back(hotkey_window_capture_portal_session_token_filepath.c_str());
            }
        }

        char region_str[128];
        add_common_gpu_screen_recorder_args(args, config.main_config, config.record_config.record_options, audio_tracks, video_bitrate, size, region_str, sizeof(region_str), record_area_option, force_type);

        args.push_back(nullptr);

        current_recording_config = config;

        record_filepath = output_file;
        gpu_screen_recorder_process = exec_program(args.data(), &gpu_screen_recorder_process_output_fd);
        if(gpu_screen_recorder_process == -1) {
            show_notification(TR("Failed to launch gpu-screen-recorder to start recording"), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::RECORD, nullptr, NotificationLevel::ERROR);
            return;
        } else {
            recording_status = RecordingStatus::RECORD;
            update_ui_recording_started();

            if(led_indicator && config.record_config.record_options.use_led_indicator)
                led_indicator->set_led(true);
        }

        prepare_gsr_output_for_reading();

        // TODO: Start recording after this notification has disappeared to make sure it doesn't show up in the video.
        // Make clear to the user that the recording starts after the notification is gone.
        // Maybe have the option in notification to show timer until its getting hidden, then the notification can say:
        // Starting recording in 3...
        // 2...
        // 1...
        if(config.record_config.record_options.show_notifications) {
            char msg[256];
            snprintf(msg, sizeof(msg), TR("Started recording %s"), capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), false).c_str());
            show_notification(msg, short_notification_timeout_seconds, get_color_theme().tint_color, get_color_theme().tint_color, NotificationType::RECORD, recording_capture_target.c_str());
        }

        // TODO: This will be incorrect if the user uses portal capture, as capture wont start until the user has
        // selected what to capture and accepted it.
        recording_duration_clock.restart();
    }

    static bool host_is_numeric_ipv4(const std::string &host) {
        if(host.empty())
            return false;

        for(char c : host) {
            if((c < '0' || c > '9') && c != '.')
                return false;
        }
        return true;
    }

    /* |url| is expected to not have a scheme (http:// or https://) prefix */
    static bool url_is_local_address(const std::string &url) {
        // The authority is everything before the first path/query/fragment separator
        std::string authority = url;
        const size_t path_index = authority.find_first_of("/?#");
        if(path_index != std::string::npos)
            authority = authority.substr(0, path_index);

        // Reject userinfo: in "localhost:@example.com/x" the real host is example.com (everything before the '@' is userinfo).
        // Whip endpoint urls never contain userinfo
        if(authority.find('@') != std::string::npos)
            return false;

        if(strncmp(authority.c_str(), "[::1]", 5) == 0)
            return authority.size() == 5 || authority[5] == ':';

        std::string host = authority;
        const size_t port_index = host.find(':');
        if(port_index != std::string::npos)
            host = host.substr(0, port_index);

        // The numeric check makes sure a hostname such as 127.malicious.example doesn't pass as a loopback address
        return host == "localhost" || (host_is_numeric_ipv4(host) && strncmp(host.c_str(), "127.", 4) == 0);
    }

    static std::string streaming_get_url(const Config &config) {
        std::string url;
        fprintf(stderr, "streaming service: %s\n", config.streaming_config.streaming_service.c_str());
        if(config.streaming_config.streaming_service == "twitch") {
            url += "rtmp://live.twitch.tv/app/";
            url += config.streaming_config.twitch.stream_key;
        } else if(config.streaming_config.streaming_service == "youtube") {
            url += "rtmp://a.rtmp.youtube.com/live2/";
            url += config.streaming_config.youtube.stream_key;
        } else if(config.streaming_config.streaming_service == "rumble") {
            url += "rtmp://rtmp.rumble.com/live/";
            url += config.streaming_config.rumble.stream_key;
        } else if(config.streaming_config.streaming_service == "kick") {
            url += config.streaming_config.kick.stream_url;
            if(!url.empty() && url.back() != '/')
                url += "/";
            url += "app/";
            url += config.streaming_config.kick.stream_key;
        } else if(config.streaming_config.streaming_service == "whip") {
            url = config.streaming_config.whip.url;
            if(url.size() >= 7 && strncmp(url.c_str(), "http://", 7) == 0)
            {}
            else if(url.size() >= 8 && strncmp(url.c_str(), "https://", 8) == 0)
            {}
            else if(url_is_local_address(url))
                url = "http://" + url;
        } else if(config.streaming_config.streaming_service == "custom") {
            url = config.streaming_config.custom.url;
            if(url.size() >= 7 && strncmp(url.c_str(), "rtmp://", 7) == 0)
            {}
            else if(url.size() >= 8 && strncmp(url.c_str(), "rtmps://", 8) == 0)
            {}
            else if(url.size() >= 7 && strncmp(url.c_str(), "rtsp://", 7) == 0)
            {}
            else if(url.size() >= 6 && strncmp(url.c_str(), "srt://", 6) == 0)
            {}
            else if(url.size() >= 7 && strncmp(url.c_str(), "http://", 7) == 0)
            {}
            else if(url.size() >= 8 && strncmp(url.c_str(), "https://", 8) == 0)
            {}
            else if(url.size() >= 6 && strncmp(url.c_str(), "tcp://", 6) == 0)
            {}
            else if(url.size() >= 6 && strncmp(url.c_str(), "udp://", 6) == 0)
            {}
            else
                url = "rtmp://" + url;

            if(!url.empty() && url.back() != '/' && url.back() != '=' && !config.streaming_config.custom.key.empty())
                url += "/";

            url += config.streaming_config.custom.key;
        }
        return url;
    }

    void Overlay::on_press_start_stream(bool finished_selection) {
        if(region_selector->is_started())
            return;

        switch(recording_status) {
            case RecordingStatus::NONE:
            case RecordingStatus::STREAM:
                break;
            case RecordingStatus::REPLAY:
                show_notification(TR("Unable to start streaming when replay is turned on. Turn off replay before starting streaming."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::REPLAY, nullptr, NotificationLevel::ERROR);
                return;
            case RecordingStatus::RECORD:
                show_notification(TR("Unable to start streaming when recording. Stop recording before starting streaming."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::RECORD, nullptr, NotificationLevel::ERROR);
                return;
        }

        update_upause_status();

        close_gpu_screen_recorder_output();

        if(gpu_screen_recorder_process > 0) {
            kill(gpu_screen_recorder_process, SIGINT);
            int status;
            if(waitpid(gpu_screen_recorder_process, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            }

            gpu_screen_recorder_process = -1;
            recording_status = RecordingStatus::NONE;
            update_ui_streaming_stopped();

            if(led_indicator)
                led_indicator->set_led(false);

            // TODO: Show this with a slight delay to make sure it doesn't show up in the video
            if(config.streaming_config.record_options.show_notifications)
                show_notification(TR("Streaming has stopped"), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::STREAM);
            return;
        }

        // 先校验再进入区域/窗口选择叠加层（原因同录制：无效目标会抓取全局输入、卡死/崩溃游戏）
        const SupportedCaptureOptions capture_options = get_supported_capture_options(gsr_info);
        if(!validate_capture_target(config.streaming_config.record_options.record_area_option, capture_options)) {
            const std::string fallback = fallback_capture_target(capture_options);
            if(!fallback.empty()) {
                fprintf(stderr, "Warning: streaming capture target '%s' is not supported on this system, falling back to '%s'\n",
                    config.streaming_config.record_options.record_area_option.c_str(), fallback.c_str());
                config.streaming_config.record_options.record_area_option = fallback;
            } else {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), TR("Failed to start streaming, capture target \"%s\" is invalid. Please change capture target in settings"), config.streaming_config.record_options.record_area_option.c_str());
                show_notification(err_msg, notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::STREAM, nullptr, NotificationLevel::ERROR);
                return;
            }
        }

        if(config.streaming_config.record_options.record_area_option == "region" && !finished_selection) {
            start_region_capture = true;
            on_region_selected = [this]() {
                on_press_start_stream(true);
            };
            return;
        }

        if(config.streaming_config.record_options.record_area_option == "window" && !finished_selection) {
            start_window_capture = true;
            on_region_selected = [this]() {
                on_press_start_stream(true);
            };
            return;
        }

        recording_capture_target = get_capture_target(config.streaming_config.record_options.record_area_option, capture_options);

        if(config.streaming_config.streaming_service == "whip") {
            if(!gsr_info.supported_containers.whip) {
                show_notification(TR("Failed to start streaming, WHIP streaming requires GPU Screen Recorder built with FFmpeg 8.0 or later"), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::STREAM, nullptr, NotificationLevel::ERROR);
                return;
            }

            const std::string &whip_url = config.streaming_config.whip.url;
            if(whip_url.empty()) {
                show_notification(TR("Failed to start streaming, the WHIP stream URL is empty. Please set a stream URL in settings"), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::STREAM, nullptr, NotificationLevel::ERROR);
                return;
            }

            const bool has_scheme = (whip_url.size() >= 7 && strncmp(whip_url.c_str(), "http://", 7) == 0) || (whip_url.size() >= 8 && strncmp(whip_url.c_str(), "https://", 8) == 0);
            // Dont assume a scheme for remote hosts. Assuming https:// breaks plain http servers (like mediamtx on a lan machine)
            // while assuming http:// would silently send the bearer token unencrypted
            if(!has_scheme && !url_is_local_address(whip_url)) {
                show_notification(TR("Failed to start streaming, the WHIP stream URL should start with http:// or https://"), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::STREAM, nullptr, NotificationLevel::ERROR);
                return;
            }
        }

        // TODO: Validate input, fallback to valid values
        const std::string fps = std::to_string(config.streaming_config.record_options.fps);
        const std::string video_bitrate = std::to_string(config.streaming_config.record_options.video_bitrate);
        std::vector<std::string> audio_tracks = create_audio_tracks_cli_args(config.streaming_config.record_options.audio_tracks_list, gsr_info);
        // This isn't possible unless the user modified the config file manually,
        // But we check it anyways as streaming on some sites can fail if there is more than one audio track
        if(audio_tracks.size() > 1)
            audio_tracks.resize(1);
        const std::string framerate_mode = get_framerate_mode_validate(config.streaming_config.record_options, gsr_info);
        const char *container = "flv";
        if(config.streaming_config.streaming_service == "custom")
            container = config.streaming_config.custom.container.c_str();
        else if(config.streaming_config.streaming_service == "whip")
            container = "whip";
        const char *video_codec = config.streaming_config.record_options.video_codec.c_str();
        const char *encoder = "gpu";
        choose_video_codec_and_container_with_fallback(gsr_info, config.streaming_config.record_options, &video_codec, &container, &encoder);

        const std::string url = streaming_get_url(config);
        if(config.streaming_config.streaming_service == "rumble" || config.streaming_config.streaming_service == "kick") {
            fprintf(stderr, "Info: forcing video codec to h264 as rumble/kick supports only h264\n");
            video_codec = "h264"; // TODO: Vulkan
        } else if(config.streaming_config.streaming_service == "whip") {
            fprintf(stderr, "Info: forcing video codec to h264 as whip (webrtc) in ffmpeg supports only h264\n");
            video_codec = "h264"; // TODO: Vulkan
        }

        const char *audio_codec = config.streaming_config.record_options.audio_codec.c_str();
        if(config.streaming_config.streaming_service == "whip")
            audio_codec = "opus"; // Whip (webrtc) only supports the opus audio codec

        char size[64];
        size[0] = '\0';
        if(config.streaming_config.record_options.record_area_option == "focused")
            snprintf(size, sizeof(size), "%dx%d", (int)config.streaming_config.record_options.record_area_width, (int)config.streaming_config.record_options.record_area_height);

        if(config.streaming_config.record_options.record_area_option != "focused" && config.streaming_config.record_options.change_video_resolution)
            snprintf(size, sizeof(size), "%dx%d", (int)config.streaming_config.record_options.video_width, (int)config.streaming_config.record_options.video_height);

        const std::string capture_source_arg = compose_capture_source_arg(recording_capture_target, config.streaming_config.record_options);

        std::vector<const char*> args = {
            "gpu-screen-recorder", "-w", capture_source_arg.c_str(),
            "-c", container,
            "-ac", audio_codec,
            "-cursor", config.streaming_config.record_options.record_cursor ? "yes" : "no",
            "-cr", config.streaming_config.record_options.color_range.c_str(),
            "-fm", framerate_mode.c_str(),
            "-k", video_codec,
            "-encoder", encoder,
            "-f", fps.c_str(),
            "-v", "no",
            "-o", url.c_str()
        };

        char region_str[128];
        add_common_gpu_screen_recorder_args(args, config.main_config, config.streaming_config.record_options, audio_tracks, video_bitrate, size, region_str, sizeof(region_str), config.streaming_config.record_options.record_area_option);

        args.push_back("-ro");
        args.push_back(config.record_config.save_directory.c_str());

        args.push_back(nullptr);

        current_recording_config = config;

        // The bearer token is passed to gpu-screen-recorder in an environment variable instead of as a command line argument
        // to make sure the token isn't visible to other processes in the process list.
        // It's set before exec_program so the child inherits it and it's removed from this process again right after
        const bool pass_whip_token = config.streaming_config.streaming_service == "whip" && !config.streaming_config.whip.bearer_token.empty();
        if(pass_whip_token)
            setenv("GSR_AUTH", config.streaming_config.whip.bearer_token.c_str(), 1);

        gpu_screen_recorder_process = exec_program(args.data(), &gpu_screen_recorder_process_output_fd);

        if(pass_whip_token)
            unsetenv("GSR_AUTH");

        if(gpu_screen_recorder_process == -1) {
            show_notification(TR("Failed to launch gpu-screen-recorder to start streaming"), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::STREAM, nullptr, NotificationLevel::ERROR);
            return;
        } else {
            recording_status = RecordingStatus::STREAM;
            update_ui_streaming_started();

            if(led_indicator && config.streaming_config.record_options.use_led_indicator)
                led_indicator->set_led(true);
        }

        prepare_gsr_output_for_reading();

        // TODO: Start recording after this notification has disappeared to make sure it doesn't show up in the video.
        // Make clear to the user that the recording starts after the notification is gone.
        // Maybe have the option in notification to show timer until its getting hidden, then the notification can say:
        // Starting recording in 3...
        // 2...
        // 1...
        // TODO: Do not run this is a daemon. Instead get the pid and when launching another notification close the current notification
        // program and start another one. This can also be used to check when the notification has finished by checking with waitpid NOWAIT
        // to see when the program has exit.
        if(config.streaming_config.record_options.show_notifications) {
            char msg[256];
            snprintf(msg, sizeof(msg), TR("Started streaming %s"), capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), false).c_str());
            show_notification(msg, short_notification_timeout_seconds, get_color_theme().tint_color, get_color_theme().tint_color, NotificationType::STREAM, recording_capture_target.c_str());
        }

        if(config.streaming_config.record_options.record_area_option == "portal")
            hide_ui = true;
    }

    void Overlay::on_press_take_screenshot(bool finished_selection, ScreenshotForceType force_type) {
        if(region_selector->is_started())
            return;

        if(gpu_screen_recorder_screenshot_process > 0) {
            fprintf(stderr, "Error: failed to take screenshot, another screenshot is currently being saved\n");
            return;
        }

        std::string record_area_option;
        switch(force_type) {
            case ScreenshotForceType::NONE:
                record_area_option = config.screenshot_config.record_area_option;
                break;
            case ScreenshotForceType::REGION:
                record_area_option = "region";
                break;
            case ScreenshotForceType::WINDOW:
                record_area_option = gsr_info.system_info.display_server == DisplayServer::X11 ? "window" : "portal";
                break;
        }

        // 先校验再进入区域/窗口选择叠加层（原因同录制：无效目标会抓取全局输入、卡死/崩溃游戏）
        const SupportedCaptureOptions capture_options = get_supported_capture_options(gsr_info);
        if(!validate_capture_target(record_area_option, capture_options)) {
            const std::string fallback = fallback_capture_target(capture_options);
            if(!fallback.empty()) {
                fprintf(stderr, "Warning: screenshot capture target '%s' is not supported on this system, falling back to '%s'\n",
                    record_area_option.c_str(), fallback.c_str());
                record_area_option = fallback;
            } else {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), TR("Failed to take a screenshot, capture target \"%s\" is invalid. Please change capture target in settings"), record_area_option.c_str());
                show_notification(err_msg, notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::SCREENSHOT, nullptr, NotificationLevel::ERROR);
                return;
            }
        }

        if(record_area_option == "region" && !finished_selection) {
            start_region_capture = true;
            on_region_selected = [this, force_type]() {
                on_press_take_screenshot(true, force_type);
            };
            return;
        }

        if(record_area_option == "window" && !finished_selection) {
            start_window_capture = true;
            on_region_selected = [this, force_type]() {
                on_press_take_screenshot(true, force_type);
            };
            return;
        }

        screenshot_capture_target = get_capture_target(record_area_option, capture_options);

        // TODO: Validate input, fallback to valid values
        std::string output_file;
        if(config.screenshot_config.save_screenshot_to_disk)
            output_file = config.screenshot_config.save_directory + "/Screenshot_" + get_date_str() + "." + config.screenshot_config.image_format; // TODO: Validate image format
        else
            output_file = "/tmp/gsr_ui_clipboard_screenshot." + config.screenshot_config.image_format;

        const bool capture_cursor = force_type == ScreenshotForceType::NONE && config.screenshot_config.record_cursor;

        std::vector<const char*> args = {
            "gpu-screen-recorder", "-w", screenshot_capture_target.c_str(),
            "-cursor", capture_cursor ? "yes" : "no",
            "-v", "no",
            "-q", config.screenshot_config.image_quality.c_str(),
            "-o", output_file.c_str()
        };

        char size[64];
        size[0] = '\0';
        if(config.screenshot_config.change_image_resolution) {
            snprintf(size, sizeof(size), "%dx%d", (int)config.screenshot_config.image_width, (int)config.screenshot_config.image_height);
            args.push_back("-s");
            args.push_back(size);
        }

        if(config.screenshot_config.restore_portal_session && force_type != ScreenshotForceType::WINDOW) {
            args.push_back("-restore-portal-session");
            args.push_back("yes");
        }

        const std::string hotkey_window_capture_portal_session_token_filepath = get_config_dir() + "/gsr-ui-window-capture-token";
        if(record_area_option == "portal") {
            hide_ui = true;
            if(force_type == ScreenshotForceType::WINDOW) {
                args.push_back("-portal-session-token-filepath");
                args.push_back(hotkey_window_capture_portal_session_token_filepath.c_str());
            }
        }

        char region_str[128];
        if(record_area_option == "region")
            add_region_command(args, region_str, sizeof(region_str));

        args.push_back(nullptr);

        if(clipboard)
            clipboard->set_current_file("", Clipboard::FileType::JPG);

        screenshot_filepath = output_file;
        gpu_screen_recorder_screenshot_process = exec_program(args.data(), nullptr);
        if(gpu_screen_recorder_screenshot_process == -1) {
            show_notification(TR("Failed to launch gpu-screen-recorder to take a screenshot"), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::SCREENSHOT, nullptr, NotificationLevel::ERROR);
        }
    }

    bool Overlay::update_compositor_texture(const Monitor &monitor) {
        window_texture_deinit(&window_texture);
        window_texture_sprite.set_texture(nullptr);
        screenshot_texture.clear();
        screenshot_sprite.set_texture(nullptr);

        if(gsr_info.system_info.display_server != DisplayServer::X11 || wayland_native_overlay)
            return false;

        Display *display = (Display*)mgl_get_context()->connection;
        if(is_compositor_running(display, 0))
            return false;

        bool window_texture_loaded = false;
        Window focused_window = get_focused_window(display, WindowCaptureType::CURSOR);
        if(!focused_window)
            focused_window = get_focused_window(display, WindowCaptureType::FOCUSED);
        if(focused_window && is_window_fullscreen_on_monitor(display, focused_window, monitor))
            window_texture_loaded = window_texture_init(&window_texture, display, mgl_window_get_egl_display(window->internal_window()), focused_window, egl_funcs) == 0;

        if(window_texture_loaded && window_texture.texture_id) {
            window_texture_texture = mgl::Texture(window_texture.texture_id, MGL_TEXTURE_FORMAT_RGB);
            window_texture_sprite.set_texture(&window_texture_texture);
        } else {
            XImage *img = XGetImage(display, DefaultRootWindow(display), monitor.position.x, monitor.position.y, monitor.size.x, monitor.size.y, AllPlanes, ZPixmap);
            if(!img)
                fprintf(stderr, "Error: failed to take a screenshot\n");

            if(img) {
                screenshot_texture = texture_from_ximage(img);
                if(screenshot_texture.is_valid())
                    screenshot_sprite.set_texture(&screenshot_texture);
                XDestroyImage(img);
                img = NULL;
            }
        }

        return true;
    }

    void Overlay::force_window_on_top() {
        if(wayland_native_overlay)
            return;

        if(force_window_on_top_clock.get_elapsed_time_seconds() >= force_window_on_top_timeout_seconds) {
            force_window_on_top_clock.restart();

            // The mgl window is on mgl's X11 connection, not x11_dpy.
            Display *mgl_display = (Display*)mgl_get_context()->connection;
            XRaiseWindow(mgl_display, (Window)window->get_system_handle());
            XFlush(mgl_display);
        }
    }
}
