#include "../../include/imgui/Host.hpp"

#include <imgui.h>
#include <backends/imgui_impl_opengl2.h>

// mgl 用 EGL + OpenGL 2.x 兼容上下文；这里直接链接系统 libGL 用于还原 mgl 期望的状态
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <mglpp/window/Event.hpp>
#include <mglpp/graphics/Texture.hpp>
#include <mglpp/system/vec.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../include/GlobalHotkeys/GlobalHotkeys.hpp"   // HotkeyModifier 位定义

namespace gsr {
namespace imgui_ui {

    namespace {
        struct FontCandidate {
            const char* path;
            unsigned int face_index;
        };

        const FontCandidate font_candidates[] = {
            { "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf", 0 },
            { "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2 },
            { "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", 2 },
            { "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.ttc", 0 },
            { "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0 },
            { "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", 0 },
            { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 0 },
        };

        bool file_exists(const char* path) {
            FILE *f = fopen(path, "rb");
            if(!f)
                return false;
            fclose(f);
            return true;
        }

        ImGuiKey to_imgui_key(mgl::Keyboard::Key key) {
            using Key = mgl::Keyboard::Key;
            switch(key) {
                case Key::A: return ImGuiKey_A;
                case Key::B: return ImGuiKey_B;
                case Key::C: return ImGuiKey_C;
                case Key::D: return ImGuiKey_D;
                case Key::E: return ImGuiKey_E;
                case Key::F: return ImGuiKey_F;
                case Key::G: return ImGuiKey_G;
                case Key::H: return ImGuiKey_H;
                case Key::I: return ImGuiKey_I;
                case Key::J: return ImGuiKey_J;
                case Key::K: return ImGuiKey_K;
                case Key::L: return ImGuiKey_L;
                case Key::M: return ImGuiKey_M;
                case Key::N: return ImGuiKey_N;
                case Key::O: return ImGuiKey_O;
                case Key::P: return ImGuiKey_P;
                case Key::Q: return ImGuiKey_Q;
                case Key::R: return ImGuiKey_R;
                case Key::S: return ImGuiKey_S;
                case Key::T: return ImGuiKey_T;
                case Key::U: return ImGuiKey_U;
                case Key::V: return ImGuiKey_V;
                case Key::W: return ImGuiKey_W;
                case Key::X: return ImGuiKey_X;
                case Key::Y: return ImGuiKey_Y;
                case Key::Z: return ImGuiKey_Z;

                case Key::Num0: return ImGuiKey_0;
                case Key::Num1: return ImGuiKey_1;
                case Key::Num2: return ImGuiKey_2;
                case Key::Num3: return ImGuiKey_3;
                case Key::Num4: return ImGuiKey_4;
                case Key::Num5: return ImGuiKey_5;
                case Key::Num6: return ImGuiKey_6;
                case Key::Num7: return ImGuiKey_7;
                case Key::Num8: return ImGuiKey_8;
                case Key::Num9: return ImGuiKey_9;

                case Key::Numpad0: return ImGuiKey_Keypad0;
                case Key::Numpad1: return ImGuiKey_Keypad1;
                case Key::Numpad2: return ImGuiKey_Keypad2;
                case Key::Numpad3: return ImGuiKey_Keypad3;
                case Key::Numpad4: return ImGuiKey_Keypad4;
                case Key::Numpad5: return ImGuiKey_Keypad5;
                case Key::Numpad6: return ImGuiKey_Keypad6;
                case Key::Numpad7: return ImGuiKey_Keypad7;
                case Key::Numpad8: return ImGuiKey_Keypad8;
                case Key::Numpad9: return ImGuiKey_Keypad9;
                case Key::NumpadEnter: return ImGuiKey_KeypadEnter;
                case Key::Add: return ImGuiKey_KeypadAdd;
                case Key::Subtract: return ImGuiKey_KeypadSubtract;
                case Key::Multiply: return ImGuiKey_KeypadMultiply;
                case Key::Divide: return ImGuiKey_KeypadDivide;

                case Key::Escape: return ImGuiKey_Escape;
                case Key::Space: return ImGuiKey_Space;
                case Key::Enter: return ImGuiKey_Enter;
                case Key::Backspace: return ImGuiKey_Backspace;
                case Key::Tab: return ImGuiKey_Tab;
                case Key::Delete: return ImGuiKey_Delete;
                case Key::Insert: return ImGuiKey_Insert;
                case Key::Home: return ImGuiKey_Home;
                case Key::End: return ImGuiKey_End;
                case Key::PageUp: return ImGuiKey_PageUp;
                case Key::PageDown: return ImGuiKey_PageDown;
                case Key::Left: return ImGuiKey_LeftArrow;
                case Key::Right: return ImGuiKey_RightArrow;
                case Key::Up: return ImGuiKey_UpArrow;
                case Key::Down: return ImGuiKey_DownArrow;
                case Key::Menu: return ImGuiKey_Menu;
                case Key::Pause: return ImGuiKey_Pause;
                case Key::Printscreen: return ImGuiKey_PrintScreen;

                case Key::LShift: return ImGuiKey_LeftShift;
                case Key::RShift: return ImGuiKey_RightShift;
                case Key::LControl: return ImGuiKey_LeftCtrl;
                case Key::RControl: return ImGuiKey_RightCtrl;
                case Key::LAlt: return ImGuiKey_LeftAlt;
                case Key::RAlt: return ImGuiKey_RightAlt;
                case Key::LSystem: return ImGuiKey_LeftSuper;
                case Key::RSystem: return ImGuiKey_RightSuper;

                case Key::Apostrophe: return ImGuiKey_Apostrophe;
                case Key::Comma: return ImGuiKey_Comma;
                case Key::Hyphen: return ImGuiKey_Minus;
                case Key::Period: return ImGuiKey_Period;
                case Key::Slash: return ImGuiKey_Slash;
                case Key::Semicolon: return ImGuiKey_Semicolon;
                case Key::Equal: return ImGuiKey_Equal;
                case Key::LBracket: return ImGuiKey_LeftBracket;
                case Key::Backslash: return ImGuiKey_Backslash;
                case Key::RBracket: return ImGuiKey_RightBracket;
                case Key::Tilde: return ImGuiKey_GraveAccent;

                case Key::F1: return ImGuiKey_F1;
                case Key::F2: return ImGuiKey_F2;
                case Key::F3: return ImGuiKey_F3;
                case Key::F4: return ImGuiKey_F4;
                case Key::F5: return ImGuiKey_F5;
                case Key::F6: return ImGuiKey_F6;
                case Key::F7: return ImGuiKey_F7;
                case Key::F8: return ImGuiKey_F8;
                case Key::F9: return ImGuiKey_F9;
                case Key::F10: return ImGuiKey_F10;
                case Key::F11: return ImGuiKey_F11;
                case Key::F12: return ImGuiKey_F12;

                default: return ImGuiKey_None;
            }
        }

        int to_imgui_mouse_button(mgl::Mouse::Button button) {
            switch(button) {
                case mgl::Mouse::Left: return 0;
                case mgl::Mouse::Right: return 1;
                case mgl::Mouse::Middle: return 2;
                case mgl::Mouse::XButton1: return 3;
                case mgl::Mouse::XButton2: return 4;
                default: return -1;
            }
        }

        // ImGui 要求后端显式提交修饰键状态
        void submit_modifiers(const mgl_key_states& states) {
            ImGuiIO& io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Ctrl, states.control);
            io.AddKeyEvent(ImGuiMod_Shift, states.shift);
            io.AddKeyEvent(ImGuiMod_Alt, states.alt);
            io.AddKeyEvent(ImGuiMod_Super, states.system);
        }

        uint32_t modifiers_from_states(const mgl_key_states& s) {
            uint32_t m = 0;
            if(s.control) m |= (HOTKEY_MOD_LCTRL | HOTKEY_MOD_RCTRL);
            if(s.shift)   m |= (HOTKEY_MOD_LSHIFT | HOTKEY_MOD_RSHIFT);
            if(s.alt)     m |= (HOTKEY_MOD_LALT | HOTKEY_MOD_RALT);
            if(s.system)  m |= (HOTKEY_MOD_LSUPER | HOTKEY_MOD_RSUPER);
            return m;
        }

        bool is_modifier_key(mgl::Keyboard::Key k) {
            using Key = mgl::Keyboard::Key;
            return k == Key::LControl || k == Key::RControl ||
                   k == Key::LShift   || k == Key::RShift   ||
                   k == Key::LAlt     || k == Key::RAlt     ||
                   k == Key::LSystem  || k == Key::RSystem;
        }
    }

