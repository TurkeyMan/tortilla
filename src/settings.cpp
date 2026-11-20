#include "settings.h"

#include "i18n.h"
#include "util/replaced_file.h"
#include "util/settings_format.h"
#include "util/settings_recovery.h"

#include <rapport/sys.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <aclapi.h>
#include <cstring>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;
using values = std::map<std::string, std::string>;
using steady = std::chrono::steady_clock;

thread_local settings_test_failure next_failure = settings_test_failure::none;
bool fail(settings_test_failure at)
{
    if (next_failure != at) return false;
    next_failure = settings_test_failure::none;
    errno = at == settings_test_failure::no_locks ? ENOLCK : EIO;
    return true;
}

std::string crt_error(int e) { return std::generic_category().message(e); }
#ifdef _WIN32
std::string native_error(DWORD e) { return std::system_category().message(int(e)); }
#endif

fs::path settings_file()
{
    return rap::sys::config_dir("tortilla") / "settings";
}

fs::path with_suffix(const fs::path& file, const std::string& suffix)
{
    fs::path out = file;
    out += rap::sys::path_from_utf8(suffix);
    return out;
}

std::string shown(const fs::path& p)
{
    return rap::sys::path_to_utf8(p);
}

// The settings read, lock and replacement must address the same file as native
// readers. Windows delegates reparse substitution to the OS; POSIX traverses
// links before parent components. Missing Windows link targets fail closed.
struct settings_target
{
    fs::path path;
    bool linked = false;
#ifdef _WIN32
    FILE_ID_INFO identity{};
    bool existing = false;
#endif
};

#ifdef _WIN32
thread_local fs::path final_candidate_for_tests;
thread_local bool opened_path_only_for_tests = false;
thread_local std::function<void(std::intptr_t)> temp_created_for_tests;
thread_local std::function<void()> before_lock_for_tests;

struct settings_native_handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~settings_native_handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

std::optional<fs::path> final_settings_path(HANDLE handle, DWORD& error,
                                            FILE_ID_INFO& original, bool& unstable)
{
    // https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew
    // Normalization can need ancestor permissions on SMB. Opened-name fallbacks
    // are safe only after reopening and matching the original volume/file ID.
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &original, sizeof original))
    { error = GetLastError(); return {}; }
    constexpr DWORD limit = 32768;
    for (DWORD spelling : {DWORD(FILE_NAME_NORMALIZED), DWORD(FILE_NAME_OPENED)})
    for (DWORD volume : {DWORD(VOLUME_NAME_DOS), DWORD(VOLUME_NAME_GUID), DWORD(VOLUME_NAME_NT)})
    {
        if (opened_path_only_for_tests && spelling == FILE_NAME_NORMALIZED)
        { error = ERROR_ACCESS_DENIED; continue; }
        std::vector<wchar_t> buffer(512);
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            error = ERROR_INSUFFICIENT_BUFFER;
            const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()),
                                                           spelling | volume);
            const DWORD native = length ? ERROR_SUCCESS : GetLastError();
            if (!length) { error = native; break; }
            if (length >= buffer.size())
            {
                if (length > limit) { error = ERROR_FILENAME_EXCED_RANGE; break; }
                buffer.resize(length);
                continue;
            }
            std::wstring path(buffer.data(), length);
            // NT returns \Device\..., not a Win32 pathname. GLOBALROOT selects
            // that namespace while remaining usable by CreateFile/_wfopen.
            if (volume == VOLUME_NAME_NT) path = L"\\\\?\\GLOBALROOT" + path;
            fs::path candidate = final_candidate_for_tests.empty() ?
                                     fs::path(std::move(path)) : final_candidate_for_tests;
            settings_native_handle checked{CreateFileW(candidate.c_str(), 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
            if (checked.value == INVALID_HANDLE_VALUE)
            {
                error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) unstable = true;
                break;
            }
            FILE_ID_INFO reopened{};
            if (!GetFileInformationByHandleEx(checked.value, FileIdInfo, &reopened, sizeof reopened))
            { error = GetLastError(); break; }
            if (original.VolumeSerialNumber != reopened.VolumeSerialNumber ||
                std::memcmp(original.FileId.Identifier, reopened.FileId.Identifier,
                            sizeof original.FileId.Identifier) != 0)
            { error = ERROR_FILE_INVALID; unstable = true; break; }
            return candidate;
        }
    }
    return {};
}
#endif

