#include "runtime/host_menu.h"
#include "runtime/native_tuning.h"
#include "runtime/ps2_pad.h"
#include "runtime/state_manager.h"

#include "raylib.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(PLATFORM_VITA)
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#endif

namespace hostmenu
{
namespace
{
    Settings g_settings;
    std::atomic<bool> g_open{false};
    bool g_help = false;
    bool g_quit = false;
    bool g_quitArmed = false; // "Quit game" needs a second Cross
    std::atomic<bool> g_holdBlock{false}; // a button pressed in the menu is still held
    int g_tab = 0, g_item = 0;
    Rectangle g_tabRects[8]{};
    std::vector<std::pair<Rectangle, int>> g_rowRects;
    bool g_stateConfirm = false, g_stateConfirmYes = false;
    int g_stateConfirmItem = 0;
    Rectangle g_stateConfirmRects[2]{}; // cancel, confirm
    std::string g_stateMessage;
    double g_lastSelect = -1.0;

#if defined(PLATFORM_VITA)
    const char *kSettingsPath = "ux0:data/celosia/settings.cfg";
#else
    const char *kSettingsPath = "celosia_settings.cfg";
#endif
    constexpr double kDoubleTapSeconds = 0.35;

    // ---- Text -------------------------------------------------------------

    bool portuguese()
    {
        const int choice = g_settings.language.load();
        if (choice != 0)
            return choice == 1;
        static const bool systemPt = []
        {
#if defined(PLATFORM_VITA)
            SceAppUtilInitParam init{};
            SceAppUtilBootParam boot{};
            sceAppUtilInit(&init, &boot);
            int lang = 0;
            sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
            return lang == SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT || lang == SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR;
#else
            const char *lang = std::getenv("LANG");
            return lang && std::strncmp(lang, "pt", 2) == 0;
#endif
        }();
        return systemPt;
    }

    // English text -> Portuguese.
    const char *T(const char *en)
    {
        if (!portuguese())
            return en;
        static const struct
        {
            const char *en, *pt;
        } kTexts[] = {
            {"SETTINGS", "AJUSTES"},
            {"Video", "Vídeo"},
            {"Sound", "Som"},
            {"System", "Sistema"},
            {"About", "Sobre"},
            {"States", "Estados"},
            {"Quick state", "Estado rápido"},
            {"Empty", "Vazio"},
            {"Save", "Salvar"},
            {"Load", "Carregar"},
            {"Delete", "Excluir"},
            {"Cancel", "Cancelar"},
            {"Confirm", "Confirmar"},
            {"Overwrite this state?", "Substituir este estado?"},
            {"Load this state?", "Carregar este estado?"},
            {"Delete this state?", "Excluir este estado?"},
            {"The selected state is empty.", "O estado selecionado está vazio."},
            {"Please wait for the current operation.", "Aguarde a operação atual terminar."},
            {"Loading replaces the current game position and progress. Your memory-card file is preserved.",
             "Carregar substitui a posição e o progresso atuais do jogo. O arquivo do cartão de memória é preservado."},
            {"This replaces the selected state with the current game session.",
             "Isto substitui o estado selecionado pela sessão atual do jogo."},
            {"This removes only the selected state file.", "Isto remove apenas o arquivo do estado selecionado."},
            {"Renderer", "Gráficos"},
            {"Quality", "Qualidade"},
            {"Debug", "Depuração"},
            {"Exact transparency", "Transparências precisas"},
            {"Show FPS", "Mostrar FPS"},
            {"Screen", "Tela"},
            {"Volume", "Volume"},
            {"Language", "Idioma"},
            {"Return to launcher", "Voltar ao launcher"},
            {"On", "Ligado"},
            {"Off", "Desligado"},
            {"Fit (4:3)", "Ajustar (4:3)"},
            {"Stretch", "Esticar"},
            {"Sharp (whole scale)", "Nítido (escala inteira)"},
            {"Automatic", "Automático"},
            {"Confirm again to return", "Confirme de novo para sair"},
            {"L/R: tab   D-pad: choose   X: change   Triangle: help   O: close",
             "L/R: aba   Direcional: escolher   X: mudar   Triângulo: ajuda   O: fechar"},
            {"Triangle / O: back", "Triângulo / O: voltar"},
            {"Draws the effects that use the alpha test exactly like the PS2 (clouds, mist, shadows). "
             "Turning it off gains a few frames per second; some mist and edges look harder.",
             "Desenha os efeitos que usam o teste de alfa exatamente como o PS2 (nuvens, névoa, sombras). "
             "Desligar ganha alguns quadros por segundo; parte da névoa e das bordas fica mais dura."},
            {"Shows the game's frame rate in the corner of the screen.",
             "Mostra a taxa de quadros do jogo no canto da tela."},
            {"Fit keeps the original 4:3 picture with bars on the sides. Stretch fills the screen. "
             "Sharp uses the largest whole multiple of the original size, so every pixel has the same size.",
             "Ajustar mantém a imagem original 4:3 com faixas nas laterais. Esticar preenche a tela. "
             "Nítido usa o maior múltiplo inteiro do tamanho original, então todo pixel tem o mesmo tamanho."},
            {"Volume of the game's sound and music.", "Volume do som e da música do jogo."},
            {"Language of this menu. Automatic follows the system language.",
             "Idioma deste menu. Automático segue o idioma do sistema."},
            {"Closes the game. Unsaved progress is lost.", "Fecha o jogo. O progresso não salvo é perdido."},
            {"Shadow of the Colossus, recompiled to run natively.", "Shadow of the Colossus, recompilado para rodar nativamente."},
            {"celosia - port by HenryKun55", "celosia - port por HenryKun55"},
            {"Shadow of the Colossus (c) Sony Interactive Entertainment.", "Shadow of the Colossus (c) Sony Interactive Entertainment."},
            {"Built on PS2Recomp. Thanks to the sotc-pc project.", "Feito sobre o PS2Recomp. Obrigado ao projeto sotc-pc."},
        };
        for (const auto &t : kTexts)
            if (std::strcmp(t.en, en) == 0)
                return t.pt;
        return en;
    }

