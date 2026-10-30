#include "blame.h"

#include <cstdlib>
#include <unordered_map>

namespace tortilla
{

std::vector<std::string> blame_args(const std::string& path, const blame_options& o)
{
    std::vector<std::string> args{"blame", "--porcelain"};
    if (o.ignore_whitespace) args.push_back("-w");
    if (o.detect_moves) args.insert(args.end(), {"-M", "-C"});
    if (!o.revision.empty()) args.push_back(o.revision);
    args.insert(args.end(), {"--", path});
    return args;
}

// Porcelain: for each line a header "<hash> <original> <final> [<count>]",
// then, the first time a commit appears, its fields ("author ...",
// "summary ...", "previous <hash> <name>", "boundary"), then
// "filename <name>" and the line itself after a tab.
std::vector<blame_line> parse_blame(std::string_view porcelain)
{
    std::vector<blame_line> lines;
    std::unordered_map<std::string, std::shared_ptr<blame_commit>> commits;
    std::shared_ptr<blame_commit> current;
    std::unordered_map<std::string, std::string> filenames;  // a commit's last "filename"
    size_t original = 0;
    size_t at = 0;
    while (at < porcelain.size())
    {
        size_t end = porcelain.find('\n', at);
        if (end == std::string_view::npos) end = porcelain.size();
        std::string_view line = porcelain.substr(at, end - at);
        at = end + 1;
        if (line.empty()) continue;

        if (line.front() == '\t')
        {
            if (!current) continue;
            blame_line out;
            out.commit = current;
            out.filename = filenames[current->hash];
            out.original_line = original;
            out.text = std::string(line.substr(1));
            lines.push_back(std::move(out));
            continue;
        }
        const size_t space = line.find(' ');
        const std::string_view key = line.substr(0, space);
        const std::string_view value =
            space == std::string_view::npos ? std::string_view() : line.substr(space + 1);
        // A header: forty hex digits, then the line numbers.
        if (key.size() == 40 && key.find_first_not_of("0123456789abcdef") == std::string_view::npos)
        {
            auto& c = commits[std::string(key)];
            if (!c)
            {
                c = std::make_shared<blame_commit>();
                c->hash = std::string(key);
            }
            current = c;
            original =
                std::strtoull(std::string(value.substr(0, value.find(' '))).c_str(), nullptr, 10);
            continue;
        }
        if (!current) continue;
        blame_commit& c = *current;
        if (key == "author")
            c.author = std::string(value);
        else if (key == "author-mail")
        {
            std::string_view mail = value;
            if (mail.size() >= 2 && mail.front() == '<' && mail.back() == '>')
                mail = mail.substr(1, mail.size() - 2);
            c.author_email = std::string(mail);
        }
        else if (key == "author-time")
            c.author_time = std::strtoll(std::string(value).c_str(), nullptr, 10);
        else if (key == "committer")
            c.committer = std::string(value);
        else if (key == "committer-time")
            c.committer_time = std::strtoll(std::string(value).c_str(), nullptr, 10);
        else if (key == "summary")
            c.summary = std::string(value);
        else if (key == "boundary")
            c.boundary = true;
        else if (key == "previous")
        {
            const size_t gap = value.find(' ');
            c.previous_hash = std::string(value.substr(0, gap));
            if (gap != std::string_view::npos)
                c.previous_filename = std::string(value.substr(gap + 1));
        }
        else if (key == "filename")
            filenames[c.hash] = std::string(value);
    }
    return lines;
}

std::vector<std::string> file_revision_args(const std::string& path, const std::string& from)
{
    // Each commit: \x1e, its fields between \x1f, then (--name-only) the
    // file's name in it on a line of its own.
    std::vector<std::string> args{"-c",          "core.quotepath=false",
                                  "log",         "--follow",
                                  "--name-only", "--format=%x1e%H%x1f%at%x1f%an%x1f%s"};
    if (!from.empty()) args.push_back(from);
    args.insert(args.end(), {"--", path});
    return args;
}

std::vector<file_revision> parse_file_revisions(std::string_view output)
{
    std::vector<file_revision> out;
    size_t at = output.find('\x1e');
    while (at != std::string_view::npos)
    {
        const size_t next = output.find('\x1e', at + 1);
        std::string_view record = output.substr(
            at + 1, next == std::string_view::npos ? std::string_view::npos : next - at - 1);
        at = next;
        const size_t nl = record.find('\n');
        std::string_view head = record.substr(0, nl);
        std::string_view rest =
            nl == std::string_view::npos ? std::string_view() : record.substr(nl + 1);
        std::vector<std::string_view> fields;
        for (size_t p = 0;;)
        {
            const size_t q = head.find('\x1f', p);
            fields.push_back(
                head.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p));
            if (q == std::string_view::npos) break;
            p = q + 1;
        }
        if (fields.size() < 4) continue;
        file_revision r;
        r.hash = std::string(fields[0]);
        r.time = std::strtoll(std::string(fields[1]).c_str(), nullptr, 10);
        r.author = std::string(fields[2]);
        r.summary = std::string(fields[3]);
        // The name: the first non-empty line after the header.
        while (!rest.empty())
        {
            const size_t e = rest.find('\n');
            std::string_view line = rest.substr(0, e);
            rest = e == std::string_view::npos ? std::string_view() : rest.substr(e + 1);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!line.empty())
            {
                r.path = std::string(line);
                break;
            }
        }
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace tortilla