    HotkeyCapture& hotkey_capture() {
        static HotkeyCapture inst;
        return inst;
    }

    struct Host::Impl {
        bool initialized = false;
        bool font_texture_uploaded = false;
        bool fonts_loaded = false;
        ImVector<ImWchar> glyph_ranges;
        std::unique_ptr<mgl::Texture> font_texture; // 用 mgl 自己的纹理机制（这台机器上验证过渲染正常）
        ImFont* fonts[3] = { nullptr, nullptr, nullptr }; // Small / Body / Title
        bool imgui_context_created = false;
        bool backend_initialized = false;
        bool reported_atlas = false;
        int width = 1;
        int height = 1;
        std::chrono::steady_clock::time_point last_frame_time = std::chrono::steady_clock::now();
        std::string font_path;
    };

    Host::Host() : impl(new Impl()) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        impl->imgui_context_created = true;

        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   // 覆盖层不需要 imgui.ini
        io.LogFilename = nullptr;
        io.BackendPlatformName = "gsr-ui (mgl)";
        io.DisplaySize = ImVec2((float)impl->width, (float)impl->height);
        io.DeltaTime = 1.0f / 60.0f;

        // 走 legacy 字体图集路径：图集由我们自己构建并上传纹理。
        // 1.92 的动态纹理路径（后端按需创建图集纹理）在这台机器上会把所有文字画成实心色块，
        // legacy 路径行为完全确定，也方便排查。
        io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;

