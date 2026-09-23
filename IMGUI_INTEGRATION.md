# Dear ImGui 界面层集成说明

本文件记录把 gpu-screen-recorder-ui 的界面换成 Dear ImGui 的现状、用法与约束。
（`RMLUI_INTEGRATION.md` 是之前尝试 RmlUi 的记录，已弃用，保留作参考。）

## 为什么是 ImGui

* **能借用自己的 GL 上下文**：ImGui 只产出绘制数据，由它自带的 OpenGL 后端画进 mgl 现有上下文，不建窗口、不抢事件循环。
* **依赖最轻**：v1.92.9b 只有 5 个源文件，`-Dimgui=true` 后二进制 13.7 MB → 18.1 MB（RmlUi 方案是 58.8 MB）。
* **中日文开箱可用**：1.92 起是动态字体（按需栅格化），不需要预烘字形范围，配 `ImFontConfig::FontNo` 选对 `.ttc` face 即可。
* **界面是 C++ 代码**：文案直接用项目现有的 `TR()` 宏，切换语言下一帧生效，不需要额外的翻译文件桥接。

## 构建与运行

**ImGui 界面现在是默认启用的**（`-Dimgui` 默认 true），要回到旧界面用 `-Dimgui=false`。

```sh
# 新界面（默认）
meson setup build && ninja -C build
./build/gsr-ui launch-show

# 旧界面（对照用）
meson setup build-old -Dimgui=false && ninja -C build-old

# 安装（install.sh 会 rm -rf build 后重新配置，因此会带上新默认值）
sudo ./install.sh
```

无显示器环境下的校验（检查字体加载、字形可用性、能否产出绘制数据）：

```sh
./build/imgui-check
# 字体 : NotoSansCJK-Regular.ttc (face 2) / 字形可用 5/5 / 顶点数 > 0 → 通过
```

### 跑起来还是老界面？先查这三件事

新界面没生效时，**绝大多数情况不是代码问题**，而是跑错了二进制或旧实例还在跑：

1. **`gsr-ui` 解析到哪个二进制**
   `which -a gsr-ui`。发行版可能装了旧版到 `/usr/bin/gsr-ui`（本项目遇到的是 1.12.1），
   而 `/usr/local/bin` 往往不在 PATH 前面，于是敲 `gsr-ui` 跑的是旧包。
   → 直接用绝对路径跑新构建，或确认 PATH 顺序。
2. **是否已经有一个旧实例在跑**
   已经有一个 gsr-ui 运行时，新二进制启动会检测到它、只发送 `show_ui` 给**旧实例**然后退出
   （见 `src/main.cpp` 的 RPC 分支）。
   → 先 `pkill -x gsr-ui`，或用旧界面里的"退出程序"，再启动新构建。
3. **构建时是否真的启用了 ImGui**
   看启动日志：会打印
   `Info: 使用 ImGui 界面（主页由 ImGui 绘制）` 或
   `Info: 使用原界面（本次构建未启用 ImGui，如需新界面请用 -Dimgui=true 重新构建）`。
   另外主页首次由 ImGui 绘制时会打印 `Info: ImGui 已接管主页渲染（W x H）`；
   若因 `page_stack` 深度 > 1（正在显示尚未移植的旧设置页）而暂不接管，也会打印一次原因。

另外注意自启动项 `~/.config/autostart/gpu-screen-recorder-ui.desktop` 里的
`Exec=gsr-ui launch-daemon` 会拉起 PATH 中那个 gsr-ui——想让常驻的是新版，把它改成绝对路径。

## 关键约束（都是实测踩出来的）

### 1. 必须用 `imgui_impl_opengl2`，不能用 opengl3

mgl 是 `EGL_CONTEXT_CLIENT_VERSION=2` + `EGL_OPENGL_API` 的 **OpenGL 2.x 兼容上下文**，
符号表里没有 VAO/FBO/`glActiveTexture`。`imgui_impl_opengl3` 需要 VAO 与 GL3 着色器，
所以用固定管线的 `backends/imgui_impl_opengl2.cpp`（客户端数组 + 矩阵栈）。

### 2. `imgui.h` 不能先于 X11 头被包含

* `X11/Xlib.h:79` 是 **`#define Status int`**（宏），而 `imgui.h:3528` 主动 `#undef Status`
  （源码注释写着 "X11 headers are leaking this"）。
* 结果：一旦 `imgui.h` 先被包含，后面的 `X11/Xutil.h`、`X11/XKBlib.h` 全部报
  `error: 'Status' does not name a type`。
* 因此 **`src/Overlay.cpp` 里绝不能 `#include <imgui.h>`**；需要 `IM_COL32` 之类时，
  用本地的 `pack_color()`（同样的字节序）替代。任何同时用 X11 和 imgui 的新文件，
  必须**先包含 X11 头**。