std::optional<settings_target> resolve(const fs::path& file, std::string& error, bool* changed = nullptr)
{
    auto failed = [&](const fs::path& at, const std::string& why)
    {
        error = rap::i18n::format(_("Couldn't resolve the settings path {0} at {1}: {2}"),
                                  shown(file), shown(at), why);
        return std::optional<settings_target>{};
    };
#ifdef _WIN32
    // https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-createfilew
    // Zero desired access queries metadata without requiring read/write rights.
    constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    // Open the original spelling, exactly as native readers do; qualifying
    // with GetFullPathName/fs::absolute could normalize before opening.
    fs::path candidate = file;
    std::vector<fs::path> missing;
    for (;;)
    {
        settings_native_handle handle{CreateFileW(candidate.c_str(), 0, share, nullptr, OPEN_EXISTING,
                                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
        const DWORD opened = handle.value == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (handle.value != INVALID_HANDLE_VALUE)
        {
            DWORD native = ERROR_SUCCESS;
            FILE_ID_INFO identity{};
            bool unstable = false;
            auto real = final_settings_path(handle.value, native, identity, unstable);
            if (!real)
            {
                if (changed) *changed = unstable;
                return failed(candidate, native_error(native));
            }
            for (auto it = missing.rbegin(); it != missing.rend(); ++it) *real /= *it;
            settings_target target{std::move(*real)};
            target.identity = identity;
            target.existing = missing.empty();
            return target;
        }
        if (opened != ERROR_FILE_NOT_FOUND && opened != ERROR_PATH_NOT_FOUND)
            return failed(candidate, native_error(opened));
        // OPEN_REPARSE_POINT can open a dangling link itself. Never mistake
        // that for a missing plain filename and replace it or create its target.
        settings_native_handle link{CreateFileW(candidate.c_str(), 0, share, nullptr, OPEN_EXISTING,
                                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                                 nullptr)};
        const DWORD probed = link.value == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (link.value != INVALID_HANDLE_VALUE)
            return failed(candidate, _("The settings path exists, but its target could not be opened."));
        if (probed != ERROR_FILE_NOT_FOUND && probed != ERROR_PATH_NOT_FOUND)
            return failed(candidate, native_error(probed));
        auto parent = candidate.parent_path();
        if (parent.empty() && candidate.has_filename()) parent = ".";
        if (parent == candidate || candidate.filename().empty())
            return failed(candidate, native_error(opened));
        missing.push_back(candidate.filename());
        candidate = parent;
    }
#else
    std::error_code ec;
    const fs::path absolute = fs::absolute(file, ec);
    if (ec) return failed(file, ec.message());
    const auto initial = split_settings_path(absolute);
    settings_target t{initial.root};
    settings_path_cursor cursor(initial.root);
    std::deque<fs::path> todo(initial.components.begin(), initial.components.end());
    int links = 0;
    while (!todo.empty())
    {
        const fs::path part = todo.front();
        todo.pop_front();
        if (part.empty() || part == ".") continue;
        if (part == "..")
        {
            cursor.step(part);
            continue;
        }
        const fs::path next = cursor.path() / part;
        // symlink_status/read_symlink use lstat/readlink on POSIX. Never
        // normalize a suffix before traversing it: missing/.. is an error.
        const auto status = fs::symlink_status(next, ec);
        if (status.type() == fs::file_type::symlink)
        {
            const auto target = fs::read_symlink(next, ec);
            if (ec) return failed(next, ec.message());
            if (++links > 40)
                return failed(next, std::make_error_code(std::errc::too_many_symbolic_link_levels).message());
            const auto parts = split_settings_path(target);
            if (target.is_absolute()) cursor = settings_path_cursor(parts.root);
            else if (target.has_root_directory()) cursor = settings_path_cursor(cursor.root());
            else if (target.has_root_name())
                return failed(next, std::make_error_code(std::errc::invalid_argument).message());
            todo.insert(todo.begin(), parts.components.begin(), parts.components.end());
            t.linked = true;
            continue;
        }
        if (status.type() == fs::file_type::not_found)
        {
            // A dangling final link is supported, but its parent must exist.
            // A plain first-save config directory may still be created below;
            // only a simple missing suffix is allowed, never missing/.. .
            if (!todo.empty() &&
                (t.linked || std::find(todo.begin(), todo.end(), fs::path("..")) != todo.end()))
                return failed(next, std::make_error_code(std::errc::no_such_file_or_directory).message());
            cursor.step(part);
            for (const auto& p : todo) cursor.step(p);
            t.path = cursor.path();
            return t;
        }
        if (ec) return failed(next, ec.message());
        if (!todo.empty() && !fs::is_directory(status))
            return failed(next, std::make_error_code(std::errc::not_a_directory).message());
        cursor.step(part);
    }
    t.path = cursor.path();
    return t;
#endif
}

// The lock writers hold across processes while they read, change and
// replace the file: a file of its own beside it, since the settings file
// is replaced, never written in place. A writer waits for it until its
// deadline (another writer holds it only to read, write and rename one
// small file), then fails: a writer stopped while it holds the lock (in a
// debugger, say) costs a save, not a hang. Released on close, so also when
// a process dies holding it. A lock file that can't be opened, or is
// anything but a file, fails the save. Only where the filesystem has no
// locks (some network shares) do writers go ahead unlocked.
struct lock_taken
{
    std::intptr_t handle = -1;  // fd, or HANDLE; -1 when not held
    std::string error;          // why not, when it failed
};

lock_taken take_lock(const fs::path& lock_file, steady::time_point deadline)
{
    auto busy = [&]
    {
        return lock_taken{
            .error = rap::i18n::format(
                _("Another Tortilla is saving its settings and hasn't finished ({0} is held)."),
                shown(lock_file))};
    };
    auto failed = [&](const std::string& why)
    {
        return lock_taken{.error = rap::i18n::format(_("Couldn't lock the settings ({0}): {1}"),
                                                     shown(lock_file), why)};
    };
    std::chrono::milliseconds pause(1);
#ifdef _WIN32
    HANDLE h =
        CreateFileW(lock_file.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        const DWORD e = GetLastError();
        return failed(native_error(e));
    }
    if (GetFileType(h) != FILE_TYPE_DISK)
    {
        CloseHandle(h);
        return failed(native_error(ERROR_INVALID_FUNCTION));
    }
    for (;;)
    {
        OVERLAPPED at{};
        if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at))
            return {.handle = reinterpret_cast<std::intptr_t>(h)};
        const DWORD e = GetLastError();
        if (e == ERROR_NOT_SUPPORTED || e == ERROR_INVALID_FUNCTION)
        {
            CloseHandle(h);  // no locks here: unlocked
            return {};
        }
        if (e != ERROR_LOCK_VIOLATION && e != ERROR_IO_PENDING)
        {
            lock_taken out = failed(native_error(e));
            CloseHandle(h);
            return out;
        }
        if (steady::now() >= deadline)
        {
            CloseHandle(h);
            return busy();
        }
        std::this_thread::sleep_for(pause);
        pause = std::min(pause * 2, std::chrono::milliseconds(10));
    }
#else
    // O_NOFOLLOW: a link planted in the lock's place isn't followed.
    const int fd = ::open(lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
    {
        const int e = errno;
        return failed(crt_error(e));
    }
    for (;;)
    {
        if (!fail(settings_test_failure::no_locks) && ::flock(fd, LOCK_EX | LOCK_NB) == 0)
            return {.handle = fd};
        const int e = errno;
        if (e == EINTR) continue;
        if (e == ENOTSUP || e == EOPNOTSUPP || e == EINVAL)
        {
            ::close(fd);  // no locks here: unlocked
            return {};
        }
        if (e != EWOULDBLOCK)
        {
            lock_taken out = failed(crt_error(e));
            ::close(fd);
            return out;
        }
        if (steady::now() >= deadline)
        {
            ::close(fd);
            return busy();
        }
        std::this_thread::sleep_for(pause);
        pause = std::min(pause * 2, std::chrono::milliseconds(10));
    }
#endif
}

void release_lock(std::intptr_t handle)
{
    if (handle == -1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    ::close(int(handle));
#endif
}

class writer_lock
{
public:
    writer_lock(const fs::path& lock_file, steady::time_point deadline)
        : taken_(take_lock(lock_file, deadline))
    {
    }
    ~writer_lock() { release_lock(taken_.handle); }
    writer_lock(const writer_lock&) = delete;
    writer_lock& operator=(const writer_lock&) = delete;
    const std::string& error() const { return taken_.error; }

private:
    lock_taken taken_;
};

bool parse(std::string_view text, values& out)
{
    settings_key_reader format;
    // Whole lines, however long: every save writes every key back.
    for (size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view s = text.substr(at, end - at);
        at = end + 1;
        if (format.read_header(s)) continue;
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
        const size_t eq = s.find('=');
        if (eq == std::string_view::npos || s.starts_with('#')) continue;
        out[format.key(s.substr(0, eq))] = format.value(s.substr(eq + 1));
    }
    return format.version_two();
}

// The file as it is now: empty if there is none; nullopt if it is there but
// can't be read, so an update doesn't replace what it couldn't see.
std::optional<values> read_file(const fs::path& file, std::string* error = nullptr,
                                bool* version_two = nullptr, uint64_t* generation = nullptr,
                                steady::time_point deadline = steady::now() + std::chrono::seconds(5))
{
    if (generation) *generation = 0;
    if (version_two) *version_two = false;
    auto document = read_settings_document(file, deadline);
    if (!document)
    { if (error) *error = document.error(); return std::nullopt; }
    const std::string& text = *document;
    const auto parsed_generation = settings_document_generation(text);
    if (!parsed_generation)
    { if (error) *error = _("The settings generation is invalid."); return std::nullopt; }
    if (generation) *generation = *parsed_generation;
    values out;
    const bool marked = parse(text, out);
    if (version_two) *version_two = marked;
    return out;
}

struct settings_replacement
{
    bool existing = false;
    std::uint32_t attributes = 0;
#ifdef _WIN32
    FILE_ID_INFO identity{};
#endif
};

#ifdef _WIN32
struct settings_file_security
{
    settings_replacement replacement;
    std::string failure;
    PSECURITY_DESCRIPTOR source = nullptr;
    SECURITY_DESCRIPTOR descriptor{};
    std::vector<unsigned char> token_user, owner_acl;
    ~settings_file_security() { if (source) LocalFree(source); }

    bool verify(HANDLE handle, DWORD& error)
    {
        PACL expected_acl = nullptr, actual_acl = nullptr;
        PSID expected_owner = nullptr, actual_owner = nullptr;
        BOOL present = FALSE, defaulted = FALSE;
        if (!GetSecurityDescriptorOwner(&descriptor, &expected_owner, &defaulted) ||
            !GetSecurityDescriptorDacl(&descriptor, &present, &expected_acl, &defaulted))
        { error = GetLastError(); return false; }
        PSECURITY_DESCRIPTOR actual = nullptr;
        error = GetSecurityInfo(handle, SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &actual_owner, nullptr, &actual_acl, nullptr, &actual);
        if (error != ERROR_SUCCESS) return false;
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision = 0;
        bool same = GetSecurityDescriptorControl(actual, &control, &revision) != 0;
        if (!same) error = GetLastError();
        same = same && (control & SE_DACL_PROTECTED) && actual_owner && actual_acl &&
               IsValidAcl(actual_acl) && EqualSid(actual_owner, expected_owner) &&
               actual_acl->AceCount == expected_acl->AceCount;
        for (DWORD i = 0; same && i < expected_acl->AceCount; ++i)
        {
            void* expected_ace = nullptr;
            void* actual_ace = nullptr;
            same = GetAce(expected_acl, i, &expected_ace) && GetAce(actual_acl, i, &actual_ace);
            if (!same) { error = GetLastError(); break; }
            const auto size = static_cast<ACE_HEADER*>(expected_ace)->AceSize;
            same = size == static_cast<ACE_HEADER*>(actual_ace)->AceSize &&
                   std::memcmp(expected_ace, actual_ace, size) == 0;
        }
        LocalFree(actual);
        if (!same && error == ERROR_SUCCESS) error = ERROR_INVALID_SECURITY_DESCR;
        return same;
    }

    bool prepare(const fs::path& target, DWORD& error)
    {
        PACL dacl = nullptr;
        PSID owner = nullptr;
        settings_native_handle existing{CreateFileW(target.c_str(), READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
        const DWORD opened = existing.value == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (existing.value != INVALID_HANDLE_VALUE)
        {
            // https://learn.microsoft.com/windows/win32/api/aclapi/nf-aclapi-getsecurityinfo
            error = GetSecurityInfo(existing.value, SE_FILE_OBJECT,
                                     OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                     &owner, nullptr, &dacl, nullptr, &source);
            if (error != ERROR_SUCCESS) return false;
            if (!owner || !IsValidSid(owner))
            { error = ERROR_INVALID_SECURITY_DESCR; return false; }
            BY_HANDLE_FILE_INFORMATION information{};
            if (!GetFileInformationByHandle(existing.value, &information))
            { error = GetLastError(); return false; }
            if (fail(settings_test_failure::encrypted_target))
                information.dwFileAttributes |= FILE_ATTRIBUTE_ENCRYPTED;
            if (information.dwFileAttributes & FILE_ATTRIBUTE_ENCRYPTED)
            {
                error = ERROR_NOT_SUPPORTED;
                failure = _("Atomic settings saves are not supported for encrypted files.");
                return false;
            }
            replacement.existing = true;
            replacement.attributes = information.dwFileAttributes;
            if (!GetFileInformationByHandleEx(existing.value, FileIdInfo,
                                               &replacement.identity, sizeof replacement.identity))
            { error = GetLastError(); return false; }
            if (information.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
            { error = ERROR_ACCESS_DENIED; return false; }
            // An explicitly NULL DACL on the source grants everyone access. Do
            // not create a NULL-DACL temp: use a narrower current-user DACL.
        }
        else if (opened != ERROR_FILE_NOT_FOUND && opened != ERROR_PATH_NOT_FOUND)
        { error = opened; return false; }
        if (!dacl)
        {
            HANDLE raw = nullptr;
            if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw))
            {
                const DWORD native = GetLastError();
                if (native != ERROR_NO_TOKEN) { error = native; return false; }
                if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
                { error = GetLastError(); return false; }
            }
            settings_native_handle token{raw};
            DWORD size = 0;
            GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
            const DWORD native = GetLastError();
            if (native != ERROR_INSUFFICIENT_BUFFER) { error = native; return false; }
            token_user.resize(size);
            if (!GetTokenInformation(token.value, TokenUser, token_user.data(), size, &size))
            { error = GetLastError(); return false; }
            PSID user = reinterpret_cast<TOKEN_USER*>(token_user.data())->User.Sid;
            if (!owner) owner = user;
            owner_acl.resize(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(user));
            dacl = reinterpret_cast<PACL>(owner_acl.data());
            if (!InitializeAcl(dacl, DWORD(owner_acl.size()), ACL_REVISION) ||
                !AddAccessAllowedAceEx(dacl, ACL_REVISION, 0, FILE_ALL_ACCESS, user))
            { error = GetLastError(); return false; }
        }
        if (!owner || !IsValidSid(owner) || !IsValidAcl(dacl))
        { error = ERROR_INVALID_SECURITY_DESCR; return false; }
        // Protected explicit DACL prevents broader parent inheritance at the
        // instant CREATE_NEW makes the temp visible. Owner assignment can fail
        // if the caller cannot assign the existing owner; never silently drop it.
        if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorOwner(&descriptor, owner, FALSE) ||
            !SetSecurityDescriptorDacl(&descriptor, TRUE, dacl, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        { error = GetLastError(); return false; }
        return true;
    }
};
#endif

#ifdef _WIN32
bool sharing_error(DWORD e)
{
    return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED;
}
#endif

#ifdef _WIN32
bool same_file(const FILE_ID_INFO& a, const FILE_ID_INFO& b)
{
    return a.VolumeSerialNumber == b.VolumeSerialNumber &&
           std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof a.FileId.Identifier) == 0;
}

DWORD path_identity(const fs::path& path, FILE_ID_INFO& identity)
{
    settings_native_handle handle{CreateFileW(path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE) return GetLastError();
    if (!GetFileInformationByHandleEx(handle.value, FileIdInfo, &identity, sizeof identity))
        return GetLastError();
    return ERROR_SUCCESS;
}

// Delete only the opened, identity-checked object, never an unrelated name that
// appeared at the same path. Both a reserved empty placeholder and old source
// are ours; every other identity is retained and reported.
DWORD remove_backup(const fs::path& backup, const FILE_ID_INFO& placeholder,
                    const FILE_ID_INFO& original)
{
    if (fail(settings_test_failure::backup_cleanup)) return ERROR_ACCESS_DENIED;
    settings_native_handle handle{CreateFileW(backup.c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE)
    {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : e;
    }
    FILE_ID_INFO identity{};
    if (!GetFileInformationByHandleEx(handle.value, FileIdInfo, &identity, sizeof identity)) return GetLastError();
    if (!same_file(identity, placeholder) && !same_file(identity, original)) return ERROR_FILE_INVALID;
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(handle.value, FileDispositionInfo, &disposition, sizeof disposition))
        return GetLastError();
    return ERROR_SUCCESS;
}

// This boundary performs real namespace changes in fault tests, rather than
// returning an error before the operation under test has changed any names.
DWORD native_replace(const fs::path& to, const fs::path& from, const fs::path& backup)
{
    if (fail(settings_test_failure::backup_retry))
    {
        if (!DeleteFileW(backup.c_str())) return GetLastError();
        return ERROR_SHARING_VIOLATION;
    }
    if (fail(settings_test_failure::partial_replace_retained))
        return ERROR_UNABLE_TO_MOVE_REPLACEMENT; // documented 1176 with backup: target retained
    const bool defensive1176 = fail(settings_test_failure::partial_replace);
    const bool documented1177 = fail(settings_test_failure::partial_replace_2);
    if (defensive1176 || documented1177)
    {
        if (!MoveFileExW(to.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return GetLastError();
        return defensive1176 ? ERROR_UNABLE_TO_MOVE_REPLACEMENT : ERROR_UNABLE_TO_MOVE_REPLACEMENT_2;
    }
    if (ReplaceFileW(to.c_str(), from.c_str(), backup.c_str(), 0, nullptr, nullptr)) return ERROR_SUCCESS;
    return GetLastError();
}
#endif

settings::saved replace(const fs::path& from, const fs::path& to, const fs::path& configured,
                        bool& retain_temp, const settings_replacement& replacement, steady::time_point deadline)
{
    if (fail(settings_test_failure::replace)) return crt_error(errno);
#ifdef _WIN32
    fs::path backup;
    FILE_ID_INFO placeholder{};
    settings_file_security security;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &security.descriptor, FALSE};
    auto reserve = [&](const fs::path& name) -> DWORD
    {
        settings_native_handle reserved{CreateFileW(name.c_str(), GENERIC_WRITE | READ_CONTROL, 0,
            &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (reserved.value == INVALID_HANDLE_VALUE) return GetLastError();
        DWORD e = ERROR_SUCCESS;
        if (!security.verify(reserved.value, e) ||
            !GetFileInformationByHandleEx(reserved.value, FileIdInfo, &placeholder, sizeof placeholder))
        {
            if (e == ERROR_SUCCESS) e = GetLastError();
            CloseHandle(reserved.value);
            reserved.value = INVALID_HANDLE_VALUE;
            DeleteFileW(name.c_str()); // an unverifiable EMPTY owned placeholder
            return e;
        }
        return ERROR_SUCCESS;
    };
    if (replacement.existing)
    {
        DWORD e = ERROR_SUCCESS;
        if (!security.prepare(to, e)) return security.failure.empty() ? native_error(e) : security.failure;
        if (!security.replacement.existing || !same_file(security.replacement.identity, replacement.identity))
            return native_error(ERROR_FILE_INVALID);
        for (const auto& name : settings_backup_files(to))
        {
            e = reserve(name);
            if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) continue;
            if (e != ERROR_SUCCESS) return native_error(e);
            backup = name;
            break;
        }
        if (backup.empty()) return {_("Every settings backup name is already taken.")};
    }
    std::optional<settings_recovery_record> receipt;
    auto publish_intent = [&]() -> std::string {
        auto made = begin_settings_recovery(to, configured, backup, from);
        if (!made) return made.error();
        if (made->old_file.volume != replacement.identity.VolumeSerialNumber ||
            std::memcmp(made->old_file.id.data(), replacement.identity.FileId.Identifier, made->old_file.id.size()))
        { finish_settings_recovery(*made); return "settings identity changed before recovery publication"; }
        receipt = std::move(*made);
        return {};
    };
    if (replacement.existing)
        if (auto error = publish_intent(); !error.empty())
        {
            remove_backup(backup, placeholder, replacement.identity);
            return error;
        }
    for (int attempt = 0;; ++attempt)
    {
        DWORD e = ERROR_SUCCESS;
        if (replacement.existing)
        {
            // A failed native attempt may remove the placeholder. Re-reserve
            // exclusively, or refuse a foreign identity before the next API call.
            FILE_ID_INFO reserved{};
            DWORD state = path_identity(backup, reserved);
            if (state == ERROR_FILE_NOT_FOUND || state == ERROR_PATH_NOT_FOUND)
            {
                if (receipt)
                    if (auto error = finish_settings_recovery(*receipt); !error.empty()) return error;
                receipt.reset();
                state = reserve(backup);
                if (state == ERROR_SUCCESS)
                    if (auto error = publish_intent(); !error.empty()) return error;
            }
            else if (state == ERROR_SUCCESS && !same_file(reserved, placeholder) &&
                     !same_file(reserved, replacement.identity)) state = ERROR_FILE_INVALID;
            if (state != ERROR_SUCCESS)
                return rap::i18n::format(_("Couldn't reserve the settings backup {0}: {1}"),
                                         shown(backup), native_error(state));
            constexpr DWORD carried = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_TEMPORARY;
            const DWORD flags = replacement.attributes & carried;
            if (!SetFileAttributesW(from.c_str(), flags ? flags : FILE_ATTRIBUTE_NORMAL)) e = GetLastError();
            else e = native_replace(to, from, backup); // no ignore-ACL/merge flags
        }
        else if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) e = GetLastError();
        if (e == ERROR_SUCCESS)
        {
            if (!backup.empty())
            {
                const DWORD cleanup = remove_backup(backup, placeholder, replacement.identity);
                if (cleanup != ERROR_SUCCESS)
                    return settings::saved::cleanup_warning(rap::i18n::format(
                        _("Settings saved to {0}, but backup retained at {1}: {2}"),
                        shown(to), shown(backup), native_error(cleanup)));
            }
            if (receipt)
                if (auto error = finish_settings_recovery(*receipt); !error.empty())
                    return settings::saved::cleanup_warning("Settings saved; " + error);
            return {};
        }
        if (e == ERROR_UNABLE_TO_MOVE_REPLACEMENT || e == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2)
        {
            retain_temp = true;
            const auto recovered = recover_settings_before_save(configured, to, deadline);
            if (!recovered.error.empty())
                return rap::i18n::format(_("Couldn't save settings to {0}: {1}. {2}; attempted data retained at {3}."),
                    shown(to), native_error(e), recovered.error, shown(from));
            return rap::i18n::format(
                _("Couldn't save settings to {0}: {1}. Last-good settings restored or retained; attempted data retained at {2}."),
                shown(to), native_error(e), shown(from));
        }
        if (attempt == 200 || !sharing_error(e) || steady::now() >= deadline)
        {
            std::string why = native_error(e);
            if (!backup.empty())
            {
                const DWORD cleanup = remove_backup(backup, placeholder, replacement.identity);
                if (cleanup != ERROR_SUCCESS)
                    why += rap::i18n::format(_("; backup retained at {0}: {1}"), shown(backup), native_error(cleanup));
                else if (receipt)
                    if (auto error = finish_settings_recovery(*receipt); !error.empty()) why += "; " + error;
            }
            return why;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#else
    if (std::rename(from.c_str(), to.c_str()) == 0) return {};
    const int e = errno;
    return crt_error(e);
#endif
}

// A temporary file beside `target`, made anew (never one already there,
// whatever it is: a leftover, a link, a folder), open for writing, private
// until it takes the mode of the file it replaces.
std::FILE* create_temp(const fs::path& target, fs::path& made, std::string& error,
                       settings_replacement& replacement)
{
#ifdef _WIN32
    settings_file_security security;
    DWORD native = ERROR_SUCCESS;
    if (!security.prepare(target, native))
    { error = security.failure.empty() ? native_error(native) : security.failure; return nullptr; }
    replacement = security.replacement;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &security.descriptor, FALSE};
#endif
    for (const fs::path& name : settings_temp_files(target))
    {
#ifdef _WIN32
        HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE | READ_CONTROL, 0, &attributes, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            const DWORD e = GetLastError();
            std::error_code ec;
            if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS ||
                fs::symlink_status(name, ec).type() != fs::file_type::not_found)
                continue;  // someone else's
            error = native_error(e);
            return nullptr;
        }
        const bool secure = security.verify(h, native);
        const bool unprotected = fail(settings_test_failure::unprotected_temp);
        if (!secure || unprotected)
        {
            error = _("The filesystem did not enforce the required private settings-file permissions.");
            if (!secure) error += ": " + native_error(native);
            CloseHandle(h);
            DeleteFileW(name.c_str());  // still empty: no secret bytes were written
            return nullptr;
        }
        if (temp_created_for_tests) temp_created_for_tests(reinterpret_cast<std::intptr_t>(h));
        const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_BINARY);
        if (fd < 0)
        {
            const int e = errno;
            error = crt_error(e);
            CloseHandle(h);
            DeleteFileW(name.c_str());
            return nullptr;
        }
        std::FILE* f = _fdopen(fd, "wb");
        if (!f)
        {
            const int e = errno;
            error = crt_error(e);
            _close(fd);
            DeleteFileW(name.c_str());
            return nullptr;
        }
#else
        const int fd =
            ::open(name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0)
        {
            const int e = errno;
            if (e == EEXIST) continue;  // someone else's
            error = crt_error(e);
            return nullptr;
        }
        // Kept as private as the file it replaces.
        if (struct stat st;
            ::stat(target.c_str(), &st) == 0 && ::fchmod(fd, st.st_mode & 07777) != 0)
        {
            const int e = errno;
            error = crt_error(e);
            ::close(fd);
            ::unlink(name.c_str());
            return nullptr;
        }
        std::FILE* f = ::fdopen(fd, "wb");
        if (!f)
        {
            const int e = errno;
            error = crt_error(e);
            ::close(fd);
            ::unlink(name.c_str());
            return nullptr;
        }
#endif
        made = name;
        return f;
    }
    error = _("every name for it is taken");
    return nullptr;
}

// Writes `v` to a file beside `target` and renames it over `target`. On any
// ordinary failure preserves `target`; Windows partial replacements roll back
// their last-good backup under the writer lock. A failed rollback retains all
// recovery data. A successful publish can still report backup-cleanup trouble.
settings::saved write_file(const fs::path& target, const fs::path& configured, const values& v,
                           uint64_t generation, steady::time_point deadline)
{
    fs::path temp;
    std::string why;
    settings_replacement replacement;
    std::FILE* f = create_temp(target, temp, why, replacement);
    if (!f)
        return rap::i18n::format(_("Couldn't make a file beside {0} to save the settings to: {1}"),
                                 shown(target), why);
    auto checked = [&](bool ok)
    {
        const int e = errno;  // CRT error, before formatting or cleanup changes it
        if (!ok && why.empty()) why = crt_error(e ? e : EIO);
        return ok;
    };
    bool ok = checked(std::fwrite(k_settings_format_marker.data(), 1,
                                  k_settings_format_marker.size(), f) == k_settings_format_marker.size()) &&
              checked(std::fputc('\n', f) != EOF);
    const std::string generation_line = settings_generation_line(generation);
    ok = ok && checked(std::fwrite(generation_line.data(), 1, generation_line.size(), f) == generation_line.size());
    for (auto& [k, val] : v)
    {
        const std::string encoded = encode_settings_key(k);
        const std::string value = encode_settings_value(val);
        ok = ok && checked(std::fwrite(encoded.data(), 1, encoded.size(), f) == encoded.size()) &&
             checked(std::fputc('=', f) != EOF) &&
             checked(std::fwrite(value.data(), 1, value.size(), f) == value.size()) &&
             checked(std::fputc('\n', f) != EOF);
    }
    ok = ok && checked(!fail(settings_test_failure::flush) && std::fflush(f) == 0) &&
         checked(!std::ferror(f));
    // On disk before the rename, so a crash leaves the old file or the new
    // one, not an empty one.
#ifdef _WIN32
    ok = ok && checked(_commit(_fileno(f)) == 0);
#else
    ok = ok && checked(::fsync(::fileno(f)) == 0);
#endif
    // Always close, including after an earlier failure. A close can be the
    // first operation to report buffered I/O failure and must supply its reason.
    const int closed = std::fclose(f);
    const int close_error = errno;
    const bool injected_close = fail(settings_test_failure::close);
    if (closed != 0 || injected_close)
    {
        if (why.empty()) why = crt_error(injected_close ? EIO : close_error);
        ok = false;
    }
    bool retain_temp = false;
    settings::saved result;
    if (ok) result = replace(temp, target, configured, retain_temp, replacement, deadline);
    ok = ok && !result.failed();
    if (result.failed()) why = result.message;
    if (!ok)
    {
        std::error_code ec;
        if (!retain_temp) fs::remove(temp, ec);  // never delete the last recoverable replacement
        return rap::i18n::format(_("Couldn't save the settings to {0}: {1}"), shown(target), why);
    }
    if (fail(settings_test_failure::cleanup_warning))
        return settings::saved::cleanup_warning(_("Settings saved; injected cleanup warning."));
    return result;
}

std::string numbers_value(const std::vector<float>& v)
{
    std::string s;
    for (float f : v) s += (s.empty() ? "" : ",") + std::to_string(int(std::lround(f)));
    return s;
}

}  // namespace

void settings_fail_next(settings_test_failure failure) { next_failure = failure; }
#ifdef _WIN32
void settings_final_path_for_tests(const fs::path& candidate, bool opened_only)
{ final_candidate_for_tests = candidate; opened_path_only_for_tests = opened_only; }
void settings_temp_created_for_tests(std::function<void(std::intptr_t)> observer)
{ temp_created_for_tests = std::move(observer); }
void settings_before_lock_for_tests(std::function<void()> action)
{ before_lock_for_tests = std::move(action); }
#endif

#ifndef _WIN32
settings_path_parts split_settings_path(const fs::path& file)
{
    settings_path_parts out;
    out.root = file.root_path();
    for (const auto& part : file.relative_path()) out.components.push_back(part);
    return out;
}
#endif

std::vector<fs::path> settings_temp_files(const fs::path& file)
{
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    std::vector<fs::path> out;
    for (int n = 0; n < 16; ++n)
        out.push_back(
            with_suffix(file, "." + std::to_string(pid) + "." + std::to_string(n) + ".tmp"));
    return out;
}

#ifdef _WIN32
std::vector<fs::path> settings_backup_files(const fs::path& file)
{
    auto out = settings_temp_files(file);
    for (auto& name : out) name.replace_extension(".bak");
    return out;
}
#endif

fs::path settings_lock_file(const fs::path& file)
{
    std::string error;
    return with_suffix(resolve(file, error).value_or(settings_target{file}).path, ".lock");
}

settings_lock_holder::settings_lock_holder(const fs::path& file)
{
    lock_taken t = take_lock(settings_lock_file(file), steady::now());
    handle_ = t.handle;
    held_ = t.handle != -1;
}

settings_lock_holder::~settings_lock_holder()
{
    release_lock(handle_);
}

rap::color_scheme appearance_from(std::string_view stored)
{
    return stored == "light"  ? rap::color_scheme::light
           : stored == "dark" ? rap::color_scheme::dark
                              : rap::color_scheme::system;
}

std::string_view appearance_value(rap::color_scheme s)
{
    return s == rap::color_scheme::light ? "light" : s == rap::color_scheme::dark ? "dark" : "";
}

rap::color_scheme appearance()
{
    return appearance_from(settings::get().value(k_appearance_setting));
}

void settings_changes::set(const std::string& key, const std::string& value)
{
    changes_.push_back([key, value](values& v) { v[key] = value; });
}

void settings_changes::remove(const std::string& key)
{
    changes_.push_back([key](values& v) { v.erase(key); });
}

void settings_changes::set_numbers(const std::string& key, const std::vector<float>& v)
{
    set(key, numbers_value(v));
}

void settings_changes::add_to_history(const std::string& key, const std::string& entry, size_t keep)
{
    // Added to the history as the file has it, so entries another process
    // added meanwhile stay.
    changes_.push_back(
        [key, entry, keep](values& v)
        {
            auto it = v.find(key);
            v[key] = history_with(it == v.end() ? std::string_view{} : it->second, entry, keep);
        });
}

void settings_changes::save_window(const std::string& prefix, const rap::window& w)
{
    const rap::size_f size = w.size();
    set_numbers(prefix + ".size", {size.w, size.h});
    if (auto pos = w.position()) set_numbers(prefix + ".position", {pos->x, pos->y});
}

settings& settings::get()
{
    static settings s;
    return s;
}

settings::settings()
    : file_(settings_file())
{
    std::string error;
    values_ = read_file(file_, &error).value_or(values{});
    if (!error.empty()) remember_save_report({error});
}

void settings::use_file(const fs::path& file)
{
    std::scoped_lock write(write_mutex_);
    std::string error;
    values v = read_file(file, &error).value_or(values{});
    {
        std::unique_lock lock(mutex_);
        file_ = file;
        values_ = std::move(v);
    }
    if (!error.empty()) remember_save_report({error});
}

settings::saved settings::apply(const settings_changes& changes)
{
    return apply_for(changes, lock_wait_.load());
}

settings::saved settings::save_on_close(const settings_changes& changes)
{
    const saved done = apply_for(changes, std::min(lock_wait_.load(), std::chrono::milliseconds(1000)));
    remember_save_report(done);
    return done;
}

settings::saved settings::apply_for(const settings_changes& changes, std::chrono::milliseconds wait)
{
    if (changes.empty()) return {};
    ++transactions_;
    // One wait for both locks: this process's other writers and others'.
    const steady::time_point deadline = steady::now() + wait;
    std::unique_lock write(write_mutex_, deadline);
    fs::path file;
    {
        std::shared_lock lock(mutex_);
        file = file_;
    }
    if (!write.owns_lock())
        return {rap::i18n::format(
            _("Another save of the settings in this Tortilla hasn't finished ({0})."),
            shown(file))};
    bool retried = false;
    for (;;)
    {
        if (retried && steady::now() >= deadline)
            return {rap::i18n::format(_("The settings file kept changing until the save deadline ({0})."), shown(file))};
        std::string error;
        bool changed = false;
        const auto initial_recovery = inspect_settings_recovery(file, {}, deadline);
        if (initial_recovery.phase == settings_recovery_phase::blocked) return {initial_recovery.message};
        const auto target_name = initial_recovery.target;
        const std::optional<settings_target> target = resolve(target_name, error, &changed);
        if (!target)
        {
#ifdef _WIN32
            if (changed) { retried = true; std::this_thread::yield(); continue; }
#endif
            return {error};
        }
#ifdef _WIN32
        auto before_lock = std::move(before_lock_for_tests);
        before_lock_for_tests = {};
        if (before_lock) before_lock();
#endif
        std::error_code ec;
        const fs::path folder = target->path.parent_path();
        if (!folder.empty() && !fs::is_directory(folder, ec))
        {
            // The config folder on a first save; never a folder a link leads to.
            if (target->linked)
                return {rap::i18n::format(_("{0} is a link to {1}, in a folder that isn't there."),
                                          shown(file), shown(target->path))};
            fs::create_directories(folder, ec);
        }
        writer_lock across_processes(with_suffix(target->path, ".lock"), deadline);
        if (!across_processes.error().empty()) return {across_processes.error()};
#ifdef _WIN32
        // A cooperating writer may have replaced the file after the first native
        // open. Reopen/revalidate under the acquired lock, then retry the WHOLE
        // resolution with that lock released and the original deadline unchanged.
        const auto current = resolve(target_name, error, &changed);
        if (!current && !changed) return {error};
        if (!current || current->path != target->path || current->existing != target->existing ||
            current->identity.VolumeSerialNumber != target->identity.VolumeSerialNumber ||
            std::memcmp(current->identity.FileId.Identifier, target->identity.FileId.Identifier,
                        sizeof target->identity.FileId.Identifier) != 0)
        { retried = true; continue; }
        // A live configured alias must still lead to the target whose lock we
        // hold. A dangling alias may recover only through its verified receipt.
        const auto alias_current = resolve(file, error, &changed);
        if (alias_current && alias_current->path != target->path)
        { retried = true; continue; }
        if (!alias_current && initial_recovery.records.empty())
        { if (changed) { retried = true; continue; } return {error}; }
#endif
        const auto recovered = recover_settings_before_save(file, target->path, deadline);
        if (!recovered.error.empty()) return {recovered.error};
        if (recovered.restored) { retried = true; continue; }
        std::string read_error;
        bool version_two = false;
        uint64_t generation = 0;
        std::optional<values> now = read_file(target->path, &read_error, &version_two, &generation, deadline);
        if (!now)
            return {rap::i18n::format(_("Couldn't read the settings in {0}: {1}"), shown(target->path),
                                      read_error)};
        values next = *now;
        for (auto& change : changes.changes_) change(next);
        // An explicit save upgrades a legacy file even if the selected value did
        // not change. Merely loading the file never rewrites it.
        saved result;
        if (next != *now || !version_two)
        {
            if (generation == UINT64_MAX) return {_("The settings generation cannot be incremented.")};
            result = write_file(target->path, file, next, generation + 1, deadline);
        }
        std::unique_lock lock(mutex_);
        // A native replacement can publish successfully but fail backup cleanup.
        // On any error, mirror the actual target, falling back to the last read
        // snapshot only when it cannot be reread (e.g. unrecoverable disk failure).
        values_ = result.committed() ? std::move(next) : read_file(target->path,nullptr,nullptr,nullptr,deadline).value_or(std::move(*now));
        return result;
    }
}

std::string settings::value(const std::string& key, const std::string& fallback) const
{
    std::shared_lock lock(mutex_);
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

bool settings::contains(const std::string& key) const
{
    std::shared_lock lock(mutex_);
    return values_.contains(key);
}

void settings::remember_save_report(const saved& done)
{
    if (done.message.empty()) return;
    {
        std::scoped_lock lock(error_mutex_);
        const bool known = std::any_of(save_reports_.begin(), save_reports_.end(), [&](const saved& old) {
            return old.outcome == done.outcome && old.message == done.message;
        });
        if (!known) save_reports_.push_back(done);
    }
    const std::string prefix = done.failed() ? "settings were not saved: " : "settings saved with cleanup warning: ";
    std::fprintf(stderr, "tortilla: %s%s\n", prefix.c_str(), done.message.c_str());
#ifdef _WIN32
    const std::string diagnostic = "Tortilla: " + prefix + done.message + "\n";
    OutputDebugStringA(diagnostic.c_str());
#endif
}

settings::saved settings::take_save_report()
{
    std::scoped_lock lock(error_mutex_);
    auto reports = std::exchange(save_reports_, {});
    if (reports.empty()) return {};
    if (reports.size() == 1) return std::move(reports.front());
    saved out = saved::cleanup_warning({});
    for (const auto& report : reports)
    {
        if (report.failed()) out.outcome = save_outcome::failed;
        if (!out.message.empty()) out.message += "\n";
        out.message += report.failed() ? _("Not saved: ") : _("Saved; cleanup warning: ");
        out.message += report.message;
    }
    return out;
}

void settings::set(const std::string& key, const std::string& value)
{
    settings_changes c;
    c.set(key, value);
    remember_save_report(apply(c));
}

void settings::remove(const std::string& key)
{
    settings_changes c;
    c.remove(key);
    remember_save_report(apply(c));
}

std::vector<std::string> history_entries(std::string_view stored)
{
    std::vector<std::string> out;
    for (size_t at = 0; at < stored.size();)
    {
        size_t end = stored.find('\t', at);
        if (end == std::string_view::npos) end = stored.size();
        if (end > at) out.emplace_back(stored.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

std::string history_with(std::string_view stored, const std::string& entry, size_t keep)
{
    std::vector<std::string> entries = history_entries(stored);
    std::erase(entries, entry);
    entries.insert(entries.begin(), entry);
    if (entries.size() > keep) entries.resize(keep);
    std::string out;
    for (const std::string& e : entries) out += (out.empty() ? "" : "\t") + e;
    return out;
}

std::vector<std::string> settings::history(const std::string& key) const
{
    return history_entries(value(key));
}

void settings::add_to_history(const std::string& key, const std::string& entry, size_t keep)
{
    settings_changes c;
    c.add_to_history(key, entry, keep);
    remember_save_report(apply(c));
}

std::vector<float> settings::numbers(const std::string& key) const
{
    const std::string s = value(key);
    std::vector<float> out;
    for (size_t at = 0; at < s.size();)
    {
        size_t comma = s.find(',', at);
        if (comma == std::string::npos) comma = s.size();
        out.push_back(std::strtof(s.substr(at, comma - at).c_str(), nullptr));
        at = comma + 1;
    }
    return out;
}

void settings::set_numbers(const std::string& key, const std::vector<float>& v)
{
    settings_changes c;
    c.set_numbers(key, v);
    remember_save_report(apply(c));
}

void settings::save_window(const std::string& prefix, const rap::window& w)
{
    settings_changes c;
    c.save_window(prefix, w);
    save_on_close(c);
}

void settings::restore_window(const std::string& prefix, rap::window& w) const
{
    if (auto v = numbers(prefix + ".size"); v.size() == 2 && v[0] > 0 && v[1] > 0)
        w.resize({v[0], v[1]});
    if (auto v = numbers(prefix + ".position"); v.size() == 2) w.move({v[0], v[1]});
}

}  // namespace tortilla