        if(!ImGui_ImplOpenGL2_Init()) {
            fprintf(stderr, "imgui: ImGui_ImplOpenGL2_Init() failed\n");
            return;
        }

        // 关键：ImGui_ImplOpenGL2_Init() 内部会重新置上 ImGuiBackendFlags_RendererHasTextures，
        // 所以必须在 Init 之后再清一次，否则图集仍会被当成"动态图集"来构建 —— 只有巴掌大
        // （实测 512x128），运行期遇到未预烘的字形会触发增量栅格化，把 TexIsBuilt 打回 false，
        // 下一帧 NewFrame 直接 assert 崩溃。清掉后图集才会按预烘范围构建（实测 1024x2048）。
        io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;

        impl->backend_initialized = true;
        impl->initialized = true;
        impl->last_frame_time = std::chrono::steady_clock::now();
    }

    Host::~Host() {
        if(impl->backend_initialized)
            ImGui_ImplOpenGL2_Shutdown();
        if(impl->imgui_context_created)
            ImGui::DestroyContext();
        impl->initialized = false;
    }

    bool Host::is_initialized() const {
        return impl->initialized;
    }

    namespace {
        ImFont* g_ui_fonts[3] = { nullptr, nullptr, nullptr };
        float g_ui_scale = 0.0f;   // 0 = 未设定（回退到按 DisplaySize 估）
    }

    ImFont* ui_font(FontRole role) {
        return g_ui_fonts[(int)role];
    }

    float interface_scale() {
        return g_ui_scale;
    }

    void set_ui_scale(float scale) {
        if(scale > 0.0f)
            g_ui_scale = scale;
    }

