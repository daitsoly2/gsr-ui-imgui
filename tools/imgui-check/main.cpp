/*
    ImGui 离线校验工具（不需要显示器/GL 上下文）。

    验证三件事：
      1) 中文字体能否加载（Noto CJK 的 .ttc face 选择是否正确）
      2) 中日文/西文字形能否按需栅格化（ImGui 1.92 起是 dynamic font，按需生成字形）
      3) 一帧 UI 能否产出绘制数据（顶点数 > 0）

    用法：
      imgui-check [字体路径]
*/

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

    struct FontCandidate {
        const char* path;
        unsigned int face_index;
    };

    // 与 src/imgui/Host.cpp 保持一致
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

    // 检查若干代表字符能否拿到字形（'录' 中文、'録' 日文、'R' 西文、'ы' 西里尔）
    // 1.92 起字体是动态按需栅格化，用 ImFont::IsGlyphInFont() 查询字形是否存在
    int count_available_glyphs(ImFont* font, const ImWchar* codepoints, int count) {
        int found = 0;
        for(int i = 0; i < count; ++i) {
            if(font->IsGlyphInFont(codepoints[i]))
                ++found;
        }
        return found;
    }

}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== ImGui 校验 ===\n");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.DeltaTime = 1.0f / 60.0f;
    // 与应用保持一致：关掉 RendererHasTextures，走 legacy 图集路径
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;

    // ---- 字体加载 ----
    std::string loaded_path;
    unsigned int loaded_face = 0;
    ImFont* font = nullptr;

    // legacy 图集必须预烘全部字形范围，否则查询未预烘字形会触发增量栅格化，
    // 把 TexIsBuilt 打回 false，下一帧 NewFrame 直接 assert
    ImVector<ImWchar> glyph_ranges;
    {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
        builder.BuildRanges(&glyph_ranges);
    }

    if(argc > 1) {
        ImFontConfig config;
        config.FontNo = 0;
        font = io.Fonts->AddFontFromFileTTF(argv[1], 18.0f, &config, glyph_ranges.Data);
        loaded_path = argv[1];
    } else {
        for(const FontCandidate& candidate : font_candidates) {
            if(!file_exists(candidate.path))
                continue;

            ImFontConfig config;
            config.FontNo = candidate.face_index;
            config.OversampleH = 2;
            config.OversampleV = 1;
            config.PixelSnapH = true;

            font = io.Fonts->AddFontFromFileTTF(candidate.path, 18.0f, &config, glyph_ranges.Data);
            if(font) {
                loaded_path = candidate.path;
                loaded_face = candidate.face_index;
                break;
            }
        }
    }

    if(!font) {
        fprintf(stderr, "失败：没有加载到任何字体\n");
        ImGui::DestroyContext();
        return 1;
    }

    printf("字体     : %s (face %u)\n", loaded_path.c_str(), loaded_face);
    printf("版本     : %s\n\n", IMGUI_VERSION);

    const ImWchar codepoints[] = {
        0x5F55,     // 录（简中）
        0x5236,     // 制（简中）
        0x9332,     // 録（日文汉字）
        0x0052,     // R（西文）
        0x044B,     // ы（西里尔）
    };
    const int glyph_total = (int)(sizeof(codepoints) / sizeof(codepoints[0]));
    const int glyph_found = count_available_glyphs(font, codepoints, glyph_total);

    // ---- 与应用一致：先构建图集、设置 TexID，再 NewFrame ----
    {
        unsigned char* pixels = nullptr;
        int tex_width = 0;
        int tex_height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &tex_width, &tex_height);
        printf("字体图集 : %d x %d  TexIsBuilt=%d\n", tex_width, tex_height, (int)io.Fonts->TexIsBuilt);
        io.Fonts->TexID = (ImTextureID)(intptr_t)1;
        if(io.Fonts->TexData) {
            io.Fonts->TexData->SetTexID((ImTextureID)(intptr_t)1);
            io.Fonts->TexData->SetStatus(ImTextureStatus_OK);
        }
        printf("设置 TexID 后 : TexIsBuilt=%d  TexRef=%lld\n",
            (int)io.Fonts->TexIsBuilt, (long long)(intptr_t)io.Fonts->TexRef.GetTexID());
    }

    // ---- 关键检查：字形是否真的被栅格化进图集 ----
    // 1.92 是动态字体：若字形加载器不工作（例如 FreeType 加载器初始化失败），
    //    FindGlyph() 会返回 U+FFFD 替换字形，而替换字形在图集里是一块"纯白实心方块"，
    //    于是界面上所有文字都渲染成实心色块。这里直接把这件事测出来。
    int glyph_problems = 0;
    {
        ImFontBaked* baked = font->GetFontBaked(18.0f);
        ImTextureData* tex = io.Fonts->TexData;
        if(!baked || !tex) {
            fprintf(stderr, "问题：拿不到 ImFontBaked 或图集纹理数据\n");
            ++glyph_problems;
        } else {
            ImFontGlyph* replacement = baked->FindGlyphNoFallback((ImWchar)0xFFFD);
            const ImWchar samples[] = { 'A', 0x5F55 /*录*/, 0x9332 /*録*/ };
            for(ImWchar codepoint : samples) {
                ImFontGlyph* baked_glyph = baked->FindGlyph(codepoint);
                if(!baked_glyph) {
                    printf("字形 U+%04X : 取不到（NULL）\n", (unsigned)codepoint);
                    ++glyph_problems;
                    continue;
                }

                // 统计该字形在图集里的 alpha 分布：真正的字形 alpha 有 0~255 的变化，
                // 替换字形（白块）则是 255 铺满。
                const int x0 = (int)(baked_glyph->U0 * (float)tex->Width);
                const int x1 = (int)(baked_glyph->U1 * (float)tex->Width);
                const int y0 = (int)(baked_glyph->V0 * (float)tex->Height);
                const int y1 = (int)(baked_glyph->V1 * (float)tex->Height);
                int min_alpha = 255;
                int max_alpha = 0;
                int transparent = 0;
                for(int y = y0; y < y1 && y < tex->Height; ++y) {
                    for(int x = x0; x < x1 && x < tex->Width; ++x) {
                        const unsigned char alpha = tex->Pixels[((size_t)y * (size_t)tex->Width + (size_t)x) * 4 + 3];
                        min_alpha = std::min(min_alpha, (int)alpha);
                        max_alpha = std::max(max_alpha, (int)alpha);
                        if(alpha < 16)
                            ++transparent;
                    }
                }

                const bool is_replacement = replacement && baked_glyph == replacement;
                printf("字形 U+%04X : uv=(%.3f,%.3f)-(%.3f,%.3f) 尺寸=%dx%d advance=%.1f alpha=%d..%d 全透明像素=%d%s\n",
                    (unsigned)codepoint, baked_glyph->U0, baked_glyph->V0, baked_glyph->U1, baked_glyph->V1,
                    std::max(0, x1 - x0), std::max(0, y1 - y0), baked_glyph->AdvanceX,
                    min_alpha, max_alpha, transparent,
                    is_replacement ? "  ← 是替换字形（说明该字形没被栅格化）" : "");

                if(is_replacement || (min_alpha == max_alpha)) {
                    ++glyph_problems;
                }
            }
        }
    }

    // ---- 跑一帧 UI，确认能产出绘制数据 ----
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("check", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    ImGui::TextUnformatted("录制 / 録画 / Recording");
    if(ImGui::Button("Start", ImVec2(200.0f, 60.0f)))
        printf("（按钮被点击，离线校验里不会发生）\n");
    ImGui::End();
    ImGui::Render();

    const ImDrawData* draw_data = ImGui::GetDrawData();
    const int vertex_count = draw_data ? draw_data->TotalVtxCount : 0;
    const int cmd_list_count = draw_data ? draw_data->CmdListsCount : 0;
    const int draw_command_count = draw_data ? draw_data->TotalIdxCount : 0;

    printf("=== 结果 ===\n");
    printf("字形可用 : %d / %d\n", glyph_found, glyph_total);
    printf("顶点数   : %d\n", vertex_count);
    printf("绘制列表 : %d，索引数 %d\n", cmd_list_count, draw_command_count);

    int exit_code = 0;
    if(glyph_found < 3) {
        fprintf(stderr, "问题：中日文/西文字形缺失（字体 face 选错或字体不完整）\n");
        exit_code = 1;
    }
    if(vertex_count <= 0) {
        fprintf(stderr, "问题：没有产出绘制数据\n");
        exit_code = 1;
    }

    printf("结论     : %s\n", exit_code == 0 ? "通过" : "有问题");

    ImGui::DestroyContext();
    return exit_code;
}
