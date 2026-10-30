#include "archive.h"

#include "i18n.h"

#include <rapport/sys.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <set>

namespace tortilla
{

namespace fs = std::filesystem;

std::vector<std::string> archive_args(archive_format format, const std::string& rev,
                                      const std::string& file)
{
    return {"archive", format == archive_format::zip ? "--format=zip" : "--format=tar", "-o", file,
            rev};
}

std::string export_name(const std::string& workdir, const std::string& rev)
{
    std::string top = workdir;
    while (top.size() > 1 && (top.back() == '/' || top.back() == '\\')) top.pop_back();
    std::string name = rap::sys::path_to_utf8(rap::sys::path_from_utf8(top).filename());
    if (name.size() > 4 && name.ends_with(".git")) name.resize(name.size() - 4);
    if (name.empty() || name == ".git") name = "export";
    if (rev.empty() || rev == "HEAD") return name;
    std::string r = rev;
    for (const char* prefix : {"refs/heads/", "refs/tags/", "refs/remotes/", "remotes/"})
        if (r.starts_with(prefix))
        {
            r = r.substr(std::string_view(prefix).size());
            break;
        }
    const bool hash =
        r.size() == 40 &&
        std::ranges::all_of(r, [](char c)
                            { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    if (hash) r.resize(8);
    // A branch's folders, and what a file name can't hold.
    for (char& c : r)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|' || c == '~' || c == '^')
            c = '-';
    return name + "-" + r;
}

std::optional<fs::path> archive_entry_path(std::string_view name)
{
    if (name.empty() || name.front() == '/' || name.front() == '\\') return std::nullopt;
    const fs::path p = rap::sys::path_from_utf8(name);
    if (p.has_root_name() || p.has_root_directory()) return std::nullopt;
    fs::path out;
    for (const fs::path& part : p)
    {
        const std::string s = rap::sys::path_to_utf8(part);
        if (s.empty() || s == ".") continue;  // a folder's trailing separator
        if (s == "..") return std::nullopt;
#ifdef _WIN32
        // "name:stream" writes an NTFS alternate stream, not a file.
        if (s.find(':') != std::string::npos) return std::nullopt;
#endif
        out /= part;
    }
    if (out.empty()) return std::nullopt;
    return out;
}

namespace
{

constexpr size_t block = 512;

// A ustar number: octal, or base-256 with the top bit set (GNU's, for
// sizes past 8 GiB).
uint64_t tar_number(const char* field, size_t n)
{
    uint64_t v = 0;
    if (static_cast<unsigned char>(field[0]) & 0x80)
    {
        v = static_cast<unsigned char>(field[0]) & 0x7f;
        for (size_t i = 1; i < n; ++i) v = (v << 8) | static_cast<unsigned char>(field[i]);
        return v;
    }
    for (size_t i = 0; i < n && field[i]; ++i)
        if (field[i] >= '0' && field[i] <= '7') v = v * 8 + (field[i] - '0');
    return v;
}

std::string tar_string(const char* field, size_t n)
{
    return std::string(field, std::find(field, field + n, '\0'));
}

// pax records, "<length> <key>=<value>\n": git archive writes a long path
// (or link target) as one, and the commit's hash in a global one.
void read_pax(const std::string& data, std::string& path, std::string& link,
              std::optional<uint64_t>& size)
{
    size_t pos = 0;
    while (pos < data.size())
    {
        const size_t space = data.find(' ', pos);
        if (space == std::string::npos) return;
        size_t len = 0;
        for (size_t i = pos; i < space; ++i)
        {
            if (data[i] < '0' || data[i] > '9') return;
            len = len * 10 + (data[i] - '0');
        }
        if (len <= space - pos || pos + len > data.size()) return;
        std::string record = data.substr(space + 1, pos + len - space - 1);
        if (!record.empty() && record.back() == '\n') record.pop_back();
        pos += len;
        const size_t eq = record.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = record.substr(0, eq), value = record.substr(eq + 1);
        if (key == "path")
            path = value;
        else if (key == "linkpath")
            link = value;
        else if (key == "size")
            size = std::strtoull(value.c_str(), nullptr, 10);
    }
}

}  // namespace

extract_result extract_tar(const fs::path& tar, const fs::path& into)
{
    extract_result result;
    std::ifstream in(tar, std::ios::binary);
    if (!in)
    {
        result.error = rap::i18n::format(_("Could not open {0}."), rap::sys::path_to_utf8(tar));
        return result;
    }
    std::error_code ec;
    fs::create_directories(into, ec);
    if (!fs::is_directory(into, ec))
    {
        result.error =
            rap::i18n::format(_("Could not create the folder {0}."), rap::sys::path_to_utf8(into));
        return result;
    }

    // Folders found not to be symbolic links: a file is never written
    // through a link, one from the tree or one already in the folder.
    std::set<fs::path> real_dirs;
    auto through_link = [&](const fs::path& rel)
    {
        fs::path dir = into;
        for (const fs::path& part : rel.parent_path())
        {
            dir /= part;
            if (real_dirs.contains(dir)) continue;
            if (fs::is_symlink(fs::symlink_status(dir, ec))) return true;
            if (fs::is_directory(dir, ec)) real_dirs.insert(dir);
        }
        return false;
    };

    std::array<char, block> header;
    std::vector<char> buffer(64 * 1024);
    std::string pax_path, pax_link;
    std::optional<uint64_t> pax_size;
    const std::string damaged = _("The archive is damaged.");
    for (;;)
    {
        if (!in.read(header.data(), block))
        {
            result.error = damaged;
            return result;
        }
        if (std::all_of(header.begin(), header.end(), [](char c) { return c == 0; }))
            return result;  // the end
        if (std::string_view(header.data() + 257, 5) != "ustar")
        {
            result.error = damaged;
            return result;
        }
        const char type = header[156];
        const bool pax = type == 'x' || type == 'g';
        const uint64_t size = pax_size && !pax ? *pax_size : tar_number(header.data() + 124, 12);
        const uint64_t padded = (size + block - 1) / block * block;

        if (pax)
        {
            std::string data(size, '\0');
            if (size > (1u << 20) || !in.read(data.data(), size) || !in.ignore(padded - size))
            {
                result.error = damaged;
                return result;
            }
            if (type == 'x') read_pax(data, pax_path, pax_link, pax_size);
            continue;
        }

        std::string name = pax_path;
        if (name.empty())
        {
            name = tar_string(header.data(), 100);
            if (const std::string prefix = tar_string(header.data() + 345, 155); !prefix.empty())
                name = prefix + "/" + name;
        }
        const std::string link = pax_link.empty() ? tar_string(header.data() + 157, 100) : pax_link;
        const unsigned mode = static_cast<unsigned>(tar_number(header.data() + 100, 8));
        pax_path.clear(), pax_link.clear(), pax_size.reset();

        const std::optional<fs::path> rel = archive_entry_path(name);
        const bool safe = rel && !through_link(*rel);
        const fs::path target = rel ? into / *rel : fs::path{};
        bool written = safe;
        if (safe && type == '5')
            written = fs::create_directories(target, ec) || fs::is_directory(target, ec);
        else if (safe && type == '2')
        {
            fs::create_directories(target.parent_path(), ec);
            fs::remove(target, ec);
            fs::create_symlink(rap::sys::path_from_utf8(link), target, ec);
            if (ec)
            {
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                written = static_cast<bool>(out << link);
            }
        }

        // A file's data, read whether it's written or not, to reach the
        // next entry.
        std::ofstream out;
        if (safe && (type == '0' || type == '\0'))
        {
            fs::create_directories(target.parent_path(), ec);
            if (fs::is_symlink(fs::symlink_status(target, ec))) fs::remove(target, ec);
            out.open(target, std::ios::binary | std::ios::trunc);
            written = out.is_open();
        }
        for (uint64_t left = padded; left > 0;)
        {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(left, buffer.size()));
            if (!in.read(buffer.data(), n))
            {
                result.error = damaged;
                return result;
            }
            const uint64_t used = padded - left;
            if (out.is_open() && used < size)
                out.write(buffer.data(),
                          static_cast<std::streamsize>(std::min<uint64_t>(n, size - used)));
            left -= n;
        }
        if (out.is_open())
        {
            out.close();
            written = !out.fail();
#ifndef _WIN32
            if (written && (mode & 0111))
                fs::permissions(target, static_cast<fs::perms>(mode & 0111), fs::perm_options::add,
                                ec);
#endif
        }
        if (!written) result.failed.push_back(name);
    }
}

}  // namespace tortilla