    /*
        legacy 图集的硬约束：所有字形必须在 Build() 时一次性预烘完，
        运行期任何新的字号/字形都会触发增量栅格化，把 TexIsBuilt 打回 false，
        下一帧 NewFrame 直接 assert。因此这里把界面要用的三档字号全部预烘。
    */
    void Host::load_font() {
        ImGuiIO& io = ImGui::GetIO();

        // legacy 图集不做按需栅格化，必须预烘字形范围：拉丁 + 常用简中 + 日文 + 西里尔
        {
            ImGuiIO& io = ImGui::GetIO();
            ImFontGlyphRangesBuilder builder;
            builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
            builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
            builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
            builder.BuildRanges(&impl->glyph_ranges);
        }

        // 三档字号：小(15) 正文(21) 标题(30)，按显示器分辨率缩放（用户反馈字体偏小，整体上浮）。
        // 缩放由 Overlay 用"显示器高度"显式设定（通知窗口小窗非全屏，不能用它的 DisplaySize 估）。
        const float scale = interface_scale() > 0.0f
            ? interface_scale()
            : std::min(std::max(io.DisplaySize.y / 1080.0f, 0.75f), 2.0f);
        const float base_sizes[3] = { 15.0f, 21.0f, 30.0f };

        for(const FontCandidate& candidate : font_candidates) {
            if(!file_exists(candidate.path))
                continue;

            bool all_ok = true;
            for(int i = 0; i < 3 && all_ok; ++i) {
                ImFontConfig config;
                config.FontNo = candidate.face_index;    // .ttc 里的 face 索引（中文取 SC）
                config.PixelSnapH = true;
                config.SizePixels = base_sizes[i] * scale;

                ImFont* font = io.Fonts->AddFontFromFileTTF(candidate.path, base_sizes[i] * scale, &config,
                    impl->glyph_ranges.Data);
                if(!font || !font->IsGlyphInFont((ImWchar)'A') || !font->IsGlyphInFont((ImWchar)0x5F55)) {
                    all_ok = false;
                    if(font)
                        io.Fonts->RemoveFont(font);
                    impl->fonts[0] = impl->fonts[1] = impl->fonts[2] = nullptr;
                    g_ui_fonts[0] = g_ui_fonts[1] = g_ui_fonts[2] = nullptr;
                    break;
                }
                impl->fonts[i] = font;
                g_ui_fonts[i] = font;
            }

            if(all_ok) {
                impl->font_path = candidate.path;
                fprintf(stderr, "imgui: 使用字体 %s (face %u, 三档字号 %.0f/%.0f/%.0f)\n",
                    candidate.path, candidate.face_index,
                    base_sizes[0] * scale, base_sizes[1] * scale, base_sizes[2] * scale);
                impl->fonts_loaded = true;
                return;
            }
        }

        fprintf(stderr, "imgui: warning: 没有找到可用的中日韩字体，中文会显示为方块\n"
            "        （安装 fonts-noto-cjk / wqy-microhei 后重启即可）\n");
        io.Fonts->AddFontDefault();
        impl->fonts_loaded = true;
    }

#if 0
    void Host::load_font_old() {
        ImGuiIO& io = ImGui::GetIO();

            ImFont* font = io.Fonts->AddFontFromFileTTF(candidate.path, 18.0f, &config, impl->glyph_ranges.Data);
            if(!font) {
                fprintf(stderr, "imgui: 无法加载字体 %s（face %u）\n", candidate.path, candidate.face_index);
                continue;
            }

            // 关键校验：FontNo 选错时字体"加载成功"但一个字形都没有，
            // 界面上表现为所有文字都变成实心色块。必须在这里验证，不合格就换下一个候选。
            const bool has_latin = font->IsGlyphInFont((ImWchar)'A');
            const bool has_cjk = font->IsGlyphInFont((ImWchar)0x5F55); /* 录 */
            if(has_latin && has_cjk) {
                impl->font_path = candidate.path;
                fprintf(stderr, "imgui: 使用字体 %s (face %u)\n", candidate.path, candidate.face_index);
                return;
            }

            fprintf(stderr, "imgui: 字体 %s (face %u) 缺少字形（拉丁=%d 中日韩=%d），换下一个候选\n",
                candidate.path, candidate.face_index, (int)has_latin, (int)has_cjk);
            io.Fonts->RemoveFont(font);
        }