    // ---- Items ------------------------------------------------------------

    enum ItemId
    {
        kExactAlpha,
        kShowFps,
        kScreen,
        kVolume,
        kLanguage,
        kQuit,
    };

    struct Item
    {
        ItemId id;
        const char *label;
        const char *help;
    };

    struct Tab
    {
        const char *label;
        const Item *items;
        int count;
    };

    const Item kVideoItems[] = {
        {kExactAlpha, "Exact transparency",
         "Draws the effects that use the alpha test exactly like the PS2 (clouds, mist, shadows). "
         "Turning it off gains a few frames per second; some mist and edges look harder."},
        {kShowFps, "Show FPS", "Shows the game's frame rate in the corner of the screen."},
        {kScreen, "Screen",
         "Fit keeps the original 4:3 picture with bars on the sides. Stretch fills the screen. "
         "Sharp uses the largest whole multiple of the original size, so every pixel has the same size."},
    };
    const Item kSoundItems[] = {
        {kVolume, "Volume", "Volume of the game's sound and music."},
    };
    const Item kSystemItems[] = {
        {kLanguage, "Language", "Language of this menu. Automatic follows the system language."},
        {kQuit, "Return to launcher", "Closes the game. Unsaved progress is lost."},
    };
    // The Quality, Renderer and Debug tabs list native_renderer::tuningItems()
    // (the same list as the Mac's F10 panel), each with a last "Defaults" row:
    // Quality the preset, resolution, filtering...; Debug what to draw at
    // all, the layers; Renderer the rest.
    constexpr int kQualityTab = 1, kRendererTab = 2, kDebugTab = 3, kStatesTab = 6;
    const Tab kTabs[] = {
        {"Video", kVideoItems, 3},
        {"Quality", nullptr, -1},
        {"Renderer", nullptr, -1},
        {"Debug", nullptr, -1},
        {"Sound", kSoundItems, 1},
        {"System", kSystemItems, 2},
        {"States", nullptr, -1},
        {"About", nullptr, 0},
    };
    constexpr int kTabCount = 8;
    int g_tabNow = 0; // the tab the renderer helpers below list

    // The list's items of a tuning tab.
    const std::vector<int> &tuningRows(int tab)
    {
        static std::vector<int> normal, dbg, quality;
        if (normal.empty() && dbg.empty())
        {
            const auto &items = native_renderer::tuningItems();
            for (int i = 0; i < static_cast<int>(items.size()); ++i)
                (items[i].debug ? dbg : (items[i].quality ? quality : normal)).push_back(i);
        }
        return tab == kDebugTab ? dbg : (tab == kQualityTab ? quality : normal);
    }

    bool isTuningTab(int tab) { return tab == kQualityTab || tab == kRendererTab || tab == kDebugTab; }

    int itemCount(int tab)
    {
        if (tab == kStatesTab)
            return savestates::SlotCount * 3;
        return isTuningTab(tab) ? static_cast<int>(tuningRows(tab).size()) + 1 : kTabs[tab].count;
    }

    void stateAction(int item, bool confirmed = false)
    {
        if (savestates::busy())
        {
            // Keep the manager's live progress/result visible after this
            // operation completes instead of pinning a stale local warning.
            g_stateMessage.clear();
            return;
        }
        const int slot = item / 3, operation = item % 3;
        if (slot < 0 || slot >= savestates::SlotCount)
            return;
        const auto slots = savestates::slots();
        if (operation != 0 && !slots[slot].exists)
        {
            g_stateMessage = T("The selected state is empty.");
            return;
        }
        if (!confirmed && (operation != 0 || slots[slot].exists))
        {
            g_stateConfirm = true;
            g_stateConfirmYes = false;
            g_stateConfirmItem = item;
            return;
        }
        g_stateMessage.clear();
        const savestates::Action action = operation == 0 ? savestates::Action::Save :
                                          operation == 1 ? savestates::Action::Load : savestates::Action::Delete;
        savestates::request(action, slot);
    }

