#include <SDL2/SDL.h>
#include <SDL2/SDL_messagebox.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "../Supervisor.hpp"
#include "../i18n.hpp"
#include "../inttypes.hpp"
#include "Widgets.hpp"

constexpr SDL_Color kBackgroundColor{43, 43, 43, 255};

namespace FileSystem
{
// 注意：这里**不能用 fopen**。configPath 是 "東方紅魔郷.cfg"，源码以 UTF-8 存储，
// 而 MinGW CRT 的 fopen 会把路径当作当前 ANSI 代码页（中文 Windows = CP936/GBK）解析，
// 导致文件名变形、fopen 直接失败（表現为「无法写入配置文件」）。
// 主游戏之所以没事，是因为它用的是项目自带的 FileSystem::FopenUTF8()（内部 UTF-8 -> UTF-16 -> _wfopen）。
// config 是独立小工具，为免把 FileSystem.cpp 及其依赖链拉进来，
// 这里改用 SDL_RWops：SDL 在 Windows 上同样把 UTF-8 转成 UTF-16 再调 CreateFileW，且跨平台行为一致。
unsigned char *OpenPath(const char *filepath, size_t *outSize)
{
    SDL_RWops *rw = SDL_RWFromFile(filepath, "rb");
    if (!rw)
    {
        return nullptr;
    }

    const Sint64 size64 = SDL_RWsize(rw);
    if (size64 <= 0)
    {
        SDL_RWclose(rw);
        return nullptr;
    }

    auto *data = (unsigned char *)malloc((size_t)size64);
    if (!data)
    {
        SDL_RWclose(rw);
        return nullptr;
    }

    if (SDL_RWread(rw, data, 1, (size_t)size64) != (size_t)size64)
    {
        free(data);
        SDL_RWclose(rw);
        return nullptr;
    }

    SDL_RWclose(rw);
    *outSize = (size_t)size64;
    return data;
}

int WriteDataToFile(const char *path, void *data, size_t size)
{
    SDL_RWops *rw = SDL_RWFromFile(path, "wb");
    if (!rw)
    {
        return -1;
    }

    if (SDL_RWwrite(rw, data, 1, size) != size)
    {
        SDL_RWclose(rw);
        return -2;
    }

    SDL_RWclose(rw);
    return 0;
}
} // namespace FileSystem

