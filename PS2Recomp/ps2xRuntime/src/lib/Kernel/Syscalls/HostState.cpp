#include "Common.h"
#include "HostState.h"
#include "runtime/save_state_archive.h"

#include <array>
#include <limits>
#include <stdexcept>

namespace ps2_syscalls
{
    namespace
    {
        constexpr uint32_t kMagic = 0x3153484Bu; // KHS1
        constexpr uint32_t kVersion = 1u;
        constexpr uint32_t kObjects = 4096u;
        constexpr size_t kLimit = 2u * 1024u * 1024u;

        struct HostState
        {
            std::unordered_map<uint32_t, RpcServerState> servers;
            std::unordered_map<uint32_t, RpcClientState> clients;
            bool rpcInitialized = false, bootInitialized = false, osdInitialized = false, pathsInitialized = false;
            uint32_t rpcNext = 1, packetIndex = 0, serverIndex = 0, activeQueue = 0;
            uint32_t bootOffset = 0, tlsIndex = 0, osd = 0, osd2 = 0;
            std::unordered_map<uint8_t, uint32_t> bootAddresses;
            std::array<std::string, 4> paths;
            std::string cwdDevice;
            std::unordered_map<int32_t, SifModuleRecord> modules;
            std::unordered_map<std::string, int32_t> modulePaths;
            int32_t nextModule = 1;
        };

        template<class Map, class Visit>
        bool writeMap(ps2state::Writer &writer, const Map &values, Visit visit)
        {
            if (values.size() > kObjects) return writer.fail("too many kernel host objects");
            const uint32_t count = static_cast<uint32_t>(values.size());
            if (!writer.pod(count)) return false;
            std::vector<typename Map::key_type> keys;
            keys.reserve(values.size());
            for (const auto &[key, value] : values) { (void)value; keys.push_back(key); }
            std::sort(keys.begin(), keys.end());
            for (const auto &key : keys)
                if (!visit(key, values.at(key))) return false;
            return true;
        }

        bool guestAddress(uint32_t address)
        {
            return (address & 3u) == 0u && (address & 0x1FFFFFFFu) <= PS2_RAM_SIZE - 4u;
        }