    // Row i of the current tuning tab -> the list's item (or -1: the reset row).
    int tuningIndex(int i)
    {
        const auto &rows = tuningRows(g_tabNow);
        return i < static_cast<int>(rows.size()) ? rows[i] : -1;
    }

    const char *rendererLabel(int i)
    {
        const int k = tuningIndex(i);
        if (k < 0)
            return portuguese() ? "Restaurar padrões" : "Reset to defaults";
        const auto &item = native_renderer::tuningItems()[k];
        return portuguese() ? item.labelPt : item.label;
    }

    const char *rendererHelp(int i)
    {
        const int k = tuningIndex(i);
        if (k < 0)
            return portuguese() ? "Volta todos os ajustes do renderer aos valores padrão." : "Puts every renderer adjustment back to its default.";
        const auto &item = native_renderer::tuningItems()[k];
        if (!native_renderer::tuningApplies(item))
        {
            // Said first: it changes nothing on this renderer.
            static std::string text;
            text = std::string(portuguese() ? "Sem efeito no renderer " : "No effect on the ") + native_renderer::backendName() +
                   (portuguese() ? " em uso. " : " renderer in use. ") + (portuguese() ? item.helpPt : item.help);
            return text.c_str();
        }
        return portuguese() ? item.helpPt : item.help;
    }

    std::string rendererValue(int i)
    {
        const int k = tuningIndex(i);
        if (k < 0)
            return "";
        const auto &item = native_renderer::tuningItems()[k];
        std::string v = native_renderer::tuningValueText(item, portuguese());
        if (v == "On" || v == "Off")
            v = T(v.c_str());
        if (!native_renderer::tuningApplies(item)) // listed, but this renderer ignores it
            v += portuguese() ? (item.backends == 1 ? " (só Metal)" : " (só OpenGL)") : (item.backends == 1 ? " (Metal only)" : " (OpenGL only)");
        return v;
    }

    std::string valueText(ItemId id)
    {
        switch (id)
        {
        case kExactAlpha: return T(g_settings.exactAlpha ? "On" : "Off");
        case kShowFps: return T(g_settings.showFps ? "On" : "Off");
        case kScreen:
        {
            static const char *modes[] = {"Fit (4:3)", "Stretch", "Sharp (whole scale)"};
            return T(modes[std::clamp(g_settings.screenMode.load(), 0, 2)]);
        }
        case kVolume:
        {
            const int v = g_settings.volume.load();
            return std::string(static_cast<size_t>(v), '|') + std::string(static_cast<size_t>(10 - v), '.') + "  " + std::to_string(v * 10) + "%";
        }
        case kLanguage:
        {
            static const char *langs[] = {"Automatic", "Português", "English"};
            return T(langs[std::clamp(g_settings.language.load(), 0, 2)]);
        }
        case kQuit: return g_quitArmed ? T("Confirm again to return") : "";
        }
        return "";
    }

    // ---- Persistence --------------------------------------------------------

    void save()
    {
        FILE *f = std::fopen(kSettingsPath, "w");
        if (!f)
            return;
        std::fprintf(f, "exact_alpha=%d\nshow_fps=%d\nscreen=%d\nfullscreen=%d\nvolume=%d\nlanguage=%d\n", g_settings.exactAlpha ? 1 : 0,
                     g_settings.showFps ? 1 : 0, g_settings.screenMode.load(), g_settings.fullscreen ? 1 : 0,
                     g_settings.volume.load(), g_settings.language.load());
        const native_renderer::Tuning &t = native_renderer::tuning();
        for (const native_renderer::TuningItem &item : native_renderer::tuningItems())
        {
            double value = 0.0;
            if (item.toggle)
                value = t.*item.toggle ? 1.0 : 0.0;
            else if (item.mask) // hidden layers: 1 = hidden
                value = ((t.*item.mask >> item.bit) & 1u) ? 1.0 : 0.0;
            else
                value = static_cast<double>(t.*item.number);
            std::fprintf(f, "%s=%g\n", item.key, value);
        }
        std::fclose(f);
    }

    // Changes an item by +1/-1 (Cross and right = +1).
    void change(ItemId id, int dir)
    {
        switch (id)
        {
        case kExactAlpha: g_settings.exactAlpha = !g_settings.exactAlpha; break;
        case kShowFps: g_settings.showFps = !g_settings.showFps; break;
        case kScreen: g_settings.screenMode = (g_settings.screenMode + 3 + dir) % 3; break;
        case kVolume: g_settings.volume = std::clamp(g_settings.volume + dir, 0, 10); break;
        case kLanguage: g_settings.language = (g_settings.language + 3 + dir) % 3; break;
        case kQuit:
            if (dir > 0)
            {
                if (g_quitArmed)
                    g_quit = true;
                g_quitArmed = true;
            }
            return;
        }
        save();
    }