        fprintf(stderr, "imgui: warning: 没有找到可用的中日韩字体，中文会显示为方块\n"
            "        （安装 fonts-noto-cjk / wqy-microhei 后重启即可）\n");
        io.Fonts->AddFontDefault();
    }
#endif

    /*
        legacy 图集路径：构建字形图集并自己上传成 GL 纹理。
        绘制命令通过 atlas->TexID 引用这个纹理，后端只负责 glBindTexture。
    */
    void Host::upload_font_texture() {
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        if(!atlas)
            return;

        // 已上传过的情况下，每次都校验纹理是否仍然有效：
        // 覆盖层隐藏/再显示可能会重建 GL 上下文或删除纹理，旧纹理 id 会失效（表现为文字全变白块）。
        if(impl->font_texture_uploaded) {
            const unsigned int current_id = (unsigned int)(intptr_t)atlas->TexRef.GetTexID();
            if(current_id != 0 && glIsTexture(current_id) == GL_TRUE)
                return;

            fprintf(stderr, "imgui: 字体图集纹理已失效(id=%u)，重新上传\n", current_id);
            impl->font_texture_uploaded = false;
            impl->font_texture.reset();
        }

        // 关键：构建图集前必须让 atlas 认为自己处于 legacy 模式，
        // 否则首次 Build 走动态路径（不预烘字形范围），图集只有巴掌大，
        // 之后设置页等出现新汉字时触发增量栅格化 → TexIsBuilt=false → NewFrame 断言崩溃。
        atlas->RendererHasTextures = false;

        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        atlas->GetTexDataAsRGBA32(&pixels, &width, &height);
        if(!pixels || width <= 0 || height <= 0)
            return;

        unsigned int texture_id = 0;
        const char* via = nullptr;

        // 首选：走 mgl 自己的纹理上传。抓屏桌面背景就是用它渲染的，在这台机器上验证过正常。
        impl->font_texture = std::make_unique<mgl::Texture>();
        if(impl->font_texture->load_from_memory(pixels, width, height, MGL_IMAGE_FORMAT_RGBA,
                {false, false, MGL_TEXTURE_SCALE_LINEAR})) {
            texture_id = impl->font_texture->internal_texture()->id;
            via = "mgl";
        }

        // 兜底：直连 GL 上传
        if(texture_id == 0) {
            glGenTextures(1, &texture_id);
            if(texture_id == 0)
                return;
            glBindTexture(GL_TEXTURE_2D, texture_id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            via = "gl";
        }

        // legacy 图集路径：绘制命令引用 atlas 的 TexID；两条引用都设置，保证一致。
        // 注意：TexData 设了 TexID 就必须同时把 Status 置为 OK，
        // 否则每帧 NewFrame 的校验会 abort："set TexID but did not update Status to OK"。
        atlas->TexID = (ImTextureID)(intptr_t)texture_id;
        if(atlas->TexData) {
            atlas->TexData->SetTexID((ImTextureID)(intptr_t)texture_id);
            atlas->TexData->SetStatus(ImTextureStatus_OK);
        }

        impl->font_texture_uploaded = true;
        fprintf(stderr, "imgui: 字体图集已上传 %dx%d via %s (纹理 id=%u)\n",
            width, height, via, texture_id);
    }

    void Host::set_display_size(int width, int height) {
        if(width <= 0 || height <= 0)
            return;

        impl->width = width;
        impl->height = height;

        if(impl->initialized)
            ImGui::GetIO().DisplaySize = ImVec2((float)width, (float)height);
    }

    void Host::set_mouse_position(float x, float y) {
        if(impl->initialized)
            ImGui::GetIO().AddMousePosEvent(x, y);
    }

    bool Host::handle_event(mgl::Event& event) {
        if(!impl->initialized)
            return false;

        ImGuiIO& io = ImGui::GetIO();

        switch(event.type) {
            case mgl::Event::MouseMoved:
                io.AddMousePosEvent((float)event.mouse_move.x, (float)event.mouse_move.y);
                submit_modifiers(event.mouse_move.key_states);
                break;

            case mgl::Event::MouseButtonPressed: {
                const int button = to_imgui_mouse_button(event.mouse_button.button);
                if(button < 0)
                    return false;
                io.AddMousePosEvent((float)event.mouse_button.x, (float)event.mouse_button.y);
                io.AddMouseButtonEvent(button, true);
                submit_modifiers(event.mouse_button.key_states);
                break;
            }

            case mgl::Event::MouseButtonReleased: {
                const int button = to_imgui_mouse_button(event.mouse_button.button);
                if(button < 0)
                    return false;
                io.AddMouseButtonEvent(button, false);
                submit_modifiers(event.mouse_button.key_states);
                break;
            }

            case mgl::Event::MouseWheelScrolled:
                // mgl: 正数=向上；ImGui: wheel_y 正数=向上
                io.AddMouseWheelEvent(0.0f, (float)event.mouse_wheel_scroll.delta);
                submit_modifiers(event.mouse_wheel_scroll.key_states);
                break;

            case mgl::Event::TextEntered:
                io.AddInputCharactersUTF8(event.text.str);
                break;

            case mgl::Event::KeyPressed: {
                // 热键捕获：等待下一次非修饰键，连同当前修饰状态一起记录
                HotkeyCapture& cap = hotkey_capture();
                if(cap.active) {
                    if(event.key.code == mgl::Keyboard::Key::Escape) {
                        cap.cancelled = true;
                        cap.active = false;
                        break;
                    }
                    if(!is_modifier_key(event.key.code)) {
                        cap.key = (int64_t)event.key.code;
                        cap.modifiers = modifiers_from_states(event.key.key_states);
                        cap.active = false;
                        break;
                    }
                    // 纯修饰键：保持等待，等主键
                }
                const ImGuiKey key = to_imgui_key(event.key.code);
                if(key != ImGuiKey_None)
                    io.AddKeyEvent(key, true);
                submit_modifiers(event.key.key_states);
                break;
            }

            case mgl::Event::KeyReleased: {
                const ImGuiKey key = to_imgui_key(event.key.code);
                if(key != ImGuiKey_None)
                    io.AddKeyEvent(key, false);
                submit_modifiers(event.key.key_states);
                break;
            }

            case mgl::Event::Resized:
                set_display_size(event.size.width, event.size.height);
                break;

            default:
                return false;
        }

        // 界面是否想独占这次输入，由调用方决定是否放行（这里只把事件喂进去）
        return io.WantCaptureMouse || io.WantCaptureKeyboard;
    }

    void Host::begin_frame() {
        if(!impl->initialized)
            return;

        const auto now = std::chrono::steady_clock::now();
        float delta = std::chrono::duration<float>(now - impl->last_frame_time).count();
        impl->last_frame_time = now;

        // 长时间没有绘制（例如覆盖层隐藏）时钳制一下，避免动画跳变
        if(delta <= 0.0f || delta > 0.25f)
            delta = 1.0f / 60.0f;

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)impl->width, (float)impl->height);
        io.DeltaTime = delta;

