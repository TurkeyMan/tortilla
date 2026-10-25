// Prints the commit graph as text, for checking the lane algorithm:
//   tortilla-graphdump [path] [rows] [--all]
#include "git/repo.h"
#include "graph/lanes.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
    std::string path = ".";
    size_t limit = 40;
    tortilla::walk_options opts;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--all") == 0)
            opts.refs = tortilla::walk_refs::all;
        else if (std::isdigit(static_cast<unsigned char>(argv[i][0])))
            limit = std::stoul(argv[i]);
        else
            path = argv[i];
    }

    auto repo = tortilla::repo::open(path);
    if (!repo)
    {
        std::fprintf(stderr, "%s\n", repo.error().c_str());
        return 1;
    }
    auto refs = repo->refs();

    tortilla::lane_builder lanes;
    size_t n = 0;
    auto result =
        repo->walk(opts,
                   [&](tortilla::commit&& c)
                   {
                       auto row = lanes.next(c.id, c.parents);
                       // Each lane gets two characters: top half and bottom half markers.
                       std::string top(row.width * 2, ' '), bottom(row.width * 2, ' ');
                       for (auto& s : row.top)
                           top[s.from * 2] = s.from == s.to ? '|' : s.from < s.to ? '\\' : '/';
                       for (auto& s : row.bottom)
                           bottom[s.to * 2] = s.from == s.to ? '|' : s.from < s.to ? '\\' : '/';
                       top[row.node * 2] = row.merge ? 'M' : row.tip ? 'o' : '*';
                       std::string labels;
                       if (auto it = refs.find(c.id); it != refs.end())
                           for (auto& r : it->second) labels += "[" + r.name + "] ";
                       std::printf("%-16s %s %s%s\n%-16s\n", top.c_str(), c.id.hex(8).c_str(),
                                   labels.c_str(), c.summary.c_str(), bottom.c_str());
                       return ++n < limit;
                   });
    if (!result)
    {
        std::fprintf(stderr, "%s\n", result.error().c_str());
        return 1;
    }
}