class ConfigUI
{
  public:
    ConfigUI(GameConfiguration &cfg, SDL_Window *window, SDL_Renderer *renderer, TTF_Font *font, const char *cfgPath)
        : cfg(cfg), window(window), renderer(renderer), font(font), configPath(cfgPath),
          ctx(renderer, font, kBackgroundColor)
    {
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);
        buildLayout();
    }

    void run()
    {
        while (!shouldExit)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_QUIT)
                {
                    requestQuit();
                    continue;
                }

                for (auto &binding : checkboxBindings)
                {
                    binding.widget.handleEvent(event);
                }
                for (auto &binding : radioBindings)
                {
                    binding.widget.handleEvent(event);
                }
                for (auto &button : bottomButtons)
                {
                    button.handleEvent(event);
                }
            }

            SET_DRAW_COLOR(renderer, ctx.Background());

            SDL_RenderClear(renderer);

            for (auto &binding : checkboxBindings)
            {
                binding.widget.render(ctx);
            }
            for (auto &binding : radioBindings)
            {
                binding.widget.render(ctx);
            }
            for (const auto &box : groupBoxes)
            {
                box.render(ctx);
            }
            for (auto &button : bottomButtons)
            {
                button.render(ctx);
            }

            SDL_RenderPresent(renderer);
        }
    }

  private:
    struct CheckboxBinding
    {
        Checkbox widget;
        int bit;

        CheckboxBinding(Checkbox &&widget, int bit) : widget(std::move(widget)), bit(bit)
        {
        }
    };

    struct RadioBinding
    {
        RadioButton widget;
        int groupId;
        int optionId;

        RadioBinding(RadioButton &&widget, int groupId, int optionId)
            : widget(std::move(widget)), groupId(groupId), optionId(optionId)
        {
        }
    };

    GameConfiguration &cfg;
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *font;
    const char *configPath;
    UIContext ctx;

    bool hasUnsavedChanges = false;
    bool shouldExit = false;
    int windowWidth = 0;
    int windowHeight = 0;

    std::vector<CheckboxBinding> checkboxBindings;
    std::vector<RadioBinding> radioBindings;
    std::vector<GroupBox> groupBoxes;
    std::vector<Button> bottomButtons;

    void buildLayout()
    {
        buildCheckboxes();
        buildRadioGroups();
        buildBottomButtons();
    }

    void buildCheckboxes()
    {
        struct CheckboxSpec
        {
            const char *label;
            int bit;
        };

        // UI 文案（已汉化）。这些都是原版 D3D 时代的图形选项，
        // 在 SDL2 portable 分支里多数已无实际作用；真正有效的一般是画面模式与帧率。
        const CheckboxSpec specs[11] = {
            {"禁用顶点缓冲", GCOS_DONT_USE_VERTEX_BUF},
            {"禁用雾效", GCOS_DONT_USE_FOG},
            {"16 位纹理", GCOS_FORCE_16BIT_COLOR_MODE},
            {"禁用高洛德着色", GCOS_SUPPRESS_USE_OF_GOROUD_SHADING},
            {"禁用颜色合成", GCOS_NO_COLOR_COMP},
            {"参考光栅器模式启动", GCOS_REFERENCE_RASTERIZER_MODE},
            {"刷新时清除后台缓冲", GCOS_CLEAR_BACKBUFFER_ON_REFRESH},
            {"最低画质显示", GCOS_DISPLAY_MINIMUM_GRAPHICS},
            {"禁用深度测试", GCOS_TURN_OFF_DEPTH_TEST},
            {"强制 60 FPS", GCOS_FORCE_60FPS},
            {"禁用 DirectInput 手柄", GCOS_NO_DIRECTINPUT_PAD},
        };

        const int checkboxCount = sizeof(specs) / sizeof(specs[0]);
        checkboxBindings.reserve(checkboxCount);

        int top = 16;
        for (const auto &spec : specs)
        {
            SDL_Rect rect{15, top, 16, 16};
            bool initial = (cfg.opts >> spec.bit) & 1;
            checkboxBindings.emplace_back(Checkbox(rect, spec.label, initial, font, &hasUnsavedChanges), spec.bit);
            top += 24;
        }
    }

    void buildRadioGroups()
    {
        struct RadioGroupSpec
        {
            const char *label;
            std::vector<const char *> options;
            int spacing;
            int groupId;
            int initialIndex;
        };

        // 1/1 1/2 1/3 保留数字形式：Supervisor.hpp 注释写的是 0=fullspeed/1=1/2/2=1/4，
        // 与这里显示的 "1/3" 对不上，不擅自改写其语义。
        const RadioGroupSpec radioGroup[]{{"画面模式", {"全屏", "窗口"}, 130, 0, cfg.windowed},
                                          {"帧率", {"1/1", "1/2", "1/3"}, 70, 1, cfg.frameskipConfig},
                                          {"色彩位数", {"32 位色", "16 位色"}, 120, 2, cfg.colorMode16bit}};

        int totalRadioButtons = 0;
        for (const auto &group : radioGroup)
        {
            totalRadioButtons += static_cast<int>(group.options.size());
        }
        radioBindings.reserve(totalRadioButtons);

        constexpr int boxWidth = 360;
        constexpr int boxHeight = 56;
        constexpr int left = 15;
        int top = 16 + checkboxBindings.size() * 24 + 20;

        for (const auto &group : radioGroup)
        {
            SDL_Rect boxRect{left, top, boxWidth, boxHeight};
            groupBoxes.push_back({boxRect, group.label});

            const int buttonY = boxRect.y + (boxRect.h / 2) - 8;
            int buttonX = boxRect.x + 18;

            for (int i = 0; i < group.options.size(); ++i)
            {
                SDL_Rect rbRect{buttonX, buttonY, 16, 16};
                bool selected = (i == group.initialIndex);
                radioBindings.emplace_back(
                    RadioButton{rbRect, group.options[i], group.groupId, i, selected, font, &hasUnsavedChanges},
                    group.groupId, i);
                RadioButton &radio = radioBindings.back().widget;
                radio.setOnSelected(
                    [&](int groupId, RadioButton *selectedButton) { onRadioSelected(groupId, selectedButton); });

                buttonX += group.spacing;
            }

            top += boxHeight + 12;
        }
    }

    void buildBottomButtons()
    {
        const int buttonWidth = 100;
        const int buttonHeight = 32;
        const int buttonSpacing = 12;
        const int margin = 20;

        const int top = windowHeight - margin - buttonHeight;
        const int saveX = windowWidth - margin - buttonWidth;
        const int cancelX = saveX - buttonSpacing - buttonWidth;

        bottomButtons.reserve(2);

        bottomButtons.emplace_back(SDL_Rect{cancelX, top, buttonWidth, buttonHeight}, "取消", [&] { requestQuit(); });

        bottomButtons.emplace_back(SDL_Rect{saveX, top, buttonWidth, buttonHeight}, "保存", [&] {
            if (saveConfig())
            {
                shouldExit = true;
            }
        });
    }

    void onRadioSelected(int groupId, RadioButton *selectedButton)
    {
        for (auto &binding : radioBindings)
        {
            if (binding.groupId != groupId)
            {
                continue;
            }

            bool shouldSelect = (&binding.widget == selectedButton);
            binding.widget.setSelectedState(shouldSelect, shouldSelect);
            if (!shouldSelect)
            {
                binding.widget.setSelectedState(false, false);
            }
        }
    }

    void requestQuit()
    {
        shouldExit = true;
    }

    void applyUiToConfig()
    {
        cfg.opts = (1 << GCOS_USE_D3D_HW_TEXTURE_BLENDING);

        for (const auto &binding : checkboxBindings)
        {
            if (binding.widget.isChecked())
            {
                cfg.opts |= (1 << binding.bit);
            }
        }

        auto getSelectedOption = [&](int groupId) -> int {
            for (const auto &binding : radioBindings)
            {
                if (binding.groupId == groupId && binding.widget.isSelected())
                {
                    return binding.optionId;
                }
            }
            return 0;
        };

        cfg.windowed = getSelectedOption(0);
        cfg.frameskipConfig = getSelectedOption(1);
        cfg.colorMode16bit = getSelectedOption(2);
    }

    bool saveConfig()
    {
        applyUiToConfig();
        if (FileSystem::WriteDataToFile(configPath, &cfg, sizeof(GameConfiguration)) != 0)
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "保存失败", "无法写入配置文件。\n（東方紅魔郷.cfg）", window);
            return false;
        }
        hasUnsavedChanges = false;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "保存成功", "配置已保存。", window);
        printf("Saved config to %s\n", configPath);
        return true;
    }
};

