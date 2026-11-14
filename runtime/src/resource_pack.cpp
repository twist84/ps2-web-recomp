#include "ps2/resource_pack.hpp"
#include "resource_pack_zip.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <system_error>

namespace ps2::resource_pack
{
namespace
{
namespace fs = std::filesystem;
std::vector<s_pack_info> g_packs;
std::mutex g_selection_mutex;
std::vector<std::string> g_pending_selection;
bool g_selection_pending = false;
std::vector<s_pack_info> g_discovered;
fs::path g_pack_directory;
constexpr std::uint64_t k_max_resource = 64ull * 1024 * 1024;

// The pack manifest is intentionally small. This parser validates JSON
// structure and rejects unknown escaping, so malformed manifests never
// accidentally enable a pack.
struct s_parser
{
    const std::string &m_text;
    std::size_t m_pos = 0;

    void spaces()
    {
        for (; m_pos < m_text.size()
            && std::isspace(static_cast<unsigned char>(m_text[m_pos])); ++m_pos) {}
    }

    bool take(char ch)
    {
        spaces();
        if (m_pos >= m_text.size() || m_text[m_pos] != ch) return false;
        ++m_pos;
        return true;
    }

    bool string(std::string &output)
    {
        output.clear();
        if (!take('"')) return false;
        for (; m_pos < m_text.size(); ++m_pos)
        {
            const unsigned char ch = static_cast<unsigned char>(m_text[m_pos]);
            if (ch == '"')
            {
                ++m_pos;
                return true;
            }
            if (ch < 32 || ch == '\\')
            {
                // Pack identifiers/paths are ASCII; reject JSON escapes to
                // avoid alternate representations of directory separators.
                return false;
            }
            output.push_back(static_cast<char>(ch));
        }
        return false;
    }

    bool number(std::int32_t &value)
    {
        spaces();
        bool neg = take('-');
        spaces();
        if (m_pos == m_text.size() || !std::isdigit(
                static_cast<unsigned char>(m_text[m_pos]))) return false;
        std::int64_t n = 0;
        const std::size_t start = m_pos;
        for (; m_pos < m_text.size()
            && std::isdigit(static_cast<unsigned char>(m_text[m_pos])); ++m_pos)
        {
            n = n * 10 + m_text[m_pos] - '0';
            if (n > 2147483648ll) return false;
        }
        if (m_pos - start > 1 && m_text[start] == '0') return false;
        n = neg ? -n : n;
        if (n < -2147483648ll || n > 2147483647ll) return false;
        value = static_cast<std::int32_t>(n);
        return true;
    }

    bool object(std::map<std::string, std::string> &fields)
    {
        if (!take('{')) return false;
        if (take('}')) return true;
        do
        {
            std::string key, value;
            if (!string(key) || !take(':')) return false;
            spaces();
            if (m_pos >= m_text.size()) return false;
            if (m_text[m_pos] == '"')
            {
                if (!string(value)) return false;
            }
            else
            {
                std::int32_t n = 0;
                if (!number(n)) return false;
                value = std::to_string(n);
            }
            if (!fields.emplace(key, value).second) return false;
            if (take('}')) return true;
        } while (take(','));
        return false;
    }

