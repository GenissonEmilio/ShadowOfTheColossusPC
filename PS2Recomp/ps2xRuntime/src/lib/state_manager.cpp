#include "runtime/state_manager.h"
#include "runtime/ee_scheduler.h"
#include "ps2_runtime.h"
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <mutex>
namespace savestates
{
namespace {
    std::mutex mutex;
    PS2Runtime *active = nullptr;
    bool queued = false, working = false;
    Action pending = Action::Save;
    int selected = 0;
    std::string message;
}
std::filesystem::path directory()
{
    if (const char *path = std::getenv("PS2X_STATE_DIR"); path && *path) return path;
    return std::filesystem::current_path() / "savestates";
}
void bind(PS2Runtime *runtime)
{
    std::lock_guard<std::mutex> lock(mutex);
    active = runtime; queued = working = false; message.clear();
}
void request(Action action, int slot)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!active || slot < 0 || slot >= SlotCount || queued || working) return;
    pending = action; selected = slot; queued = true;
    message = action == Action::Save ? "Salvando estado…" : action == Action::Load ? "Carregando estado…" : "Removendo estado…";
    active->eeScheduler().notifyStateRequest();
}
bool busy() { std::lock_guard<std::mutex> lock(mutex); return queued || working; }
std::string status() { std::lock_guard<std::mutex> lock(mutex); return message; }
bool take(Action &action, int &slot)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!queued) return false;
    action = pending; slot = selected; queued = false; working = true; return true;
}
void complete(const std::string &value)
{ std::lock_guard<std::mutex> lock(mutex); message = value; working = false; std::fprintf(stderr,"[state] %s\n",value.c_str()); }
std::array<Slot, SlotCount> slots()
{
    std::array<Slot, SlotCount> out{};
    for (int i = 0; i < SlotCount; ++i)
    {
        const auto path = directory() / (i == 0 ? "quick.sotcstate" : "slot-" + std::to_string(i) + ".sotcstate");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) continue;
        out[i].bytes = std::filesystem::file_size(path, ec);
        if (ec) continue;
        const auto ft = std::filesystem::last_write_time(path, ec);
        if (ec) continue;
        out[i].exists = true;
        const auto st = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - decltype(ft)::clock::now() + std::chrono::system_clock::now());
        const std::time_t time = std::chrono::system_clock::to_time_t(st);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &time);
#else
        localtime_r(&time, &local);
#endif
        char text[32]{}; std::strftime(text, sizeof(text), "%d/%m %H:%M:%S", &local);
        out[i].modified = text;
    }
    return out;
}
uint32_t checksum(const std::vector<uint8_t> &bytes)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int b = 0; b < 8; ++b) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0); t[i] = c; }
        return t;
    }();
    uint32_t c = ~0u; for (uint8_t b : bytes) c = table[(c ^ b) & 255u] ^ (c >> 8);
    return ~c;
}
}