// 必须用带参数的签名：SDL 会把 main 宏替换为 SDL_main，
// 而 SDL_main.h 声明的原型是 C 链接的 SDL_main(int, char **)，签名不一致会冲突。
int main(int argc, char *argv[])
{
    const char *configPath = "東方紅魔郷.cfg";

    GameConfiguration cfg;
    size_t configFileSize = 0;
    if (const auto data = FileSystem::OpenPath(configPath, &configFileSize))
    {
        // 必须校验长度：原代码直接 memcpy(sizeof(GameConfiguration)) 而不看实际文件大小，
        // 遇到被截断或损坏的配置文件会越界读。
        memcpy(&cfg, data, configFileSize < sizeof(GameConfiguration) ? configFileSize : sizeof(GameConfiguration));
        free(data);
        cfg.windowed = std::min<u8>(cfg.windowed, 1);
        cfg.frameskipConfig = std::min<u8>(cfg.frameskipConfig, 2);
        cfg.colorMode16bit = std::min<u8>(cfg.colorMode16bit, 1);
    }
    else
    {
        cfg.version = GAME_VERSION;
        cfg.lifeCount = 2;
        cfg.bombCount = 3;
        cfg.colorMode16bit = 0;
        cfg.musicMode = 1;
        cfg.playSounds = 1;
        cfg.defaultDifficulty = 1;
        cfg.windowed = 0;
        cfg.frameskipConfig = 0;
        cfg.padXAxis = 600;
        cfg.padYAxis = 600;
        cfg.opts = (1 << GCOS_USE_D3D_HW_TEXTURE_BLENDING);
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    if (TTF_Init() != 0)
    {
        printf("TTF_Init failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Window *window =
        SDL_CreateWindow("東方紅魔郷 配置工具", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 450, 600, SDL_WINDOW_SHOWN);

    if (!window)
    {
        printf("Failed to create window: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer)
    {
        printf("Failed to create renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    // 字体候选链：优先中文字体，因为 UI 文案已汉化。
    // 注意 msgothic.ttc 是**日文字体，不含简体中文字形**，命中它会整屏豆腐块(□)，
    // 所以它只能作为最后的兜底。Win7+ 都自带微软雅黑，
    // 相对路径失败时还有 C:\Windows\Fonts 绝对路径兜底。
    // 注意本程序是 WindowedApp（无控制台），printf 用户看不见，致命错误必须弹 MessageBox。
    const char *fontCandidates[] = {
        "msyh.ttc",
        "C:\\Windows\\Fonts\\msyh.ttc",
        "simhei.ttf",
        "C:\\Windows\\Fonts\\simhei.ttf",
        "NotoSansSC-Regular.ttf",
        TH_FALLBACK_FONT_FILENAME,
        TH_PRIMARY_FONT_FILENAME,
    };

    TTF_Font *font = NULL;
    for (const char *candidate : fontCandidates)
    {
        font = TTF_OpenFont(candidate, 18);
        if (font != NULL)
        {
            printf("Loaded font: %s\n", candidate);
            break;
        }
        printf("Failed to load font %s: %s\n", candidate, TTF_GetError());
    }

    if (!font)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "缺少字体",
                                 "找不到可用的字体文件。\n\n请在 th06_config.exe 旁边放一个字体：\n"
                                 "  msyh.ttc（微软雅黑，推荐）\n"
                                 "  simhei.ttf（黑体）\n"
                                 "  NotoSansSC-Regular.ttf",
                                 NULL);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    ConfigUI ui = {cfg, window, renderer, font, configPath};
    ui.run();

    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