### 3. 渲染后要还原 mgl 的 GL 状态

mgl 的状态只在上下文创建时设置一次（`mgl_graphics_make_context_current`），投影只在窗口 resize 时设置。
ImGui 的 GL2 后端会保存/恢复大部分状态（纹理、视图口、scissor、矩阵栈、多边形模式），
但**不恢复混合模式**，而 mgl 用的是 `glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`。
所以 `Host::end_frame()` 末尾必须调用 `restore_mgl_state()`，否则下一帧现有自绘 UI 会整体变色/错位。

### 4. 输入要手动喂，修饰键要显式提交

没有用平台后端，`Host::handle_event()` 把 `mgl::Event` 翻译成：
`AddMousePosEvent` / `AddMouseButtonEvent` / `AddMouseWheelEvent` / `AddKeyEvent` / `AddInputCharactersUTF8`，
并且按 ImGui 要求每个事件都显式提交 `AddKeyEvent(ImGuiMod_Ctrl/Shift/Alt/Super, ...)`。

## 界面实现与美化

界面在 `src/imgui/HomePage.cpp`，**完全自绘**（`ImDrawList`），不用 ImGui 的默认控件。
理由：默认控件是"开发者工具"观感；ImGui 的价值在于提供绘制、命中测试、动画状态存储、字体图集这些基础设施。

已实现的视觉细节：

| 项 | 做法 |
| --- | --- |
| 卡片 | `AddRectFilled` 圆角 12dp + `AddRect` 描边 + 左侧强调色条 |
| 阴影 | 3 层递增扩散的半透明圆角矩形（ImGui 无 blur，这是成本最低的近似） |
| 悬停动画 | `GetStateStorage()->GetFloatRef` 存插值进度，按 `io.DeltaTime` 指数趋近；悬停上浮 2dp、底色渐入、边框转为强调色 |
| 按下反馈 | `IsItemActive()` 驱动轻微收缩（scale 0.98 的近似） |
| 状态强调 | 激活时卡片叠加强调色、状态文字用强调色、按钮变实心 |
| 图标 | **矢量绘制**（`AddCircle/AddCircleFilled/PathArcTo/AddRect`），不依赖图标字体，任意 DPI 清晰；替代了 `images/*.png` 那批固定尺寸精灵 |
| 快捷键 | 按键帽样式的小圆角标签（`AddRectFilled` + 描边 + 文字） |
| 文字清晰度 | 编入 `misc/freetype/imgui_freetype.cpp` 并定义 `IMGUI_ENABLE_FREETYPE`（FreeType 光栅化 + hinting） |
| 缩放 | 以 1080p 为基准算 `scale = display.y / 1080`，用 `PushFont(font, size * scale)` 请求对应字号的字形（动态字体按需栅格化，不糊） |

字体：`src/imgui/Host.cpp` 按候选列表加载并选 `face_index = 2`（Noto CJK 的 `.ttc` 顺序为 0=JP 1=KR **2=SC** 3=TC 4=HK，
用 `fc-query -f '%{family}\n' 文件` 可确认），中文才不会出现日文字形。

## 事件路由与旧界面的关系

* `Overlay::imgui_owns_screen()` = `page_stack.size() <= 1`：**只有停留最外层主页时由 ImGui 接管**。
* 主页由 ImGui 画，且鼠标/滚轮/文本事件由 ImGui 独占（`on_event` 里直接 `return`），避免同时触发下面看不见的旧控件；
  **键盘继续放行**，保证 Esc、Alt+Z 等既有快捷键仍然生效。
* 打开设置页时 `page_stack.size() > 1`，改由旧的 Widget 栈绘制与处理事件——设置页尚未移植，
  这是当前唯一的"新旧并存"处（为此给 `PageStack` 加了 `size()`）。

## 待办

* 把设置页（`SettingsPage` 2017 行、`GlobalSettingsPage` 921 行、`ScreenshotSettingsPage`）搬到 ImGui：
  这类表单用 `BeginChild` + 表格 + ImGui 的输入控件比手算坐标省事得多。
* 手柄导航：`io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad`，把现有 `/dev/input/jsN` 事件
  用 `AddKeyAnalogEvent(ImGuiKey_GamepadDpad*)` 喂进去。
* 文本输入与 IME：`Entry` 控件与文件选择器搜索框需要接 XIM（ImGui 需要自己实现 `SetPlatformImeDataFn`）。
* 剪贴板/鼠标光标：接 `Clipboard/` 与 X11 现有实现。
* 通知与页面过渡动画：目前通知仍是旧实现（`Overlay::show_notification`），可搬过来统一。