        // 防御性再清一次：只要这个 flag 是置上的，字体图集就会被当成"动态图集"构建
        // （只有巴掌大），运行期新字形触发增量栅格化 → 下一帧 NewFrame 断言崩溃。
        // 万一后端/其它代码重新置上它，这里保证每一帧都回到 legacy 路径。
        io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;

        ImGui_ImplOpenGL2_NewFrame();
        if(!impl->fonts_loaded)
            load_font();             // 需要先知道分辨率，才能确定三档字号的缩放
        upload_font_texture();       // 构建图集并上传（legacy），首帧前完成
        ImGui::NewFrame();
    }

    void Host::end_frame() {
        if(!impl->initialized)
            return;

        ImGui::Render();
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

        // 第一次渲染后打印图集状态：便于确认纹理已经上传并被引用
        if(!impl->reported_atlas) {
            impl->reported_atlas = true;
            ImFontAtlas* atlas = ImGui::GetIO().Fonts;
            ImFont* font = ImGui::GetFont();
            fprintf(stderr, "imgui: 图集 %dx%d texid=%lld | 字形(拉丁=%d 中日韩=%d) | 已上传=%d\n",
                atlas && atlas->TexData ? atlas->TexData->Width : -1,
                atlas && atlas->TexData ? atlas->TexData->Height : -1,
                atlas ? (long long)(intptr_t)atlas->TexRef.GetTexID() : -1LL,
                font ? (int)font->IsGlyphInFont((ImWchar)'A') : -1,
                font ? (int)font->IsGlyphInFont((ImWchar)0x5F55) : -1,
                (int)impl->font_texture_uploaded);
        }

        restore_mgl_state();
    }

    void Host::restore_mgl_state() {
        glViewport(0, 0, impl->width, impl->height);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, (double)impl->width, (double)impl->height, 0.0, 0.0, 1.0);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glDisable(GL_STENCIL_TEST);

        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        glEnable(GL_SCISSOR_TEST);
        glEnable(GL_TEXTURE_2D);

        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
    }

    void Host::apply_theme(unsigned int tint_color_rgba, unsigned int page_bg_color_rgba) {
        if(!impl->initialized)
            return;

        ImGuiStyle& style = ImGui::GetStyle();

        const ImVec4 tint = ImGui::ColorConvertU32ToFloat4(tint_color_rgba);
        const ImVec4 page_bg = ImGui::ColorConvertU32ToFloat4(page_bg_color_rgba);
        const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);

        style.WindowRounding = 10.0f;
        style.FrameRounding = 8.0f;
        style.GrabRounding = 8.0f;
        style.PopupRounding = 8.0f;
        style.ScrollbarRounding = 8.0f;
        style.WindowBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowPadding = ImVec2(20.0f, 16.0f);
        style.FramePadding = ImVec2(12.0f, 8.0f);
        style.ItemSpacing = ImVec2(10.0f, 10.0f);

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg] = page_bg;
        colors[ImGuiCol_ChildBg] = ImVec4(0.14f, 0.17f, 0.18f, 1.0f);      // #262b2f，与旧 UI 的按钮底色一致
        colors[ImGuiCol_PopupBg] = page_bg;
        colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
        colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        colors[ImGuiCol_TextDisabled] = ImVec4(1.0f, 1.0f, 1.0f, 0.45f);
        colors[ImGuiCol_Button] = ImVec4(tint.x, tint.y, tint.z, 0.22f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(tint.x, tint.y, tint.z, 0.55f);
        colors[ImGuiCol_ButtonActive] = tint;
        colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.17f, 0.18f, 1.0f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(tint.x, tint.y, tint.z, 0.35f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(tint.x, tint.y, tint.z, 0.65f);
        colors[ImGuiCol_Header] = ImVec4(tint.x, tint.y, tint.z, 0.35f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(tint.x, tint.y, tint.z, 0.55f);
        colors[ImGuiCol_HeaderActive] = tint;
        colors[ImGuiCol_ScrollbarBg] = transparent;
        colors[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);
        colors[ImGuiCol_TitleBg] = page_bg;
        colors[ImGuiCol_TitleBgActive] = page_bg;
    }

}
}
