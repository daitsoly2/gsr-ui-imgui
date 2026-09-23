project('gsr-ui', ['c', 'cpp'], version : '1.13.9', default_options : ['warning_level=2', 'cpp_std=c++17'], subproject_dir : 'depends')

add_project_arguments('-D_FILE_OFFSET_BITS=64', language : ['c', 'cpp'])

if get_option('buildtype') == 'debug'
    add_project_arguments('-g3', language : ['c', 'cpp'])
elif get_option('buildtype') == 'release'
    add_project_arguments('-DNDEBUG', language : ['c', 'cpp'])
endif

src = [
    'src/window_texture.c',
    'src/Theme.cpp',
    'src/gui/Widget.cpp',
    'src/gui/ScrollablePage.cpp',
    'src/gui/Button.cpp',
    'src/gui/RadioButton.cpp',
    'src/gui/Entry.cpp',
    'src/gui/CheckBox.cpp',
    'src/gui/ComboBox.cpp',
    'src/gui/Page.cpp',
    'src/gui/StaticPage.cpp',
    'src/gui/PageStack.cpp',
    'src/gui/List.cpp',
    'src/gui/Utils.cpp',
    'src/gui/DropdownButton.cpp',
    'src/gui/Label.cpp',
    'src/gui/Image.cpp',
    'src/gui/LineSeparator.cpp',
    'src/gui/CustomRendererWidget.cpp',
    'src/gui/FileChooser.cpp',
    'src/gui/SettingsPage.cpp',
    'src/gui/ScreenshotSettingsPage.cpp',
    'src/gui/GlobalSettingsPage.cpp',
    'src/gui/GsrPage.cpp',
    'src/gui/Subsection.cpp',
    'src/gui/Tooltip.cpp',
    'src/GlobalHotkeys/GlobalHotkeysX11.cpp',
    'src/GlobalHotkeys/GlobalHotkeysLinux.cpp',
    'src/GlobalHotkeys/GlobalHotkeysJoystick.cpp',
    'src/CursorTracker/CursorTrackerX11.cpp',
    'src/CursorTracker/CursorTrackerDrm.cpp',
    'src/CursorTracker/CursorTrackerHyprland.cpp',
    'src/CursorTracker/CursorTrackerNiri.cpp',
    'src/CursorTracker/CursorTrackerSway.cpp',
    'src/RegionSelector/RegionSelectorX11.cpp',
    'src/RegionSelector/RegionSelectorWayland.cpp',
    'src/DesktopEnvironment/DesktopEnvironmentX11.cpp',
    'src/DesktopEnvironment/DesktopEnvironmentWlroots.cpp',
    'src/DesktopEnvironment/DesktopEnvironmentKde.cpp',
    'src/DesktopEnvironment/DesktopEnvironmentGnome.cpp',
    'src/Utils.cpp',
    'src/WindowUtils.cpp',
    'src/WaylandHostBridge.cpp',
    'src/Clipboard/ClipboardTransfer.cpp',
    'src/Clipboard/ClipboardX11.cpp',
    'src/Clipboard/ClipboardWayland.cpp',
    'src/Config.cpp',
    'src/GsrInfo.cpp',
    'src/Process.cpp',
    'src/Overlay.cpp',
    'src/AudioPlayer.cpp',
    'src/Hotplug.cpp',
    'src/ClipboardFile.cpp',
    'src/LedIndicator.cpp',
    'src/Rpc.cpp',
    'src/Translation.cpp',
    'src/main.cpp',
]

subdir('protocol')
src += protocol_src

mglpp_proj = subproject('mglpp', default_options : ['wayland=true'])
mglpp_dep = mglpp_proj.get_variable('mglpp_dep')

# 新的 UI 层（RmlUi / ImGui）都直接链接系统 libGL（与 RmlUi/ImGui 自带后端一致）
gl_dep = dependency('gl')

# Dear ImGui：新的界面层。界面对应 src/imgui/ 下的 C++ 代码，文案直接用 TR()。
# 用 imgui_impl_opengl2（固定管线）而不是 opengl3，因为 mgl 是 GL2 兼容上下文、没有 VAO。
imgui_dep = dependency('', required : false)
imgui_ui_src = []
if get_option('imgui')
    imgui_include_dirs = [
        include_directories('third_party/imgui'),
        include_directories('third_party/imgui/backends'),
    ]

    imgui_sources = [
        'third_party/imgui/imgui.cpp',
        'third_party/imgui/imgui_draw.cpp',
        'third_party/imgui/imgui_tables.cpp',
        'third_party/imgui/imgui_widgets.cpp',
        'third_party/imgui/backends/imgui_impl_opengl2.cpp',
    ]

    # 注意：这里刻意【不】启用 IMGUI_ENABLE_FREETYPE。
    # 实测在 1.92 的动态字体（按需栅格化）路径下，FreeType 加载器会让字形退化成"缺失字形"
    # （界面表现为所有文字都是实心白块）。改用 ImGui 自带 stb_truetype 加载器即可正常。
    imgui_cpp_args = []
    freetype_dep_imgui = dependency('freetype2')

    imgui_ui_src = [
        'src/imgui/Host.cpp',
        'src/imgui/HomePage.cpp',
        'src/imgui/Settings.cpp',
        'src/imgui/NotificationToast.cpp',
    ]