    // ---- Input --------------------------------------------------------------

    enum Button
    {
        kUp, kDown, kLeft, kRight, kCross, kCircle, kTriangle, kL1, kR1, kSelect, kButtonCount
    };

    bool g_down[kButtonCount] = {};
    bool g_prev[kButtonCount] = {};

    void readButtons()
    {
        std::memcpy(g_prev, g_down, sizeof(g_down));
        std::memset(g_down, 0, sizeof(g_down));
        const auto pad = ps2pad::readGamepad();
        if (pad.index >= 0)
        {
            g_down[kUp] = pad.buttons[GAMEPAD_BUTTON_LEFT_FACE_UP] || pad.axes[GAMEPAD_AXIS_LEFT_Y] < -0.6f;
            g_down[kDown] = pad.buttons[GAMEPAD_BUTTON_LEFT_FACE_DOWN] || pad.axes[GAMEPAD_AXIS_LEFT_Y] > 0.6f;
            g_down[kLeft] = pad.buttons[GAMEPAD_BUTTON_LEFT_FACE_LEFT] || pad.axes[GAMEPAD_AXIS_LEFT_X] < -0.6f;
            g_down[kRight] = pad.buttons[GAMEPAD_BUTTON_LEFT_FACE_RIGHT] || pad.axes[GAMEPAD_AXIS_LEFT_X] > 0.6f;
            g_down[kCross] = pad.buttons[GAMEPAD_BUTTON_RIGHT_FACE_DOWN];
            g_down[kCircle] = pad.buttons[GAMEPAD_BUTTON_RIGHT_FACE_RIGHT];
            g_down[kTriangle] = pad.buttons[GAMEPAD_BUTTON_RIGHT_FACE_UP];
            g_down[kL1] = pad.buttons[GAMEPAD_BUTTON_LEFT_TRIGGER_1];
            g_down[kR1] = pad.buttons[GAMEPAD_BUTTON_RIGHT_TRIGGER_1];
            g_down[kSelect] = pad.buttons[GAMEPAD_BUTTON_MIDDLE_LEFT];
        }
        // Keyboard: arrows, Enter = Cross, Backspace = Circle, H = help, Q/E = tabs.
        g_down[kUp] |= IsKeyDown(KEY_UP);
        g_down[kDown] |= IsKeyDown(KEY_DOWN);
        g_down[kLeft] |= IsKeyDown(KEY_LEFT);
        g_down[kRight] |= IsKeyDown(KEY_RIGHT);
        g_down[kCross] |= IsKeyDown(KEY_ENTER);
        g_down[kCircle] |= IsKeyDown(KEY_BACKSPACE);
        g_down[kTriangle] |= IsKeyDown(KEY_H);
        g_down[kL1] |= IsKeyDown(KEY_Q);
        g_down[kR1] |= IsKeyDown(KEY_E);
    }

    bool pressed(Button b) { return g_down[b] && !g_prev[b]; }

    bool anyDown()
    {
        for (bool d : g_down)
            if (d)
                return true;
        return false;
    }

    void close()
    {
        g_open = false;
        g_help = false;
        g_quitArmed = false;
        g_stateConfirm = false;
        g_holdBlock = true; // until the closing button is released
    }

    // ---- Drawing --------------------------------------------------------------

    // Draws `text` wrapped at maxWidth; returns the number of lines (none
    // drawn when `draw` is false).
    int drawWrapped(const char *text, int x, int y, int maxWidth, int size, Color color, int lineGap, bool draw = true)
    {
        std::string line, word;
        int lines = 0;
        auto flush = [&]
        {
            ++lines;
            if (draw)
                DrawText(line.c_str(), x, y, size, color);
            y += size + lineGap;
            line.clear();
        };
        for (const char *p = text;; ++p)
        {
            if (*p == ' ' || *p == '\0')
            {
                const std::string candidate = line.empty() ? word : line + " " + word;
                if (!line.empty() && MeasureText(candidate.c_str(), size) > maxWidth)
                {
                    flush();
                    line = word;
                }
                else
                    line = candidate;
                word.clear();
                if (*p == '\0')
                    break;
            }
            else
                word += *p;
        }
        if (!line.empty())
            flush();
        return lines;
    }

