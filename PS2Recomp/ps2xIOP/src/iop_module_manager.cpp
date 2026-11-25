#include "iop_module_manager.h"

#include "ps2x/iop/ps2_path.h"
#include "ps2x/iop/save_state_archive.h"

#include <algorithm>
#include <limits>
#include <map>

namespace ps2x::iop::detail
{
    IopModuleManager::IopModuleManager()
    {
        // ROM modules that the no-BIOS HLE environment can legitimately provide. 
        // Entries with RPC services become routable only after load.
        constexpr std::string_view modules[] = {
            "sysmem",
            "loadcore",
            "intrman",
            "sifman",
            "sifcmd",
            "sifinit",
            "ioman",
            "iomanx",
            "modload",
            "stdio",
            "sysclib",
            "thbase",
            "thevent",
            "thsemap",
            "thmsgbx",
            "timrman",
            "vblank",
            "secrman",
            "sio2man",
            "xsio2man",
            "sio2d",
            "padman",
            "xpadman",
            "mcman",
            "xmcman",
            "mcserv",
            "libsd",
            "cdvdman",
            "cdvdfsv",
            "dev9",
            "usbd",
            "usbhdfsd",
            "udnl",
            "fileio",
            "poweroff",
            "netman",
            "ps2ip",
            "dbcman",
            "dbcm",
        };
        for (const std::string_view module : modules)
            m_builtinKeys.emplace(module);
    }

    void IopModuleManager::reset()
    {
        m_records.clear();
        m_hleIdsByKey.clear();
        m_loadedKeyReferences.clear();
        m_nextHleId = 0x40000000;
    }

    void IopModuleManager::setServiceModuleKeys(std::vector<std::string> keys)
    {
        m_serviceKeys.clear();
        for (std::string &key : keys)
        {
            const std::string normalized = ps2PathLeafKey(key);
            if (!normalized.empty())
                m_serviceKeys.emplace(normalized);
        }
    }

    ModuleLoadResult IopModuleManager::loadHle(std::string_view path)
    {
        ModuleLoadResult result{true, -1, -1};
        const std::string key = ps2PathLeafKey(path);
        if (key.empty() || (!m_builtinKeys.contains(key) && !m_serviceKeys.contains(key)))
            return result;

        const auto existing = m_hleIdsByKey.find(key);
        if (existing != m_hleIdsByKey.end())
        {
            Record &record = m_records[existing->second];
            ++record.references;
            addLoadedKey(key);
            result.moduleId = existing->second;
            result.startResult = 0;
            return result;
        }

        if (m_nextHleId <= 0)
            return result;
        const int32_t id = m_nextHleId++;
        m_records.emplace(id, Record{key, 1u, false});
        m_hleIdsByKey.emplace(key, id);
        addLoadedKey(key);
        result.moduleId = id;
        result.startResult = 0;
        return result;
    }

    void IopModuleManager::observePhysicalLoad(int32_t moduleId, std::string_view path)
    {
        if (moduleId <= 0)
            return;
        const std::string key = ps2PathLeafKey(path);
        if (key.empty())
            return;
        m_records[moduleId] = Record{key, 1u, true};
        addLoadedKey(key);
    }

    bool IopModuleManager::stopHle(int32_t moduleId, int32_t *result)
    {
        const auto found = m_records.find(moduleId);
        if (found == m_records.end() || found->second.physical)
            return false;

        Record &record = found->second;
        removeLoadedKey(record.key);
        if (record.references > 1u)
        {
            --record.references;
        }
        else
        {
            m_hleIdsByKey.erase(record.key);
            m_records.erase(found);
        }
        if (result)
            *result = 0;
        return true;
    }

    void IopModuleManager::observePhysicalStop(int32_t moduleId)
    {
        const auto found = m_records.find(moduleId);
        if (found == m_records.end() || !found->second.physical)
            return;
        removeLoadedKey(found->second.key);
        m_records.erase(found);
    }

    bool IopModuleManager::isLoaded(std::span<const std::string_view> aliases) const
    {
        if (aliases.empty())
            return true;
        return std::any_of(aliases.begin(), aliases.end(), [&](std::string_view alias)
                           {
                               const std::string key = ps2PathLeafKey(alias);
                               const auto found = m_loadedKeyReferences.find(key);
                               return found != m_loadedKeyReferences.end() && found->second != 0u; });
    }

