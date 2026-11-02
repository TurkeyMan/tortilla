#include "pathspec_file.h"

#include "git/temp_files.h"

#include <rapport/sys.h>

#include <system_error>

namespace fs = std::filesystem;

namespace tortilla
{

std::shared_ptr<pathspec_file> pathspec_file::write(const std::vector<std::string>& paths)
{
    const fs::path file = unique_temp_path("paths", ".txt");
    if (file.empty()) return nullptr;
    auto out = std::shared_ptr<pathspec_file>(new pathspec_file(file));
    std::string data;
    for (const std::string& p : paths)
    {
        data += p;
        data += '\0';
    }
    return write_file(file, data) ? out : nullptr;
}

pathspec_file::~pathspec_file()
{
    std::error_code ec;
    fs::remove(path_, ec);
}

std::vector<std::string> pathspec_file::arguments() const
{
    return {"--pathspec-from-file=" + rap::sys::path_to_utf8(path_), "--pathspec-file-nul"};
}

}  // namespace tortilla