    void drawStates(int boxX, int boxY, int boxW, int boxH, int listY, int size, int small)
    {
        const int pad = size;
        const Color text{225, 228, 230, 255}, dim{150, 156, 162, 255}, accent{232, 196, 120, 255};
        if (g_help)
        {
            DrawText(T("States"), boxX + pad, listY, size, accent);
            const char *help = portuguese()
                ? "Estados retomam a sessão exatamente no ponto salvo e são compatíveis com a mesma versão do núcleo e do jogo. F5 salva no espaço rápido; F8 carrega esse espaço diretamente. Use os cinco espaços manuais para manter outros pontos. Carregar pelo menu, substituir e excluir pedem confirmação. O cartão de memória continua separado: se ele mudar, estados anteriores podem deixar de ser compatíveis."
                : "States resume the session at its saved position and require the same core and game version. F5 saves the quick slot; F8 loads it directly. Keep other positions in the five manual slots. Menu loads, overwrites and deletions require confirmation. The memory card stays separate: changing it can make an older state incompatible.";
            drawWrapped(help, boxX + pad, listY + size * 2, boxW - 2 * pad, small, text, small / 3);
            DrawText(T("Triangle / O: back"), boxX + pad, boxY + boxH - size * 3 / 2, small, dim);
            return;
        }
        const auto slots = savestates::slots();
        const bool working = savestates::busy();
        const int statusY = boxY + boxH - size * 9 / 2;
        const int rowH = std::max(1, (statusY - size / 2 - listY) / savestates::SlotCount);
        const int labelSize = std::min(small, std::max(10, rowH * 9 / 20));
        const int metaSize = std::max(10, std::min(size / 2, rowH / 3));
        const int infoW = (boxW - 2 * pad) * 43 / 100;
        const int gap = std::max(3, size / 3);
        const int buttonW = (boxW - 2 * pad - infoW - gap * 3) / 3;
        for (int slot = 0; slot < savestates::SlotCount; ++slot)
        {
            const int y = listY + slot * rowH;
            if (g_item / 3 == slot)
                DrawRectangle(boxX + pad / 2, y, boxW - pad, rowH - 1, Color{255, 255, 255, 16});
            const std::string name = slot == 0 ? T("Quick state") : std::string("Slot ") + std::to_string(slot);
            DrawText(name.c_str(), boxX + pad, y + 2, labelSize, g_item / 3 == slot ? text : dim);
            std::string metadata = T("Empty");
            if (slots[slot].exists)
            {
                char bytes[32];
                std::snprintf(bytes, sizeof(bytes), "  %.1f MiB", static_cast<double>(slots[slot].bytes) / (1024.0 * 1024.0));
                metadata = slots[slot].modified + bytes;
            }
            int metadataSize = metaSize;
            while (metadataSize > 8 && MeasureText(metadata.c_str(), metadataSize) > infoW - gap)
                --metadataSize;
            DrawText(metadata.c_str(), boxX + pad, y + labelSize + 3, metadataSize, dim);
            for (int operation = 0; operation < 3; ++operation)
            {
                const int item = slot * 3 + operation;
                const int x = boxX + pad + infoW + gap + operation * (buttonW + gap);
                const bool enabled = !working && (operation == 0 || slots[slot].exists);
                const Rectangle rectangle{float(x), float(y + 2), float(buttonW), float(std::max(1, rowH - 5))};
                g_rowRects.push_back({rectangle, item});
                DrawRectangle(x, y + 2, buttonW, std::max(1, rowH - 5),
                              g_item == item ? Color{232, 196, 120, 45} : Color{255, 255, 255, 9});
                const char *label = T(operation == 0 ? "Save" : operation == 1 ? "Load" : "Delete");
                int font = small;
                while (font > 8 && MeasureText(label, font) > buttonW - gap)
                    --font;
                DrawText(label, x + (buttonW - MeasureText(label, font)) / 2, y + (rowH - font) / 2,
                         font, enabled ? (g_item == item ? accent : text) : Color{91, 96, 104, 255});
            }
        }
        DrawRectangle(boxX + pad, statusY - size / 3, boxW - 2 * pad, 1, Color{70, 80, 90, 255});
        const std::string status = g_stateMessage.empty() ? savestates::status() : g_stateMessage;
        const char *idle = portuguese() ? "F5: salvar rápido   F8: carregar rápido" : "F5: quick save   F8: quick load";
        int statusSize = small;
        const char *message = status.empty() ? idle : status.c_str();
        while (statusSize > 10 && drawWrapped(message, 0, 0, boxW - 2 * pad, statusSize, dim, statusSize / 4, false) *
               (statusSize + statusSize / 4) > size * 3)
            --statusSize;
        drawWrapped(message, boxX + pad, statusY, boxW - 2 * pad, statusSize, working ? accent : dim, statusSize / 4);
        const char *hint = portuguese() ? "Q/E: abas  Setas: espaço/ação  Enter/clique: escolher  H: ajuda  F1: fechar"
                                       : "Q/E: tabs  Arrows: slot/action  Enter/click: select  H: help  F1: close";
        int hintSize = small;
        while (hintSize > 8 && MeasureText(hint, hintSize) > boxW - 2 * pad) --hintSize;
        DrawText(hint, boxX + pad, boxY + boxH - size, hintSize, dim);

        if (g_stateConfirm)
        {
            DrawRectangle(boxX, boxY, boxW, boxH, Color{0, 0, 0, 205});
            const int width = boxW - 4 * pad, height = std::min(boxH - 2 * pad, size * 9);
            const int x = boxX + 2 * pad, y = boxY + (boxH - height) / 2;
            DrawRectangle(x, y, width, height, Color{20, 24, 30, 255});
            DrawRectangle(x, y, width, 2, accent);
            const int operation = g_stateConfirmItem % 3;
            const char *title = T(operation == 0 ? "Overwrite this state?" : operation == 1 ? "Load this state?" : "Delete this state?");
            drawWrapped(title, x + pad, y + pad, width - 2 * pad, small, accent, small / 4);
            const char *explanation = T(operation == 0 ? "This replaces the selected state with the current game session." :
                                        operation == 1 ? "Loading replaces the current game position and progress. Your memory-card file is preserved." :
                                                         "This removes only the selected state file.");
            drawWrapped(explanation, x + pad, y + size * 5 / 2, width - 2 * pad, small, text, small / 4);
            for (int choice = 0; choice < 2; ++choice)
            {
                const int buttonX = x + pad + choice * (width / 2 - pad / 2);
                const int buttonWidth = width / 2 - pad * 3 / 2;
                const int buttonY = y + height - size * 2;
                g_stateConfirmRects[choice] = Rectangle{float(buttonX), float(buttonY), float(buttonWidth), float(size * 3 / 2)};
                const bool selected = g_stateConfirmYes == (choice == 1);
                DrawRectangle(buttonX, buttonY, buttonWidth, size * 3 / 2,
                              selected ? Color{232, 196, 120, 45} : Color{255, 255, 255, 12});
                const char *label = T(choice == 0 ? "Cancel" : "Confirm");
                DrawText(label, buttonX + (buttonWidth - MeasureText(label, small)) / 2, buttonY + size / 4, small, selected ? accent : text);
            }
        }
    }
}

Settings &settings() { return g_settings; }
void saveSettings() { save(); }

void load()
{
    if (const char *open = std::getenv("PS2X_SETTINGS_OPEN")) // development: open at start on tab N
    {
        g_open = true;
        g_tab = std::clamp(std::atoi(open), 0, kTabCount - 1);
    }
    FILE *f = std::fopen(kSettingsPath, "r");
    if (!f)
        return;
    char key[64];
    float number = 0.0f;
    while (std::fscanf(f, " %63[^=]=%f", key, &number) == 2)
    {
        const int value = static_cast<int>(number);
        if (native_renderer::tuningRestore(key, number))
            continue;
        if (!std::strcmp(key, "exact_alpha")) g_settings.exactAlpha = value != 0;
        else if (!std::strcmp(key, "show_fps")) g_settings.showFps = value != 0;
        else if (!std::strcmp(key, "screen")) g_settings.screenMode = std::clamp(value, 0, 2);
        else if (!std::strcmp(key, "fullscreen")) g_settings.fullscreen = value != 0;
        else if (!std::strcmp(key, "volume")) g_settings.volume = std::clamp(value, 0, 10);
        else if (!std::strcmp(key, "language")) g_settings.language = std::clamp(value, 0, 2);
    }
    std::fclose(f);
}

bool isOpen() { return g_open; }
bool quitRequested() { return g_quit; }
bool inputBlocked() { return g_open || g_holdBlock; }

void update()
{
    ps2pad::pollGamepads();
    readButtons();
    if (g_holdBlock && !anyDown())
        g_holdBlock = false;

    if (!g_open)
    {
        bool open = IsKeyPressed(KEY_F1);
        if (pressed(kSelect))
        {
            const double now = GetTime();
            open |= g_lastSelect >= 0.0 && now - g_lastSelect < kDoubleTapSeconds;
            g_lastSelect = open ? -1.0 : now;
        }
        if (open)
        {
            g_open = true;
            g_help = false;
            g_item = std::min(g_item, std::max(itemCount(g_tab) - 1, 0));
        }
        return;
    }

    if (IsKeyPressed(KEY_F1) || pressed(kSelect))
    {
        close();
        return;
    }
    if (g_stateConfirm)
    {
        if (pressed(kCircle) || pressed(kTriangle))
        {
            g_stateConfirm = false;
            return;
        }
        bool activate = pressed(kCross);
        if (pressed(kLeft) || pressed(kRight) || pressed(kUp) || pressed(kDown))
            g_stateConfirmYes = !g_stateConfirmYes;
#if defined(__linux__) && !defined(__ANDROID__)
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            const Vector2 pointer = GetMousePosition();
            for (int choice = 0; choice < 2; ++choice)
                if (CheckCollisionPointRec(pointer, g_stateConfirmRects[choice]))
                {
                    g_stateConfirmYes = choice == 1;
                    activate = true;
                }
        }
#endif
        if (activate)
        {
            g_stateConfirm = false;
            if (g_stateConfirmYes)
                stateAction(g_stateConfirmItem, true);
        }
        return;
    }
    if (g_help)
    {
        if (pressed(kTriangle) || pressed(kCircle))
            g_help = false;
        return;
    }
    if (pressed(kCircle))
    {
        close();
        return;
    }
    bool mouseActivate = false;
#if defined(__linux__) && !defined(__ANDROID__)
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const Vector2 pointer = GetMousePosition();
        for (int i = 0; i < kTabCount; ++i) {
            if (CheckCollisionPointRec(pointer, g_tabRects[i])) {
                g_tab = i; g_item = 0; g_quitArmed = false;
                return;
            }
        }
        for (const auto &row : g_rowRects) {
            if (CheckCollisionPointRec(pointer, row.first)) {
                if (g_item != row.second) g_quitArmed = false;
                g_item = row.second;
                mouseActivate = true;
                break;
            }
        }
    }