    bool manifest(std::map<std::string, std::string> &fields)
    {
        if (!take('{')) return false;
        std::string key;
        if (!string(key) || key != "pack" || !take(':') || !object(fields)
            || !take('}')) return false;
        spaces();
        return m_pos == m_text.size();
    }
};

bool contained(const fs::path &root, const fs::path &candidate)
{
    auto a = root.begin();
    auto b = candidate.begin();
    for (; a != root.end(); ++a, ++b)
        if (b == candidate.end() || *a != *b) return false;
    return true;
}

bool valid_component(const std::string &component)
{
    if (component.empty() || component == "." || component == "..") return false;
    for (unsigned char ch : component)
        if (!(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.'))
            return false;
    return true;
}

bool valid_logical_path(const std::string &path)
{
    if (path.empty() || path[0] == '/' || path.back() == '/'
        || path.find('\\') != std::string::npos
        || path.find(':') != std::string::npos) return false;
    std::size_t start = 0;
    for (std::size_t end = 0;; ++end)
    {
        if (end == path.size() || path[end] == '/')
        {
            if (!valid_component(path.substr(start, end - start)))
                return false;
            if (end == path.size()) return true;
            start = end + 1;
        }
    }
}

bool valid_pack_id(const std::string &id)
{
    if (id.empty() || id.size() > 80 || id.front() == ' '
        || id.back() == ' ' || id == "." || id == "..")
        return false;
    for (unsigned char ch : id)
        if (!(std::isalnum(ch) || ch == '_' || ch == '-'
            || ch == '.' || ch == ' '))
            return false;
    return true;
}

void warn(const fs::path &path, const char *reason)
{
    std::fprintf(stderr, "[resource-pack] ignoring %s: %s\n",
        path.string().c_str(), reason);
}

bool load_pack(const fs::path &source_path, s_pack_info &pack,
    bool is_archive)
{
    std::string source;
    if (is_archive)
    {
        if (!zip::index(source_path, pack.m_entries)) return false;
        const auto it = std::find_if(pack.m_entries.begin(), pack.m_entries.end(),
            [](const s_zip_entry &entry)
            {
                return entry.m_path == "pack.json" && entry.m_size <= 16384;
            });
        if (it == pack.m_entries.end()) return false;
        std::vector<std::uint8_t> data;
        if (!zip::read(source_path, *it, data)) return false;
        source.assign(data.begin(), data.end());
    }
    else
    {
        const auto manifest_path = source_path / "pack.json";
        std::error_code error;
        if (!fs::is_regular_file(manifest_path, error) || error)
            return false;
        const auto size = fs::file_size(manifest_path, error);
        if (error || size > 16384) return false;
        std::ifstream input(manifest_path, std::ios::binary);
        if (!input) return false;
        source.assign(std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }
    std::map<std::string, std::string> fields;
    s_parser parser{source};
    if (!parser.manifest(fields)) return false;
    if (fields["format"] != "1" || fields["game"] != "sdbz"
        || (fields.count("region") && fields["region"] != "eu-beta"))
        return false;

    pack.m_id = is_archive ? source_path.stem().string()
        : source_path.filename().string();
    if (!valid_pack_id(pack.m_id)) return false;
    pack.m_name = fields.count("name") ? fields["name"] : pack.m_id;
    pack.m_priority = 0;
    if (fields.count("priority"))
    {
        const std::string &value = fields["priority"];
        s_parser priority_parser{value};
        if (!priority_parser.number(pack.m_priority)
            || priority_parser.m_pos != value.size()) return false;
    }
    // Avoid absolute priorities that would overflow future ordering rules.
    if (pack.m_priority < -100000 || pack.m_priority > 100000) return false;
    pack.m_root = fs::canonical(source_path);
    pack.m_archive = is_archive;
    return true;
}

} // namespace

void clear()
{
    std::lock_guard<std::mutex> lock(g_selection_mutex);
    g_selection_pending = false;
    g_pending_selection.clear();
    g_packs.clear();
    g_discovered.clear();
    g_pack_directory.clear();
}

void mount(const fs::path &disc_root,
    const fs::path &executable_directory)
{
    clear();
    std::error_code error;
    // Standalone builds keep resource packs beside SDBZ.exe. For existing
    // development layouts, retain the disc-adjacent location as a fallback.
    const fs::path executable_base = executable_directory / "resourcepacks";
    const fs::path fallback_base = disc_root.parent_path() / "resourcepacks";
    const fs::path base = !executable_directory.empty()
        && fs::is_directory(executable_base, error) && !error
        ? executable_base : fallback_base;
    error.clear();
    if (!fs::is_directory(base, error)) return;
    const fs::path canonical_base = fs::canonical(base, error);
    if (error) return;
    g_pack_directory = canonical_base;
    std::set<std::string> ids;
    for (fs::directory_iterator it(base, error), end; !error && it != end;
        it.increment(error))
    {
        if (it->is_symlink(error) || error) continue;
        const bool is_directory = it->is_directory(error);
        if (error) continue;
        const bool is_archive = !is_directory
            && it->is_regular_file(error) && !error
            && it->path().extension() == ".zip";
        if (!is_directory && !is_archive) continue;
        const auto source = fs::canonical(it->path(), error);
        if (error || !contained(canonical_base, source)) continue;
        s_pack_info info;
        if (!load_pack(source, info, is_archive))
        {
            warn(source, "invalid or incompatible pack.json or ZIP");
            continue;
        }
        std::string id = info.m_id;
        std::transform(id.begin(), id.end(), id.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (!ids.insert(id).second)
        {
            warn(source, "duplicate pack ID");
            continue;
        }
        std::fprintf(stderr, "[resource-pack] detected %s (%s) priority %d\n",
            info.m_id.c_str(), is_archive ? "zip" : "folder", info.m_priority);
        g_packs.push_back(std::move(info));
    }
    // Optional user-controlled order. Explicitly listed packs precede
    // unlisted packs (which retain their manifest priority ordering).
    // Prefix + enables a pack; - disables it. Names are folder IDs.
    std::map<std::string, std::size_t> explicit_order;
    std::set<std::string> disabled;
    const fs::path order_file = canonical_base / "load_order.txt";
    error.clear();
    if (fs::is_regular_file(order_file, error) && !error
        && !fs::is_symlink(order_file, error) && !error)
    {
        error.clear();
        const auto bytes = fs::file_size(order_file, error);
        if (!error && bytes <= 16384)
        {
            std::ifstream input(order_file);
            std::string line;
            std::size_t line_number = 0;
            while (line_number < 256 && std::getline(input, line))
            {
                ++line_number;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                const char flag = line[0];
                const std::string name = line.substr(1);
                if ((flag != '+' && flag != '-') || !valid_pack_id(name))
                {
                    std::fprintf(stderr,
                        "[resource-pack] ignoring invalid order entry at line %zu\n",
                        line_number);
                    continue;
                }
                std::string key = name;
                std::transform(key.begin(), key.end(), key.begin(),
                    [](unsigned char ch)
                    {
                        return static_cast<char>(std::tolower(ch));
                    });
                if (!explicit_order.emplace(key, line_number).second)
                {
                    std::fprintf(stderr,
                        "[resource-pack] ignoring duplicate order entry at line %zu\n",
                        line_number);
                    continue;
                }
                if (flag == '-') disabled.insert(key);
            }
        }
        else
        {
            warn(order_file, "load order exceeds 16 KiB");
        }
    }

    g_discovered = g_packs;

    g_packs.erase(std::remove_if(g_packs.begin(), g_packs.end(),
        [&disabled](const s_pack_info &pack)
        {
            std::string key = pack.m_id;
            std::transform(key.begin(), key.end(), key.begin(),
                [](unsigned char ch)
                {
                    return static_cast<char>(std::tolower(ch));
                });
            if (!disabled.count(key)) return false;
            std::fprintf(stderr, "[resource-pack] disabled %s\n",
                pack.m_id.c_str());
            return true;
        }), g_packs.end());

    std::sort(g_packs.begin(), g_packs.end(),
        [&explicit_order](const s_pack_info &a, const s_pack_info &b)
        {
            auto lookup = [&explicit_order](const std::string &id)
            {
                std::string key = id;
                std::transform(key.begin(), key.end(), key.begin(),
                    [](unsigned char ch)
                    {
                        return static_cast<char>(std::tolower(ch));
                    });
                return explicit_order.find(key);
            };
            const auto ai = lookup(a.m_id);
            const auto bi = lookup(b.m_id);
            if ((ai != explicit_order.end()) != (bi != explicit_order.end()))
                return ai != explicit_order.end();
            if (ai != explicit_order.end() && bi != explicit_order.end())
                return ai->second < bi->second;
            if (a.m_priority != b.m_priority)
                return a.m_priority > b.m_priority;
            return a.m_id < b.m_id;
        });
}

const std::vector<s_pack_info> &packs()
{
    return g_packs;
}

const std::vector<s_pack_info> &discovered_packs()
{
    return g_discovered;
}

std::vector<s_pack_info> active_packs_snapshot()
{
    std::lock_guard<std::mutex> lock(g_selection_mutex);
    return g_packs;
}

bool selection_pending()
{
    std::lock_guard<std::mutex> lock(g_selection_mutex);
    return g_selection_pending;
}

bool apply_pending_selection()
{
    std::lock_guard<std::mutex> lock(g_selection_mutex);
    if (!g_selection_pending) return false;
    g_selection_pending = false;
    std::vector<s_pack_info> active;
    for (const auto &id : g_pending_selection)
    {
        const auto it = std::find_if(g_discovered.begin(), g_discovered.end(),
            [&id](const s_pack_info &pack)
            {
                return pack.m_id == id;
            });
        if (it != g_discovered.end())
            active.push_back(*it);
    }
    g_packs = std::move(active);
    std::fprintf(stderr,
        "[resource-pack] applied %zu enabled packs without restarting\n",
        g_packs.size());
    return true;
}

bool save_selection(const std::vector<std::string> &enabled_ids)
{
    if (g_pack_directory.empty()) return false;
    std::set<std::string> known, enabled;
    for (const auto &pack : g_discovered)
    {
        known.insert(pack.m_id);
    }
    for (const auto &id : enabled_ids)
    {
        if (!valid_pack_id(id) || !known.count(id)
            || !enabled.insert(id).second)
            return false;
    }
    const fs::path file = g_pack_directory / "load_order.txt";
    std::error_code error;
    if (fs::is_symlink(file, error)) return false;
    // A new install has no load_order.txt yet. ENOENT is expected when
    // creating that file, but other filesystem failures must be rejected.
    if (error && error != std::make_error_code(
            std::errc::no_such_file_or_directory))
        return false;
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << "# SDBZ resource packs: top enabled pack has highest priority\n";
    for (const auto &id : enabled_ids) output << '+' << id << '\n';
    for (const auto &pack : g_discovered)
    {
        if (!enabled.count(pack.m_id))
            output << '-' << pack.m_id << '\n';
    }
    output.flush();
    if (!output.good()) return false;
    // The UI thread only publishes the selection. The GS render thread
    // installs it between commands, before invalidating its native caches.
    {
        std::lock_guard<std::mutex> lock(g_selection_mutex);
        g_pending_selection = enabled_ids;
        g_selection_pending = true;
    }
    return true;
}


std::vector<std::string> texture_registries()
{
    std::vector<std::string> result;
    std::lock_guard<std::mutex> lock(g_selection_mutex);
    // The built-in registry is a data file, not a compiled fingerprint table.
    // It remains available even if every resource pack is disabled.
    if (!g_pack_directory.empty())
    {
        const auto base = g_pack_directory.parent_path()
            / "native_textures.registry";
        std::error_code error;
        const auto size = fs::file_size(base, error);
        if (!error && size && size <= k_max_resource)
        {
            std::ifstream input(base, std::ios::binary);
            if (input)
                result.emplace_back(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
        }
    }
    // Scan discovered, not only enabled, packs: a source identity must
    // remain known when a replacement pack is disabled and re-enabled.
    for (const auto &pack : g_discovered)
    {
        std::vector<std::uint8_t> bytes;
        if (pack.m_archive)
        {
            const auto it = std::find_if(pack.m_entries.begin(),
                pack.m_entries.end(), [](const s_zip_entry &entry)
                {
                    return entry.m_path == "assets/sdbz/textures.registry";
                });
            if (it == pack.m_entries.end() || it->m_size > k_max_resource
                || !zip::read(pack.m_root, *it, bytes))
                continue;
        }
        else
        {
            const fs::path file = pack.m_root / "assets"
                / "sdbz" / "textures.registry";
            std::error_code error;
            if (fs::is_symlink(file, error) || error
                || !fs::is_regular_file(file, error) || error)
                continue;
            const auto canonical = fs::canonical(file, error);
            if (error || !contained(pack.m_root, canonical))
                continue;
            const auto size = fs::file_size(canonical, error);
            if (error || !size || size > k_max_resource)
                continue;
            std::ifstream input(canonical, std::ios::binary);
            if (!input) continue;
            bytes.assign(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
        }
        result.emplace_back(bytes.begin(), bytes.end());
    }
    return result;
}

bool find_resource(const std::string &logical_path, s_resource &resource)
{
    std::lock_guard<std::mutex> lock(g_selection_mutex);
    resource = {};
    if (!valid_logical_path(logical_path)) return false;
    for (const auto &pack : g_packs)
    {
        if (pack.m_archive)
        {
            const std::string key = "assets/sdbz/" + logical_path;
            const auto it = std::find_if(pack.m_entries.begin(), pack.m_entries.end(),
                [&key](const s_zip_entry &entry)
                {
                    return entry.m_path == key;
                });
            if (it == pack.m_entries.end()) continue;
            resource = {pack.m_id, pack.m_root, it->m_size, true, *it};
            return true;
        }
        std::error_code error;
        const fs::path candidate = pack.m_root / "assets" / "sdbz"
            / fs::path(logical_path);
        if (fs::is_symlink(candidate, error) || error
            || !fs::is_regular_file(candidate, error) || error)
            continue;
        const fs::path canonical = fs::canonical(candidate, error);
        if (error || !contained(pack.m_root, canonical)) continue;
        const auto size = fs::file_size(canonical, error);
        if (error || !size || size > k_max_resource) continue;
        resource = {pack.m_id, canonical, size, false, {}};
        return true;
    }
    return false;
}

bool read_resource(const std::string &logical_path,
    std::vector<std::uint8_t> &data)
{
    data.clear();
    s_resource resource;
    if (!find_resource(logical_path, resource)) return false;
    if (resource.m_archive)
        return zip::read(resource.m_path, resource.m_entry, data);
    std::ifstream input(resource.m_path, std::ios::binary);
    if (!input) return false;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(resource.m_size));
    if (!input.read(reinterpret_cast<char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())))
        return false;
    data = std::move(bytes);
    return true;
}

}