endif

prefix = get_option('prefix')
datadir = get_option('datadir')
gsr_ui_resources_path = join_paths(prefix, datadir, 'gsr-ui')
icons_path = join_paths(prefix, datadir, 'icons')

add_project_arguments('-DGSR_UI_VERSION="' + meson.project_version() + '"', language: ['c', 'cpp'])
add_project_arguments('-DGSR_FLATPAK_VERSION="6.1.2"', language: ['c', 'cpp'])

add_project_arguments('-DKWIN_HELPER_SCRIPT_PATH="' + gsr_ui_resources_path + '/gsrkwinhelper.js"', language: ['c', 'cpp'])
add_project_arguments('-DGNOME_EXTENSION_SOURCE_DIR="' + gsr_ui_resources_path + '/gnome-extension"', language: ['c', 'cpp'])

if get_option('imgui')
    imgui_lib = static_library('imgui',
        imgui_sources,
        include_directories : imgui_include_dirs,
        cpp_args : imgui_cpp_args,
        dependencies : [gl_dep, freetype_dep_imgui],
    )

    imgui_dep = declare_dependency(
        link_with : imgui_lib,
        include_directories : imgui_include_dirs,
        compile_args : imgui_cpp_args,
        dependencies : [gl_dep, freetype_dep_imgui],
    )

    # 离线校验工具：无需显示器/GL 上下文，验证字体加载与中日文字形栅格化
    executable('imgui-check',
        ['tools/imgui-check/main.cpp'],
        dependencies : [imgui_dep],
        install : false,
    )
endif

executable(
    meson.project_name(),
    src + imgui_ui_src,
    install : true,
    dependencies : [
        mglpp_dep,
        imgui_dep,
        dependency('threads'),
        dependency('xcomposite'),
        dependency('xfixes'),
        dependency('xext'),
        dependency('xi'),
        dependency('xcursor'),
        dependency('xrandr'),
        dependency('libpulse-simple'),
        dependency('libdrm'),
        dependency('wayland-client'),
    ],
    cpp_args : [
        '-DGSR_UI_RESOURCES_PATH="' + gsr_ui_resources_path + '"',
    ] + (get_option('imgui') ? ['-DGSR_IMGUI=1'] : []),
)

executable(
    'gsr-global-hotkeys',
    [
        'tools/gsr-global-hotkeys/hotplug.c',
        'tools/gsr-global-hotkeys/keyboard_event.c',
        'tools/gsr-global-hotkeys/keys.c',
        'tools/gsr-global-hotkeys/leds.c',
        'tools/gsr-global-hotkeys/main.c'
    ],
    c_args : '-fstack-protector-all',
    install : true
)

executable(
    'gsr-ui-cli',
    [
        'tools/gsr-ui-cli/main.c'
    ],
    install : true
)

executable(
    'gsr-wayland-bridge',
    [
        'tools/gsr-wayland-bridge/main.c'
    ],
    install : true
)

executable(
    'gsr-kwin-helper',
    [
        'tools/gsr-kwin-helper/main.cpp'
    ],
    install : true,
    dependencies: [
        dependency('dbus-1'),
    ]
)

executable(
    'gsr-gnome-helper',
    [
        'tools/gsr-gnome-helper/main.cpp'
    ],
    install : true,
    dependencies: [
        dependency('dbus-1'),
    ]
)

executable(
    'gsr-game-tracker',
    [
        'tools/gsr-game-tracker/native_games.c',
        'tools/gsr-game-tracker/main.c'
    ],
    install : true
)

install_data(
    'tools/gsr-kwin-helper/gsrkwinhelper.js',
    install_dir: gsr_ui_resources_path,
    install_mode: 'rwxr-xr-x'
)

install_data(
    'tools/gsr-gnome-helper/extension/metadata.json',
    'tools/gsr-gnome-helper/extension/extension.js',
    install_dir: join_paths(gsr_ui_resources_path, 'gnome-extension'),
    install_mode: 'rw-r--r--'
)

install_subdir('images', install_dir : gsr_ui_resources_path)
install_subdir('translations', install_dir : gsr_ui_resources_path)
install_subdir('ui', install_dir : gsr_ui_resources_path)
install_man('gsr-ui.1', 'gsr-ui-cli.1')

if get_option('desktop-files') == true
    install_data(files('gpu-screen-recorder.desktop'), install_dir : join_paths(prefix, datadir, 'applications'))
    install_subdir('icons/hicolor', install_dir : icons_path)

    gnome = import('gnome')
    gnome.post_install(update_desktop_database : true)
endif

if get_option('capabilities') == true
    meson.add_install_script('meson_post_install.sh')
endif