#endif
    const Tab &tab = kTabs[g_tab];
    if (pressed(kL1) || pressed(kR1))
    {
        g_tab = (g_tab + (pressed(kR1) ? 1 : kTabCount - 1)) % kTabCount;
        g_item = 0;
        g_quitArmed = false;
        return;
    }
    const int count = itemCount(g_tab);
    if (count == 0)
        return;
    if (g_tab == kStatesTab)
    {
        if (pressed(kUp) || pressed(kDown))
        {
            const int slot = (g_item / 3 + (pressed(kDown) ? 1 : savestates::SlotCount - 1)) % savestates::SlotCount;
            g_item = slot * 3 + g_item % 3;
        }
        if (pressed(kLeft) || pressed(kRight))
            g_item = (g_item / 3) * 3 + (g_item % 3 + (pressed(kRight) ? 1 : 2)) % 3;
        if (mouseActivate || pressed(kCross))
            stateAction(g_item);
        if (pressed(kTriangle))
            g_help = true;
        return;
    }
    if (pressed(kUp) || pressed(kDown))
    {
        g_item = (g_item + (pressed(kDown) ? 1 : count - 1)) % count;
        g_quitArmed = false;
    }
    if (isTuningTab(g_tab))
    {
        g_tabNow = g_tab;
        const int dir = (mouseActivate || pressed(kCross) || pressed(kRight)) ? 1 : (pressed(kLeft) ? -1 : 0);
        if (dir != 0)
        {
            const int k = tuningIndex(g_item);
            if (k < 0)
                native_renderer::tuning() = native_renderer::defaultTuning();
            else
                native_renderer::tuningStep(native_renderer::tuningItems()[k], dir);
            save();
        }
        if (pressed(kTriangle))
            g_help = true;
        return;
    }
    const ItemId id = tab.items[g_item].id;
    if (mouseActivate || pressed(kCross) || pressed(kRight))
        change(id, +1);
    else if (pressed(kLeft) && id != kQuit)
        change(id, -1);
    if (pressed(kTriangle))
        g_help = true;
}