    bool IopModuleManager::recognizes(std::string_view path) const
    {
        const std::string key = ps2PathLeafKey(path);
        return m_builtinKeys.contains(key) || m_serviceKeys.contains(key);
    }

    void IopModuleManager::addLoadedKey(std::string_view key)
    {
        ++m_loadedKeyReferences[std::string(key)];
    }

    void IopModuleManager::removeLoadedKey(std::string_view key)
    {
        const auto found = m_loadedKeyReferences.find(std::string(key));
        if (found == m_loadedKeyReferences.end())
            return;
        if (found->second > 1u)
            --found->second;
        else
            m_loadedKeyReferences.erase(found);
    }
}

namespace ps2x::iop::detail
{
    namespace
    {
        struct ModuleRecordChunk { int32_t id = 0; std::string key; uint32_t references = 0; bool physical = false; };
        struct ModuleManagerChunk { int32_t next = 0; std::vector<ModuleRecordChunk> rows; };

        bool decodeModuleManager(const std::vector<uint8_t> &data, ModuleManagerChunk &x, std::string &error)
        {
            ps2state::Reader r(data, error);
            uint32_t magic = 0, version = 0, endian = 0, count = 0;
            if (!r.pod(magic) || !r.pod(version) || !r.pod(endian) || magic != 0x31444f4du ||
                version != 1 || endian != 0x01020304u) return r.fail("unsupported IOP module state");
            if (!r.pod(x.next) || !r.pod(count)) return false;
            if (x.next < 0x40000000 || count > 4096) return r.fail("invalid IOP module count/next ID");
            std::unordered_set<int32_t> ids;
            std::unordered_set<std::string> hleKeys;
            std::unordered_map<std::string, uint32_t> refs;
            for (uint32_t i = 0; i < count; ++i)
            {
                ModuleRecordChunk row;
                if (!r.pod(row.id) || !r.string(row.key, 256) || !r.pod(row.references) || !r.pod(row.physical)) return false;
                if (row.id <= 0 || row.key.empty() || ps2PathLeafKey(row.key) != row.key || !row.references ||
                    !ids.emplace(row.id).second || (row.physical && row.id >= 0x40000000) ||
                    (!row.physical && (row.id < 0x40000000 || row.id >= x.next || !hleKeys.emplace(row.key).second)) ||
                    row.references > UINT32_MAX - refs[row.key]) return r.fail("invalid IOP module record");
                refs[row.key] += row.references;
                x.rows.push_back(std::move(row));
            }
            return r.finish();
        }
    }

    bool IopModuleManager::captureState(std::vector<uint8_t> &out, std::string &error) const
    {
        ps2state::Writer w(out, error);
        w.pod(uint32_t{0x31444f4du}); w.pod(uint32_t{1}); w.pod(uint32_t{0x01020304u});
        w.pod(m_nextHleId); w.pod(static_cast<uint32_t>(m_records.size()));
        std::map<int32_t, Record> sorted(m_records.begin(), m_records.end());
        for (const auto &[id, record] : sorted)
        { w.pod(id); w.string(record.key, 256); w.pod(record.references); w.pod(record.physical); }
        return w.finish();
    }

    bool IopModuleManager::validateState(const std::vector<uint8_t> &data, std::string &error) const
    {
        ModuleManagerChunk x;
        if (!decodeModuleManager(data, x, error)) return false;
        for (const auto &row : x.rows)
            if (!row.physical && !recognizes(row.key)) { error = "IOP HLE provider is unavailable"; return false; }
        return true;
    }

    bool IopModuleManager::restoreState(const std::vector<uint8_t> &data, std::string &error)
    {
        ModuleManagerChunk x;
        if (!decodeModuleManager(data, x, error)) return false;
        std::unordered_map<int32_t, Record> records;
        std::unordered_map<std::string, int32_t> hleIds;
        std::unordered_map<std::string, uint32_t> refs;
        for (auto &row : x.rows)
        {
            if (!row.physical && !recognizes(row.key)) { error = "IOP HLE provider is unavailable"; return false; }
            if (!row.physical) hleIds.emplace(row.key, row.id);
            refs[row.key] += row.references;
            records.emplace(row.id, Record{std::move(row.key), row.references, row.physical});
        }
        m_records.swap(records); m_hleIdsByKey.swap(hleIds); m_loadedKeyReferences.swap(refs); m_nextHleId = x.next;
        return true;
    }
}
