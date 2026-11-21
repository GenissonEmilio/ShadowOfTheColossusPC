// Controller setup for any joystick exposed by the Linux GLFW backend.
// Stores data-only GLFW mappings; no ROM, Steam or runtime compiler is needed.
#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"
#include "raylib.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>

struct Step { const char *field, *label; bool axis = false; };
static const Step steps[] = {
    {"a", "Pressione X / A (confirmar)"}, {"b", "Pressione Circulo / B (voltar)"},
    {"x", "Pressione Quadrado / X"}, {"y", "Pressione Triangulo / Y"},
    {"back", "Pressione Select / Share / View"}, {"start", "Pressione Start / Options / Menu"},
    {"leftshoulder", "Pressione L1 / LB"}, {"rightshoulder", "Pressione R1 / RB"},
    {"lefttrigger", "Pressione L2 / LT ate o fim"}, {"righttrigger", "Pressione R2 / RT ate o fim"},
    {"leftstick", "Pressione o analogico esquerdo (L3)"}, {"rightstick", "Pressione o analogico direito (R3)"},
    {"dpup", "Direcional: cima"}, {"dpdown", "Direcional: baixo"},
    {"dpleft", "Direcional: esquerda"}, {"dpright", "Direcional: direita"},
    {"leftx", "Mova o analogico esquerdo para a DIREITA", true},
    {"lefty", "Mova o analogico esquerdo para BAIXO", true},
    {"rightx", "Mova o analogico direito para a DIREITA", true},
    {"righty", "Mova o analogico direito para BAIXO", true},
};
struct Raw {
    std::vector<float> axes;
    std::vector<unsigned char> buttons, hats;
};
static Raw readRaw(int device) {
    Raw r; int n = 0;
    if (const auto *p = glfwGetJoystickAxes(device, &n); p && n) r.axes.assign(p, p + n);
    if (const auto *p = glfwGetJoystickButtons(device, &n); p && n) r.buttons.assign(p, p + n);
    if (const auto *p = glfwGetJoystickHats(device, &n); p && n) r.hats.assign(p, p + n);
    return r;
}
static bool neutral(const Raw &r, const Raw &base) {
    for (auto b : r.buttons) if (b) return false;
    for (auto h : r.hats) if (h) return false;
    for (size_t i = 0; i < std::min(r.axes.size(), base.axes.size()); ++i)
        if (std::fabs(r.axes[i] - base.axes[i]) > .3f) return false;
    return true;
}
static std::string binding(const Raw &r, const Raw &base, const Step &step) {
    if (!step.axis) {
        // GLFW appends emulated hat buttons to its raw button array, but
        // mappings validate button indices against physical buttons only.
        for (size_t i = 0; i < r.hats.size(); ++i)
            if (r.hats[i]) return "h" + std::to_string(i) + "." + std::to_string(r.hats[i]);
        for (size_t i = 0; i < r.buttons.size(); ++i)
            if (r.buttons[i]) return "b" + std::to_string(i);
    }
    for (size_t i = 0; i < std::min(r.axes.size(), base.axes.size()); ++i) {
        const float delta = r.axes[i] - base.axes[i];
        if (std::fabs(delta) < .7f) continue;
        const bool trigger = std::string(step.field).find("trigger") != std::string::npos;
        if (step.axis || trigger) {
            if (trigger && std::fabs(base.axes[i]) < .3f)
                return std::string(delta > 0 ? "+a" : "-a") + std::to_string(i);
            return "a" + std::to_string(i) + (delta < 0 ? "~" : "");
        }
        return std::string(delta > 0 ? "+a" : "-a") + std::to_string(i);
    }
    return {};
}
static void save(const std::filesystem::path &file, const std::string &guid, const std::string &line) {
    std::string content, old;
    if (std::filesystem::exists(file) && std::filesystem::file_size(file) > 1024 * 1024)
        throw std::runtime_error("O arquivo de perfis excedeu 1 MiB.");
    std::ifstream in(file);
    while (std::getline(in, old))
        if (!old.empty() && old.substr(0, 32) != guid) content += old + "\n";
    content += line + "\n";
    std::filesystem::create_directories(file.parent_path());
    auto tmp = file; tmp += ".tmp-" + std::to_string(getpid());
    try {
        std::ofstream out(tmp, std::ios::binary);
        out << content;
        out.close();
        if (!out) throw std::runtime_error("Nao foi possivel gravar o perfil.");
        std::filesystem::rename(tmp, file);
    } catch (...) { std::filesystem::remove(tmp); throw; }
}
int main(int argc, char **argv) {
    std::filesystem::path output, status, screenshot;
    int device = -1;
    bool begin = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--output" && i + 1 < argc) output = argv[++i];
        else if (a == "--status" && i + 1 < argc) status = argv[++i];
        else if (a == "--device" && i + 1 < argc) { device = std::atoi(argv[++i]); begin = true; }
        else if (a == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else { std::fprintf(stderr, "--output profiles.txt [--status progress.json] [--device 0..3]\n"); return 2; }
    }
    if (output.empty() || device < -1 || device > 3) return 2;
    output = std::filesystem::absolute(output);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(840, 570, "Shadow of the Colossus - Configurar controle");
    SetWindowMinSize(720, 540); SetTargetFPS(60);
    int step = -1, published = -99; bool release = false;
    double releaseAt = 0;
    Raw base;
    std::string guid, name, error;
    std::map<std::string, std::string> mapping;
    bool saved = false;
    while (!WindowShouldClose()) {
        std::vector<int> devices;
        for (int i = 0; i < 4; ++i) if (glfwJoystickPresent(i)) devices.push_back(i);
        const bool present = device >= 0 && glfwJoystickPresent(device);
        if (!present && step >= 0) { error = "Controle desconectado. Reconecte e comece novamente."; step = -1; }
        if (!present && !devices.empty()) device = devices.front();
        if (step < 0 && !devices.empty()) {
            auto pos = std::find(devices.begin(), devices.end(), device);
            int i = pos == devices.end() ? 0 : static_cast<int>(pos - devices.begin());
            if (IsKeyPressed(KEY_RIGHT)) device = devices[(i + 1) % devices.size()];
            if (IsKeyPressed(KEY_LEFT)) device = devices[(i + devices.size() - 1) % devices.size()];
            name = glfwGetJoystickName(device) ? glfwGetJoystickName(device) : "Controle USB";
            if (IsKeyPressed(KEY_ENTER) || (begin && GetTime() > .5)) {
                begin = false;
                base = readRaw(device);
                guid = glfwGetJoystickGUID(device);
                mapping.clear(); step = 0; release = true; error.clear();
            }
        } else if (step >= 0 && step < static_cast<int>(std::size(steps)) && present) {
            const Raw raw = readRaw(device);
            if (release) {
                if (!neutral(raw, base)) releaseAt = GetTime();
                else if (GetTime() - releaseAt > .25) release = false;
            } else if (IsKeyPressed(KEY_ENTER)) { mapping.erase(steps[step].field); ++step; release = true; releaseAt = GetTime(); }
            else if (IsKeyPressed(KEY_BACKSPACE)) { step = std::max(0, step - 1); release = true; releaseAt = GetTime(); }
            else if (auto b = binding(raw, base, steps[step]); !b.empty()) {
                mapping[steps[step].field] = b;
                ++step; release = true; releaseAt = GetTime();
            }
        }
        if (step == static_cast<int>(std::size(steps))) {
            if (!mapping.count("a")) { error = "Configure pelo menos o botao X / A para confirmar."; step = -1; }
            else {
                std::string clean = name;
                for (char &c : clean) if (c == ',' || c == '\n' || c == '\r') c = ' ';
                std::string line = guid + "," + clean + ",";
                for (auto &[key, value] : mapping) line += key + ":" + value + ",";
                line += "platform:Linux,";
                std::fprintf(stderr, "[controller-profile] %s\n", line.c_str());
                if (!glfwUpdateGamepadMappings(line.c_str()) || !glfwJoystickIsGamepad(device)) {
                    error = "O driver recusou o perfil. Tente novamente."; step = -1;
                } else try { save(output, guid, line); saved = true; break; }
                catch (const std::exception &e) { error = e.what(); step = -1; }
            }
        }
        if (!status.empty() && step != published) {
            std::ofstream s(status);
            s << "{\"step\":" << step << ",\"device\":" << device << "}\n";
            published = step;
        }
        const Color bg{16, 24, 29, 255}, gold{204, 185, 132, 255}, pale{225, 232, 228, 255};
        BeginDrawing(); ClearBackground(bg);
        DrawText("CONTROLES", 42, 34, 18, gold);
        DrawText("Seu controle. Sua jornada.", 42, 70, 29, pale);
        DrawText("USB ou Bluetooth - perfis salvos neste computador", 42, 114, 18, GRAY);
        DrawRectangleRounded({36, 163, static_cast<float>(GetScreenWidth() - 72), 267}, .07f, 8, {27, 40, 46, 255});
        if (devices.empty()) DrawText("Conecte um controle para comecar.", 58, 205, 23, pale);
        else {
            DrawText(TextFormat("Controle %d  %s", device + 1, name.substr(0, 54).c_str()), 58, 188, 20, gold);
            if (step < 0) {
                DrawText("Deixe os analogicos no centro e solte os botoes.", 58, 246, 21, pale);
                DrawText("Enter: comecar    Setas: escolher controle", 58, 292, 19, pale);
            } else {
                DrawText(TextFormat("ETAPA %d DE %d", step + 1, static_cast<int>(std::size(steps))), 58, 240, 17, gold);
                DrawText(release ? "Solte o controle..." : steps[step].label, 58, 278, 23, pale);
                DrawText("Enter: pular    Backspace: refazer anterior", 58, 329, 18, GRAY);
                DrawRectangle(58, 389, (GetScreenWidth() - 116) * step / std::size(steps), 5, gold);
            }
        }
        DrawText("O perfil funciona no jogo e no menu F1. Esc: cancelar.", 42, 458, 17, pale);
        if (!error.empty()) DrawText(error.c_str(), 42, 497, 17, ORANGE);
        EndDrawing();
        if (!screenshot.empty() && step >= 0) {
            Image frame = LoadImageFromScreen();
            ExportImage(frame, screenshot.c_str());
            UnloadImage(frame);
            screenshot.clear();
        }
    }
    CloseWindow();
    return saved ? 0 : 1;
}