        bool readState(const std::vector<uint8_t> &bytes, HostState &state, std::string &error)
        {
            ps2state::Reader reader(bytes, error, kLimit);
            uint32_t magic = 0, version = 0;
            if (!reader.pod(magic) || !reader.pod(version) || magic != kMagic || version != kVersion)
                return reader.fail("incompatible kernel host state");
            for (bool *value : {&state.rpcInitialized, &state.bootInitialized, &state.osdInitialized, &state.pathsInitialized})
                if (!reader.pod(*value)) return false;
            for (uint32_t *value : {&state.rpcNext, &state.packetIndex, &state.serverIndex, &state.activeQueue,
                                    &state.bootOffset, &state.tlsIndex, &state.osd, &state.osd2})
                if (!reader.pod(*value)) return false;
            if (!reader.pod(state.nextModule) || state.nextModule <= 0 || state.bootOffset > kBootModePoolBytes ||
                (state.bootOffset & 3u) != 0u || !guestAddress(state.activeQueue))
                return reader.fail("invalid kernel allocator state");
            for (auto &path : state.paths)
                if (!reader.string(path, 4096u) || path.find('\0') != std::string::npos) return reader.fail("invalid kernel host path");
            if (!reader.string(state.cwdDevice, 32u) ||
                (state.cwdDevice != "host0" && state.cwdDevice != "cdrom0" && state.cwdDevice != "cdrom" && state.cwdDevice != "host"))
                return reader.fail("invalid kernel working-directory device");
            uint32_t count = 0;
            auto objects = [&]() { return reader.pod(count) && (count <= kObjects || reader.fail("too many kernel host objects")); };
            if (!objects()) return false;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t key = 0; RpcServerState value;
                if (!reader.pod(key) || !reader.pod(value.sid) || !reader.pod(value.sd_ptr)) return false;
                if (key != value.sid || !guestAddress(value.sd_ptr) || !state.servers.emplace(key, value).second)
                    return reader.fail("invalid/duplicate kernel RPC server");
            }
            if (!objects()) return false;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t key = 0; RpcClientState value;
                if (!reader.pod(key) || !reader.pod(value.busy) || !reader.pod(value.last_rpc) || !reader.pod(value.sid)) return false;
                if (value.busy || !guestAddress(key) || !state.clients.emplace(key, value).second)
                    return reader.fail("busy/invalid/duplicate kernel RPC client");
            }
            if (!objects()) return false;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint8_t key = 0; uint32_t address = 0;
                if (!reader.pod(key) || !reader.pod(address)) return false;
                if (address < kBootModePoolBase || address >= kBootModePoolBase + state.bootOffset ||
                    (address & 3u) != 0u || !state.bootAddresses.emplace(key, address).second)
                    return reader.fail("invalid/duplicate kernel boot-mode entry");
            }
            if (!objects()) return false;
            for (uint32_t i = 0; i < count; ++i)
            {
                int32_t key = 0; SifModuleRecord value;
                if (!reader.pod(key) || !reader.pod(value.id) || !reader.string(value.path) || !reader.string(value.pathKey) ||
                    !reader.pod(value.refCount) || !reader.pod(value.loaded)) return false;
                if (key <= 0 || key != value.id || value.path.empty() || value.pathKey.empty() ||
                    value.path.find('\0') != std::string::npos || value.pathKey.find('\0') != std::string::npos ||
                    !state.modules.emplace(key, std::move(value)).second)
                    return reader.fail("invalid/duplicate kernel module record");
            }
            if (!objects()) return false;
            for (uint32_t i = 0; i < count; ++i)
            {
                std::string key; int32_t id = 0;
                if (!reader.string(key) || !reader.pod(id)) return false;
                const auto module = state.modules.find(id);
                if (key.empty() || module == state.modules.end() || module->second.pathKey != key ||
                    !state.modulePaths.emplace(std::move(key), id).second)
                    return reader.fail("inconsistent kernel module path index");
            }
            return reader.finish();
        }

        bool rpcIdle(std::string &error)
        {
            for (const auto &[address, client] : g_rpc_clients)
                if (client.busy)
                {
                    error = "Kernel RPC client " + std::to_string(address) + " is busy; retry after the callback completes";
                    return false;
                }
            return true;
        }
    }

    bool captureKernelHostState(std::vector<uint8_t> &bytes, std::string &error)
    {
        try
        {
            std::scoped_lock lock(g_sif_call_rpc_mutex, g_rpc_mutex, g_bootmode_mutex, g_tls_mutex, g_osd_mutex, g_ps2_path_mutex, g_sif_module_mutex);
            if (!rpcIdle(error)) return false;
            std::vector<uint8_t> candidate;
            ps2state::Writer writer(candidate, error, kLimit);
            if (!writer.pod(kMagic) || !writer.pod(kVersion)) return false;
            for (const bool value : {g_rpc_initialized, g_bootmode_initialized, g_osd_config_initialized, g_ps2_paths_initialized})
                if (!writer.pod(value)) return false;
            for (const uint32_t value : {g_rpc_next_id, g_rpc_packet_index, g_rpc_server_index, g_rpc_active_queue,
                                         g_bootmode_pool_offset, g_tls_index, g_osd_config_raw, g_osd_config2_raw})
                if (!writer.pod(value)) return false;
            if (!writer.pod(g_next_sif_module_id)) return false;
            for (const auto &path : {g_host_base, g_cdrom_base, g_host_cwd, g_cdrom_cwd})
                if (!writer.string(path.generic_string())) return false;
            if (!writer.string(g_ps2_cwd_device, 32u)) return false;
            if (!writeMap(writer, g_rpc_servers, [&](uint32_t key, const RpcServerState &value)
                { return writer.pod(key) && writer.pod(value.sid) && writer.pod(value.sd_ptr); }) ||
                !writeMap(writer, g_rpc_clients, [&](uint32_t key, const RpcClientState &value)
                { return writer.pod(key) && writer.pod(value.busy) && writer.pod(value.last_rpc) && writer.pod(value.sid); }) ||
                !writeMap(writer, g_bootmode_addresses, [&](uint8_t key, uint32_t value)
                { return writer.pod(key) && writer.pod(value); }) ||
                !writeMap(writer, g_sif_modules_by_id, [&](int32_t key, const SifModuleRecord &value)
                { return writer.pod(key) && writer.pod(value.id) && writer.string(value.path) && writer.string(value.pathKey) && writer.pod(value.refCount) && writer.pod(value.loaded); }) ||
                !writeMap(writer, g_sif_module_id_by_path, [&](const std::string &key, int32_t id)
                { return writer.string(key) && writer.pod(id); }) || !writer.finish()) return false;
            HostState checked;
            if (!readState(candidate, checked, error)) return false;
            bytes.swap(candidate);
            return true;
        }
        catch (const std::exception &exception) { error = exception.what(); return false; }
    }

    bool validateKernelHostState(const std::vector<uint8_t> &bytes, std::string &error)
    {
        try { HostState state; return readState(bytes, state, error); }
        catch (const std::exception &exception) { error = exception.what(); return false; }
    }

    bool restoreKernelHostState(const std::vector<uint8_t> &bytes, std::string &error)
    {
        try
        {
            HostState state;
            if (!readState(bytes, state, error)) return false;
            // Construct paths before mutation; restore does no filesystem IO.
            std::array<std::filesystem::path, 4> paths;
            for (size_t i = 0; i < paths.size(); ++i) paths[i] = state.paths[i];
            std::scoped_lock lock(g_sif_call_rpc_mutex, g_rpc_mutex, g_bootmode_mutex, g_tls_mutex, g_osd_mutex, g_ps2_path_mutex, g_sif_module_mutex);
            if (!rpcIdle(error)) return false;
            g_rpc_servers.swap(state.servers); g_rpc_clients.swap(state.clients); g_bootmode_addresses.swap(state.bootAddresses);
            g_sif_modules_by_id.swap(state.modules); g_sif_module_id_by_path.swap(state.modulePaths);
            g_rpc_initialized = state.rpcInitialized; g_bootmode_initialized = state.bootInitialized;
            g_osd_config_initialized = state.osdInitialized; g_ps2_paths_initialized = state.pathsInitialized;
            g_rpc_next_id = state.rpcNext; g_rpc_packet_index = state.packetIndex; g_rpc_server_index = state.serverIndex; g_rpc_active_queue = state.activeQueue;
            g_bootmode_pool_offset = state.bootOffset; g_tls_index = state.tlsIndex; g_osd_config_raw = state.osd; g_osd_config2_raw = state.osd2;
            g_next_sif_module_id = state.nextModule;
            g_host_base.swap(paths[0]); g_cdrom_base.swap(paths[1]); g_host_cwd.swap(paths[2]); g_cdrom_cwd.swap(paths[3]);
            g_ps2_cwd_device.swap(state.cwdDevice);
            // Diagnostic history contains const char* labels. Clear it rather
            // than pretending those process-local pointers are saved state.
            std::fill(std::begin(g_sif_rpc_debug_history), std::end(g_sif_rpc_debug_history), SifRpcDebugEvent{});
            g_sif_rpc_debug_next_seq = 0;
            g_sif_module_log_count = 0;
            error.clear();
            return true;
        }
        catch (const std::exception &exception) { error = exception.what(); return false; }
    }
}