void draw(int screenWidth, int screenHeight)
{
    if (!g_open)
        return;
    const int size = std::max(10, screenHeight / 27 / 2 * 2); // ~20 px on the Vita
    const int small = std::max(10, size * 4 / 5);
    const int pad = size;
    const int boxW = std::min(screenWidth - 2 * pad, size * 34);
    const int boxH = std::min(screenHeight - 2 * pad, size * 20);
    const int boxX = (screenWidth - boxW) / 2, boxY = (screenHeight - boxH) / 2;
    const Color panel{12, 14, 18, 230}, line{70, 80, 90, 255}, text{225, 228, 230, 255}, dim{150, 156, 162, 255},
        accent{232, 196, 120, 255}, highlight{255, 255, 255, 28};

    DrawRectangle(0, 0, screenWidth, screenHeight, Color{0, 0, 0, 110});
    DrawRectangle(boxX, boxY, boxW, boxH, panel);
    DrawRectangle(boxX, boxY, boxW, 2, accent);
    DrawText(T("SETTINGS"), boxX + pad, boxY + pad * 2 / 3, size, accent);

    g_rowRects.clear();
    // Tabs (smaller when they do not fit the box)
    int tabX = boxX + pad;
    int tabY = boxY + pad * 2;
    int tabSize = size, tabGap = size * 3 / 2;
    {
        int total = 0;
        for (int i = 0; i < kTabCount; ++i)
            total += MeasureText(T(kTabs[i].label), size) + tabGap;
        if (total > boxW - 2 * pad)
        {
            tabGap = size;
            total = 0;
            for (int i = 0; i < kTabCount; ++i)
                total += MeasureText(T(kTabs[i].label), size) + tabGap;
            if (total > boxW - 2 * pad)
                tabSize = small;
        }
    }
    for (int i = 0; i < kTabCount; ++i)
    {
        const char *label = T(kTabs[i].label);
        const int w = MeasureText(label, tabSize);
        // The additional States tab must not push controls outside the menu
        // at smaller window sizes or with longer Portuguese labels.
        if (tabX > boxX + pad && tabX + w + size / 3 > boxX + boxW - pad)
        {
            tabX = boxX + pad;
            tabY += size * 7 / 4;
        }
        g_tabRects[i] = Rectangle{float(tabX - size/3), float(tabY - size/4), float(w + size*2/3), float(size*3/2)};
        if (i == g_tab)
            DrawRectangle(tabX - size / 3, tabY + size + size / 4, w + size * 2 / 3, 2, accent);
        DrawText(label, tabX, tabY, tabSize, i == g_tab ? text : dim);
        tabX += w + tabGap;
    }
    DrawRectangle(boxX + pad, tabY + size * 3 / 2 + 2, boxW - 2 * pad, 1, line);

    const Tab &tab = kTabs[g_tab];
    const int listY = tabY + size * 2 + size / 2;
    const int helpY = boxY + boxH - size * 5;
    const bool renderer = isTuningTab(g_tab);
    g_tabNow = g_tab;
    if (g_tab == kStatesTab)
    {
        drawStates(boxX, boxY, boxW, boxH, listY, size, small);
        return;
    }
    if (g_help && itemCount(g_tab) > 0)
    {
        DrawText(renderer ? rendererLabel(g_item) : T(tab.items[g_item].label), boxX + pad, listY, size, accent);
        drawWrapped(renderer ? rendererHelp(g_item) : T(tab.items[g_item].help), boxX + pad, listY + size * 2, boxW - 2 * pad, small, text, small / 2);
        DrawText(T("Triangle / O: back"), boxX + pad, boxY + boxH - size * 3 / 2, small, dim);
        return;
    }
    if (itemCount(g_tab) == 0)
    {
        int y = listY;
        for (const char *l : {"Shadow of the Colossus, recompiled to run natively.", "celosia - port by HenryKun55", "",
                              "Shadow of the Colossus (c) Sony Interactive Entertainment.",
                              "Built on PS2Recomp. Thanks to the sotc-pc project."})
        {
            DrawText(T(l), boxX + pad, y, small, *l ? text : dim);
            y += small * 3 / 2;
        }
    }
    else
    {
        // Scrolls when the list is longer than the box (the Renderer tab).
        const int count = itemCount(g_tab);
        const int rows = std::max(1, (helpY - size - listY) / (size * 3 / 2));
        const int first = std::clamp(g_item - rows + 1, 0, std::max(0, count - rows));
        for (int i = first; i < count && i < first + rows; ++i)
        {
            const int y = listY + (i - first) * size * 3 / 2;
            g_rowRects.push_back({Rectangle{float(boxX + pad/2), float(y - size/4), float(boxW - pad), float(size*3/2)}, i});
            if (i == g_item)
                DrawRectangle(boxX + pad / 2, y - size / 4, boxW - pad, size * 3 / 2, highlight);
            DrawText(renderer ? rendererLabel(i) : T(tab.items[i].label), boxX + pad, y, size, i == g_item ? text : dim);
            const std::string value = renderer ? rendererValue(i) : valueText(tab.items[i].id);
            DrawText(value.c_str(), boxX + boxW * 11 / 20, y, size, i == g_item ? accent : dim);
        }
        if (first > 0)
            DrawText("^", boxX + boxW - pad, listY, small, dim);
        if (first + rows < count)
            DrawText("v", boxX + boxW - pad, listY + (rows - 1) * size * 3 / 2, small, dim);
        DrawRectangle(boxX + pad, helpY - size / 2, boxW - 2 * pad, 1, line);
        // The help in the space above the controls line, smaller when long.
        const char *help = renderer ? rendererHelp(g_item) : T(tab.items[g_item].help);
        const int helpRoom = boxY + boxH - size * 3 / 2 - helpY;
        int helpSize = small;
        while (helpSize > 10 && drawWrapped(help, 0, 0, boxW - 2 * pad, helpSize, dim, helpSize / 3, false) * (helpSize + helpSize / 3) > helpRoom)
            helpSize -= 2;
        drawWrapped(help, boxX + pad, helpY, boxW - 2 * pad, helpSize, dim, helpSize / 3);
    }
#if defined(__linux__) && !defined(__ANDROID__)
    const char *hint = portuguese() ? "Q/E: abas  Setas: item  Enter/clique: mudar  H: ajuda  F1: fechar"
                                   : "Q/E: tabs  Arrows: item  Enter/click: change  H: help  F1: close";
    DrawText(hint, boxX + pad, boxY + boxH - size * 3 / 2, small, dim);
#else
    DrawText(T("L/R: tab   D-pad: choose   X: change   Triangle: help   O: close"), boxX + pad, boxY + boxH - size * 3 / 2, small, dim);
#endif
}
}
