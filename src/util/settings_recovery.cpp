#include "settings_recovery.h"
#include "settings_format.h"
#include "sha256.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#endif

namespace tortilla
{
namespace
{
namespace fs = std::filesystem;
constexpr std::string_view magic = "#tortilla-settings-recovery 1\n";
constexpr size_t receipt_limit = 16384, document_limit = 64 * 1024 * 1024, record_limit = 128;
thread_local bool fail_restore = false;
thread_local std::function<void()> after_inspect_for_tests;
thread_local std::function<void(const fs::path&, const fs::path&, bool)> before_publish_for_tests;
thread_local std::function<void(const fs::path&)> after_enumerate_for_tests;
thread_local bool transient_discovery = false;
std::string utf8(const fs::path& p) { auto s = p.u8string(); return {s.begin(), s.end()}; }
std::string pending(const fs::path& p, const std::string& why)
{ return "Settings recovery pending at " + utf8(p) + ": " + why; }
std::string hex(const unsigned char* data, size_t n)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) { out += digits[data[i] >> 4]; out += digits[data[i] & 15]; }
    return out;
}
bool unhex(std::string_view s, unsigned char* out, size_t n)
{
    if (s.size() != 2 * n) return false;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < n; ++i)
    {
        int a = digit(s[2*i]), b = digit(s[2*i+1]);
        if (a < 0 || b < 0) return false;
        out[i] = static_cast<unsigned char>(16 * a + b);
    }
    return true;
}
bool number(std::string_view text, uint64_t& out)
{
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
bool basename(const fs::path& p)
{ return !p.empty() && p != "." && p != ".." && p == p.filename() && !p.has_root_path() &&
         utf8(p).find_first_of("/\\:") == std::string::npos; }
bool same_id(const settings_recovery_snapshot& a, const settings_recovery_snapshot& b)
{ return a.volume == b.volume && a.id == b.id; }
bool same_document(const settings_recovery_snapshot& a, const settings_recovery_snapshot& b)
{ return same_id(a,b) && a.generation == b.generation && a.length == b.length && a.hash == b.hash; }
#ifdef _WIN32
struct handle
{
    HANDLE h = INVALID_HANDLE_VALUE;
    ~handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
struct local_memory
{
    void* p = nullptr;
    ~local_memory() { if (p) LocalFree(p); }
};
std::string native(DWORD error) { return std::system_category().message(int(error)); }
std::expected<std::vector<unsigned char>, std::string> current_user()
{
    HANDLE raw = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw))
    {
        DWORD e = GetLastError();
        if (e != ERROR_NO_TOKEN) return std::unexpected(native(e));
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return std::unexpected(native(GetLastError()));
    }
    handle token{raw};
    DWORD n = 0;
    GetTokenInformation(token.h, TokenUser, nullptr, 0, &n);
    DWORD e = GetLastError();
    if (e != ERROR_INSUFFICIENT_BUFFER) return std::unexpected(native(e));
    std::vector<unsigned char> data(n);
    if (!GetTokenInformation(token.h, TokenUser, data.data(), n, &n)) return std::unexpected(native(GetLastError()));
    auto sid = reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid;
    if (!IsValidSid(sid)) return std::unexpected("invalid token user");
    std::vector<unsigned char> result(GetLengthSid(sid));
    if (!CopySid(DWORD(result.size()), result.data(), sid)) return std::unexpected(native(GetLastError()));
    return result;
}
bool private_receipt(HANDLE h)
{
    auto user = current_user();
    if (!user) return false;
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR raw = nullptr;
    DWORD e = GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              &owner, nullptr, &acl, nullptr, &raw);
    local_memory memory{raw};
    if (e != ERROR_SUCCESS || !owner || !acl || !EqualSid(owner, user->data()) || !IsValidAcl(acl)) return false;
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(raw, &control, &revision) || !(control & SE_DACL_PROTECTED) || acl->AceCount != 1)
        return false;
    void* p = nullptr;
    if (!GetAce(acl, 0, &p)) return false;
    const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(p);
    return ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceFlags == 0 &&
           EqualSid(const_cast<DWORD*>(&ace->SidStart), user->data()) && (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS;
}
std::expected<std::string, std::string> read_bytes(const fs::path& p, size_t limit, bool receipt,
                                                 settings_recovery_snapshot* snapshot = nullptr)
{
    handle file{CreateFileW(p.c_str(), GENERIC_READ | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | ((receipt || snapshot) ? FILE_FLAG_OPEN_REPARSE_POINT : 0), nullptr)};
    if (file.h == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
            error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            transient_discovery = true;
        return std::unexpected(native(error));
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(file.h, FileAttributeTagInfo, &attributes, sizeof attributes))
        return std::unexpected(native(GetLastError()));
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        ((receipt || snapshot) && (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)))
        return std::unexpected("reparse/directory recovery artifact refused");
    if (receipt && !private_receipt(file.h)) return std::unexpected("receipt owner/ACL is not private to this user");
    FILE_ID_INFO before{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandleEx(file.h, FileIdInfo, &before, sizeof before) || !GetFileSizeEx(file.h, &size))
        return std::unexpected(native(GetLastError()));
    if (size.QuadPart < 0 || uint64_t(size.QuadPart) > limit) return std::unexpected("recovery artifact exceeds bounded size");
    std::string bytes(size_t(size.QuadPart), '\0');
    size_t at = 0;
    while (at < bytes.size())
    {
        DWORD count = 0;
        if (!ReadFile(file.h, bytes.data() + at, DWORD(std::min<size_t>(65536, bytes.size()-at)), &count, nullptr))
            return std::unexpected(native(GetLastError()));
        if (!count) return std::unexpected("incomplete recovery artifact");
        at += count;
    }
    FILE_ID_INFO after{};
    LARGE_INTEGER after_size{};
    if (!GetFileInformationByHandleEx(file.h, FileIdInfo, &after, sizeof after) || !GetFileSizeEx(file.h, &after_size))
        return std::unexpected(native(GetLastError()));
    if (before.VolumeSerialNumber != after.VolumeSerialNumber ||
        std::memcmp(&before.FileId, &after.FileId, sizeof before.FileId) || size.QuadPart != after_size.QuadPart)
        return std::unexpected("recovery artifact changed while reading");
    if (snapshot)
    {
        auto generation = settings_document_generation(bytes);
        if (!generation) return std::unexpected("invalid settings generation");
        snapshot->volume = before.VolumeSerialNumber;
        std::memcpy(snapshot->id.data(), before.FileId.Identifier, snapshot->id.size());
        snapshot->generation = *generation;
        snapshot->generation_present = settings_document_has_generation(bytes);
        snapshot->length = bytes.size();
        snapshot->hash = sha256_hex(bytes);
    }
    return bytes;
}
std::expected<settings_recovery_snapshot, std::string> snapshot(const fs::path& p, std::string* contents = nullptr)
{
    settings_recovery_snapshot out;
    auto bytes = read_bytes(p, document_limit, false, &out);
    if (!bytes) return std::unexpected(bytes.error());
    if (contents) *contents = std::move(*bytes);
    return out;
}
// Canonicalize the NAME, not the final reparse target. This also works for a
// dangling alias and never parses/substitutes a Windows symlink target.
std::expected<fs::path, std::string> alias_name(const fs::path& file)
{
    handle h{CreateFileW(file.c_str(), 0, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    bool parent = false;
    if (h.h == INVALID_HANDLE_VALUE)
    {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) return std::unexpected(native(e));
        auto folder = file.parent_path();
        if (folder.empty()) folder = ".";
        h.h = CreateFileW(folder.c_str(), 0, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h.h == INVALID_HANDLE_VALUE) return std::unexpected(native(GetLastError()));
        parent = true;
    }
    for (DWORD volume : {DWORD(VOLUME_NAME_DOS), DWORD(VOLUME_NAME_GUID), DWORD(VOLUME_NAME_NT)})
    {
        std::vector<wchar_t> data(512);
        for (int retry=0; retry<4; ++retry)
        {
            DWORD n = GetFinalPathNameByHandleW(h.h, data.data(), DWORD(data.size()), FILE_NAME_OPENED|volume);
            if (!n) break;
            if (n >= data.size()) { if(n>32768) break; data.resize(n); continue; }
            std::wstring name(data.data(),n);
            if(volume==VOLUME_NAME_NT) name=L"\\\\?\\GLOBALROOT"+name;
            fs::path out(name);
            if(parent) out/=file.filename();
            return out;
        }
    }
    return std::unexpected("could not name configured settings path");
}
bool same_path(const fs::path& a,const fs::path& b)
{
    if(_wcsicmp(a.c_str(),b.c_str())==0)return true;
    if(_wcsicmp(a.filename().c_str(),b.filename().c_str())!=0)return false;
    handle left{CreateFileW(a.parent_path().c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr)};
    handle right{CreateFileW(b.parent_path().c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr)};
    if(left.h==INVALID_HANDLE_VALUE||right.h==INVALID_HANDLE_VALUE)return false;
    FILE_ID_INFO x{},y{};
    return GetFileInformationByHandleEx(left.h,FileIdInfo,&x,sizeof x)&&
           GetFileInformationByHandleEx(right.h,FileIdInfo,&y,sizeof y)&&
           x.VolumeSerialNumber==y.VolumeSerialNumber&&!std::memcmp(&x.FileId,&y.FileId,sizeof x.FileId);
}
std::expected<std::vector<fs::path>,std::string> receipt_paths(const fs::path& name)
{
    const auto pattern=fs::path(name.native()+L".recovery.*.receipt");
    WIN32_FIND_DATAW data{};
    HANDLE search=FindFirstFileW(pattern.c_str(),&data);
    if(search==INVALID_HANDLE_VALUE)
    {
        DWORD e=GetLastError();
        if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND) return std::vector<fs::path>{};
        return std::unexpected(native(e));
    }
    std::vector<fs::path> out;
    do
    {
        if(out.size()==record_limit) { FindClose(search); return std::unexpected("too many pending receipts"); }
        out.push_back(name.parent_path()/data.cFileName);
    } while(FindNextFileW(search,&data));
    DWORD e=GetLastError(); FindClose(search);
    if(e!=ERROR_NO_MORE_FILES) return std::unexpected(native(e));
    return out;
}
std::string serialize(const settings_recovery_record& r)
{
    std::string text(magic);
    auto field=[&](std::string_view key,std::string value){text+=std::string(key)+"="+encode_settings_value(value)+"\n";};
    field("transaction",r.transaction); field("target",utf8(r.target)); field("alias",utf8(r.alias));
    field("backup",utf8(r.backup.filename())); field("replacement",utf8(r.replacement.filename()));
    auto stamp=[&](std::string_view prefix,const settings_recovery_snapshot& s){
        field(std::string(prefix)+"volume",std::to_string(s.volume));
        field(std::string(prefix)+"id",hex(s.id.data(),s.id.size()));
        field(std::string(prefix)+"generation",std::to_string(s.generation));
        field(std::string(prefix)+"length",std::to_string(s.length)); field(std::string(prefix)+"hash",s.hash);
    };
    stamp("old.",r.old_file); stamp("new.",r.new_file); stamp("reservation.",r.reservation);
    return text+"complete="+sha256_hex(text)+"\n";
}
std::expected<settings_recovery_record,std::string> parse_receipt(const fs::path& path,const fs::path& requested)
{
    auto raw=read_bytes(path,receipt_limit,true);
    if(!raw) return std::unexpected(raw.error());
    const auto complete=raw->rfind("complete=");
    if(!raw->starts_with(magic)||complete==raw->npos||raw->back()!='\n'||
       raw->substr(complete)!="complete="+sha256_hex(std::string_view(*raw).substr(0,complete))+"\n")
        return std::unexpected("incomplete or invalid receipt checksum");
    std::map<std::string,std::string> fields;
    size_t at=magic.size();
    while(at<complete)
    {
        auto end=raw->find('\n',at), eq=raw->find('=',at);
        if(end==raw->npos||eq>=end) return std::unexpected("invalid receipt fields");
        auto key=raw->substr(at,eq-at), value=decode_settings_key(std::string_view(*raw).substr(eq+1,end-eq-1));
        if(!fields.emplace(key,value).second) return std::unexpected("duplicate receipt field");
        at=end+1;
    }
    if(fields.size()!=20) return std::unexpected("unexpected receipt field count");
    settings_recovery_record r;
    r.transaction=fields["transaction"];
    if(r.transaction.size()!=32||!std::all_of(r.transaction.begin(),r.transaction.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');}))
        return std::unexpected("invalid transaction identifier");
    r.target=fs::u8path(fields["target"]);r.alias=fs::u8path(fields["alias"]);
    const auto backup=fs::u8path(fields["backup"]), temp=fs::u8path(fields["replacement"]);
    if(!r.target.is_absolute()||!r.alias.is_absolute()||!basename(backup)||!basename(temp)||
       (!same_path(requested,r.target)&&!same_path(requested,r.alias)))
        return std::unexpected("receipt path binding refused");
    r.backup=r.target.parent_path()/backup;r.replacement=r.target.parent_path()/temp;
    auto stamp=[&](std::string prefix,settings_recovery_snapshot& s){
        return number(fields[prefix+"volume"],s.volume)&&unhex(fields[prefix+"id"],s.id.data(),s.id.size())&&
            number(fields[prefix+"generation"],s.generation)&&number(fields[prefix+"length"],s.length)&&
            (s.hash=fields[prefix+"hash"]).size()==64&&s.length<=document_limit;
    };
    if(!stamp("old.",r.old_file)||!stamp("new.",r.new_file)||!stamp("reservation.",r.reservation)||
       r.old_file.generation==UINT64_MAX||r.new_file.generation!=r.old_file.generation+1||r.reservation.length!=0)
        return std::unexpected("invalid recovery generations/stamps");
    const auto name=utf8(path.filename()), prefix=utf8(requested.filename())+".recovery.";
    const auto suffix="."+r.transaction+".receipt";
    if(!name.starts_with(prefix)||!name.ends_with(suffix))return std::unexpected("receipt filename binding refused");
    const auto pid=std::string_view(name).substr(prefix.size(),name.size()-prefix.size()-suffix.size());
    if(pid.empty()||!std::all_of(pid.begin(),pid.end(),[](char c){return c>='0'&&c<='9';}))
        return std::unexpected("invalid receipt process identifier");
    const auto tail=path.filename().native().substr(requested.filename().native().size());
    r.receipts={fs::path(r.target.native()+tail)};
    if(!same_path(r.target,r.alias))r.receipts.push_back(fs::path(r.alias.native()+tail));
    return r;
}
std::expected<void,std::string> write_private(const fs::path& path,std::string_view text)
{
    auto user=current_user();if(!user)return std::unexpected(user.error());
    std::vector<unsigned char> acl(sizeof(ACL)+sizeof(ACCESS_ALLOWED_ACE)-sizeof(DWORD)+user->size());
    auto p=reinterpret_cast<PACL>(acl.data());SECURITY_DESCRIPTOR sd{};
    if(!InitializeAcl(p,DWORD(acl.size()),ACL_REVISION)||!AddAccessAllowedAceEx(p,ACL_REVISION,0,FILE_ALL_ACCESS,user->data())||
       !InitializeSecurityDescriptor(&sd,SECURITY_DESCRIPTOR_REVISION)||!SetSecurityDescriptorOwner(&sd,user->data(),FALSE)||
       !SetSecurityDescriptorDacl(&sd,TRUE,p,FALSE)||!SetSecurityDescriptorControl(&sd,SE_DACL_PROTECTED,SE_DACL_PROTECTED))
        return std::unexpected(native(GetLastError()));
    SECURITY_ATTRIBUTES attrs{sizeof attrs,&sd,FALSE};
    // Never expose an incomplete or exclusively open receipt to discovery.
    const fs::path temporary(path.native()+L".pending");
    handle file{CreateFileW(temporary.c_str(),GENERIC_WRITE|READ_CONTROL|DELETE,0,&attrs,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr)};
    if(file.h==INVALID_HANDLE_VALUE)return std::unexpected(native(GetLastError()));
    auto failed=[&](std::string why)->std::expected<void,std::string>{
        FILE_DISPOSITION_INFO remove{TRUE};
        SetFileInformationByHandle(file.h,FileDispositionInfo,&remove,sizeof remove); // own unpublished artifact
        return std::unexpected(std::move(why));
    };
    if(!private_receipt(file.h))return failed("filesystem did not enforce private receipt permissions");
    auto hook=std::exchange(before_publish_for_tests,{});
    if(hook)hook(temporary,path,false);
    DWORD written=0;
    if(!WriteFile(file.h,text.data(),DWORD(text.size()),&written,nullptr))return failed(native(GetLastError()));
    if(written!=text.size())return failed("incomplete receipt write");
    if(!FlushFileBuffers(file.h))return failed(native(GetLastError()));
    if(hook)hook(temporary,path,true);
    if(!CloseHandle(file.h))return failed(native(GetLastError()));
    file.h=INVALID_HANDLE_VALUE;
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH))
    {
        const DWORD error=GetLastError();
        DeleteFileW(temporary.c_str());
        return std::unexpected(native(error));
    }
    return {};
}
std::string remove_receipt(const fs::path& path,const std::string& transaction)
{
    // Recheck private complete ownership before deleting this specific transaction.
    auto name=path.native();auto token=std::wstring(transaction.begin(),transaction.end());
    if(name.find(token)==name.npos)return "receipt transaction name mismatch";
    auto bytes=read_bytes(path,receipt_limit,true);
    if(!bytes)
    {
        DWORD e=GetFileAttributesW(path.c_str());
        if(e==INVALID_FILE_ATTRIBUTES&&(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND))return {};
        return bytes.error();
    }
    if(bytes->find("transaction="+transaction+"\n")==bytes->npos)return "receipt transaction changed";
    if(!DeleteFileW(path.c_str()))return native(GetLastError());return {};
}
bool missing(const fs::path& p)
{
    if(GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES)return false;
    DWORD e=GetLastError();return e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND;
}
std::string remove_owned(const fs::path& p,const settings_recovery_snapshot& expected)
{
    handle h{CreateFileW(p.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    if(h.h==INVALID_HANDLE_VALUE)
    {
        DWORD e=GetLastError();if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)return {};
        return native(e);
    }
    FILE_ID_INFO id{};
    if(!GetFileInformationByHandleEx(h.h,FileIdInfo,&id,sizeof id))return native(GetLastError());
    if(id.VolumeSerialNumber!=expected.volume||std::memcmp(id.FileId.Identifier,expected.id.data(),expected.id.size()))
        return "recovery artifact identity changed";
    FILE_DISPOSITION_INFO remove{TRUE};
    if(!SetFileInformationByHandle(h.h,FileDispositionInfo,&remove,sizeof remove))return native(GetLastError());
    return {};
}
#endif
}  // namespace

void settings_recovery_fail_restore_for_tests(bool on){fail_restore=on;}


static settings_recovery_view inspect_settings_recovery_once(const fs::path& configured,
                                                             const fs::path& locked_target)
{
    settings_recovery_view out;out.target=configured;out.readable=configured;
#ifdef _WIN32
    auto requested=alias_name(configured);
    if(!requested&&locked_target.empty())return out; // ordinary open handles a missing parent
    std::vector<fs::path> names;
    if(requested)names.push_back(*requested);
    if(!locked_target.empty())names.push_back(locked_target);
    std::set<std::wstring> scanned;
    std::map<std::string,settings_recovery_record> records;
    for(size_t index=0;index<names.size();++index)
    {
        if(!scanned.insert(names[index].native()).second)continue;
        auto paths=receipt_paths(names[index]);
        if(!paths){out.phase=settings_recovery_phase::blocked;out.message=pending(names[index],paths.error());return out;}
        for(const auto& path:*paths)
        {
            auto hook=std::exchange(after_enumerate_for_tests,{});
            if(hook)hook(path);
            auto record=parse_receipt(path,names[index]);
            if(!record){out.phase=settings_recovery_phase::blocked;out.message=pending(path,record.error());return out;}
            if(!locked_target.empty()&&!same_path(record->target,locked_target))
            {out.phase=settings_recovery_phase::blocked;out.message=pending(path,"receipt target differs from the locked settings target");return out;}
            if(records.size()>=record_limit&&!records.contains(record->transaction))
            {out.phase=settings_recovery_phase::blocked;out.message=pending(path,"too many transactions");return out;}
            auto [it,inserted]=records.emplace(record->transaction,*record);
            if(!inserted&&serialize(it->second)!=serialize(*record))
            {out.phase=settings_recovery_phase::blocked;out.message=pending(path,"conflicting receipt copies");return out;}
            for(const auto& copy:record->receipts)
                if(std::none_of(it->second.receipts.begin(),it->second.receipts.end(),[&](const fs::path& old){return same_path(old,copy);}))
                    it->second.receipts.push_back(copy);
            if(std::none_of(it->second.receipts.begin(),it->second.receipts.end(),[&](const fs::path& old){return same_path(old,path);}))
                it->second.receipts.push_back(path);
            if(inserted)
            {
                names.push_back(record->target);names.push_back(record->alias);
                if(records.size()==1)out.target=record->target;
                else if(!same_path(out.target,record->target))
                {out.phase=settings_recovery_phase::blocked;out.message=pending(path,"ambiguous target binding");return out;}
            }
        }
    }
    if(records.empty())return out;
    const bool absent=missing(out.target);
    std::string target_bytes;
    auto target=snapshot(out.target,&target_bytes);
    std::optional<std::string> selected_backup_bytes;
    const settings_recovery_record* winner=nullptr;
    bool pending_restore=false;
    std::map<uint64_t,std::string> verified_generations;
    for(auto& [transaction,r]:records)
    {
        std::string backup_bytes;
        auto backup=snapshot(r.backup,absent?&backup_bytes:nullptr);
        const bool old_backup=backup&&same_document(*backup,r.old_file);
        if(old_backup)
        {
            auto [known,inserted]=verified_generations.emplace(r.old_file.generation,r.old_file.hash);
            if(!inserted&&known->second!=r.old_file.hash)
            {out.phase=settings_recovery_phase::blocked;out.message=pending(r.receipts.front(),"same-generation last-good backups disagree");return out;}
        }
        const bool new_target=target&&same_document(*target,r.new_file);
        const bool old_target=target&&same_document(*target,r.old_file);
        if(new_target)
        {
            if(out.phase==settings_recovery_phase::clear)out.phase=settings_recovery_phase::committed;
        }
        else if(old_target)
        {
            // A prepublication intent may have only an empty private placeholder.
            if(backup&&!old_backup&&!(same_id(*backup,r.reservation)&&backup->length==0))
            {out.phase=settings_recovery_phase::blocked;out.message=pending(r.receipts.front(),"backup identity/content is not owned");return out;}
            if(!backup&&!missing(r.backup))
            {out.phase=settings_recovery_phase::blocked;out.message=pending(r.receipts.front(),backup.error());return out;}
            if(out.phase==settings_recovery_phase::clear)out.phase=settings_recovery_phase::prepared;
        }
        else if(old_backup&&(absent||(target&&target->generation_present&&target->generation<r.old_file.generation)))
        {
            if(winner&&winner->old_file.generation==r.old_file.generation&&winner->old_file.hash!=r.old_file.hash)
            {out.phase=settings_recovery_phase::blocked;out.message=pending(r.receipts.front(),"same-generation last-good backups disagree");return out;}
            if(!winner||r.old_file.generation>winner->old_file.generation)
            {
                winner=&r;
                if(absent)selected_backup_bytes=std::move(backup_bytes);
            }
            pending_restore=true;
        }
        else
        {
            out.phase=settings_recovery_phase::blocked;
            out.message=pending(r.receipts.front(),"target is neither proved old/new nor strictly older than a verified last-good backup");
            return out;
        }
    }
    if(pending_restore)
    {
        out.phase=settings_recovery_phase::recoverable;
        if(absent)
        {
            out.readable=winner->backup;
            out.verified_bytes=std::move(selected_backup_bytes);
        }
        else if(target)out.verified_bytes=std::move(target_bytes);
    }
    else
    {
        out.readable=out.target;
        if(target)out.verified_bytes=std::move(target_bytes);
    }
    for(auto& [transaction,r]:records)out.records.push_back(std::move(r));
#else
    // Native Windows IDs cannot be verified on another platform. Never let a
    // mounted Windows pending receipt turn a missing document into an empty save.
    std::error_code ec;
    const auto parent=configured.parent_path().empty()?fs::path("."):configured.parent_path();
    const auto prefix=configured.filename().string()+".recovery.";
    size_t checked=0;
    fs::directory_iterator entry(parent,ec), end;
    if(ec)
    {
        if(ec == std::errc::no_such_file_or_directory)return out; // ordinary first save, no adjacent receipts
        out.phase=settings_recovery_phase::blocked;out.message=pending(configured,ec.message());return out;
    }
    for(;entry!=end;entry.increment(ec))
    {
        if(ec || ++checked>4096)
        {out.phase=settings_recovery_phase::blocked;out.message=pending(configured,"recovery scan could not complete within its bound");return out;}
        const auto name=entry->path().filename().string();
        if(name.starts_with(prefix)&&name.ends_with(".receipt"))
        {out.phase=settings_recovery_phase::blocked;out.message=pending(entry->path(),"native recovery verification requires Windows");return out;}
    }
    if(ec)
    {out.phase=settings_recovery_phase::blocked;out.message=pending(configured,ec.message());return out;}
#endif
    return out;
}

settings_recovery_view inspect_settings_recovery(const fs::path& configured, const fs::path& locked_target,
                                                std::chrono::steady_clock::time_point deadline)
{
    settings_recovery_view view;
    for(;;)
    {
        transient_discovery=false;
        view=inspect_settings_recovery_once(configured,locked_target);
        if(view.phase!=settings_recovery_phase::blocked||!transient_discovery||
           std::chrono::steady_clock::now()>=deadline)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto hook=std::exchange(after_inspect_for_tests,{});
    if(hook)hook();
    return view;
}
void settings_recovery_after_inspect_for_tests(std::function<void()> hook)
{after_inspect_for_tests=std::move(hook);}
void settings_recovery_before_publish_for_tests(std::function<void(const fs::path&,const fs::path&,bool)> hook)
{before_publish_for_tests=std::move(hook);}
void settings_recovery_after_enumerate_for_tests(std::function<void(const fs::path&)> hook)
{after_enumerate_for_tests=std::move(hook);}

std::expected<std::string,std::string> read_settings_document(const fs::path& file,
    std::chrono::steady_clock::time_point deadline)
{
    std::error_code ec;
    bool observed=fs::exists(file,ec);
    std::string why;
    for(;;)
    {
        auto view=inspect_settings_recovery(file,{},deadline);
        observed=observed||!view.records.empty();
        if(view.phase==settings_recovery_phase::blocked)
        {
            return std::unexpected(view.message);
        }
        if(view.verified_bytes)return std::move(*view.verified_bytes);
#ifdef _WIN32
        auto bytes=read_bytes(view.readable,document_limit,false);
        if(bytes)return bytes;
        why=bytes.error();
        if(!transient_discovery)return std::unexpected(why);
#else
        std::FILE* input=std::fopen(view.readable.c_str(),"rb");
        if(input)
        {
            std::string bytes;
            char buffer[16384];
            while(size_t count=std::fread(buffer,1,sizeof buffer,input))
            {
                if(bytes.size()+count>document_limit)
                {std::fclose(input);return std::unexpected("settings document exceeds bounded size");}
                bytes.append(buffer,count);
            }
            const int read_error=errno;
            const bool valid=!std::ferror(input);
            const int closed=std::fclose(input);
            const int close_error=errno;
            if(valid&&closed==0)return bytes;
            return std::unexpected(std::generic_category().message(valid?close_error:read_error));
        }
        const int error=errno;
        why=std::generic_category().message(error);
        if(error!=ENOENT)return std::unexpected(why);
#endif
        // A cooperating recovery may have moved the selected namespace after
        // inspection. Reinspect on the next attempt, never parse an unverified
        // replacement at the old backup path or cache a false empty document.
        ec.clear();
        observed=observed||fs::exists(file,ec);
        if(!observed||std::chrono::steady_clock::now()>=deadline)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ec.clear();
    if(!observed&&!fs::exists(file,ec)&&!ec)
    {
        const auto final=inspect_settings_recovery(file,{},deadline);
        if(final.verified_bytes)return *final.verified_bytes;
        if(final.phase==settings_recovery_phase::clear&&final.records.empty())return std::string{};
        if(!final.message.empty())why=final.message;
    }
    return std::unexpected(why.empty()?"settings changed while reading":why);
}

settings_recovery_step recover_settings_before_save(const fs::path& configured,
                                                    const fs::path& locked_target,
                                                    std::chrono::steady_clock::time_point deadline)
{
    settings_recovery_step result;
    auto view=inspect_settings_recovery(configured,locked_target,deadline);
    if(view.phase==settings_recovery_phase::blocked){result.error=view.message;return result;}
#ifdef _WIN32
    if(view.records.empty())return result;
    if(view.phase==settings_recovery_phase::recoverable)
    {
        const settings_recovery_record* winner=nullptr;
        for(const auto& r:view.records)
        {
            auto candidate=snapshot(r.backup);
            if(candidate&&same_document(*candidate,r.old_file)&&
               (!winner||r.old_file.generation>winner->old_file.generation))winner=&r;
        }
        if(!winner){result.error=pending(view.target,"no complete last-good backup remains");return result;}
        auto before=snapshot(view.target);
        const bool absent=missing(view.target);
        if(!absent&&(!before||!before->generation_present||before->generation>=winner->old_file.generation))
        {result.error=pending(winner->backup,"target changed before restore");return result;}
        if(std::exchange(fail_restore,false))
        {result.error=pending(winner->backup,"injected restore I/O failure");return result;}
        for(;;)
        {
            const DWORD flags=MOVEFILE_WRITE_THROUGH|(absent?0:MOVEFILE_REPLACE_EXISTING);
            if(MoveFileExW(winner->backup.c_str(),view.target.c_str(),flags)){result.restored=true;break;}
            DWORD e=GetLastError();
            if((e!=ERROR_SHARING_VIOLATION&&e!=ERROR_LOCK_VIOLATION)||std::chrono::steady_clock::now()>=deadline)
            {result.error=pending(winner->backup,native(e));return result;}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        view=inspect_settings_recovery(configured,locked_target,deadline);
        if(view.phase==settings_recovery_phase::blocked||view.phase==settings_recovery_phase::recoverable)
        {result.error=view.message.empty()?pending(view.target,"recovery is still pending"):view.message;return result;}
    }
    for(const auto& r:view.records)
    {
        auto backup=snapshot(r.backup);
        if(backup)
        {
            if(!same_document(*backup,r.old_file)&&!(same_id(*backup,r.reservation)&&backup->length==0))
            {result.error=pending(r.backup,"refusing unrelated backup");return result;}
            if(auto why=remove_owned(r.backup,*backup);!why.empty())
            {result.error=pending(r.backup,why);return result;}
        }
        else if(!missing(r.backup)){result.error=pending(r.backup,backup.error());return result;}
        if(auto why=finish_settings_recovery(r);!why.empty())
        {result.error=pending(r.receipts.front(),why);return result;}
    }
#endif
    return result;
}

std::expected<settings_recovery_record,std::string> begin_settings_recovery(
    const fs::path& target,const fs::path& configured,const fs::path& backup,const fs::path& replacement)
{
#ifdef _WIN32
    settings_recovery_record r;r.target=target;r.backup=backup;r.replacement=replacement;
    auto alias=alias_name(configured);if(!alias)return std::unexpected(alias.error());r.alias=*alias;
    auto old=snapshot(target), next=snapshot(replacement), reservation=snapshot(backup);
    if(!old||!next||!reservation)return std::unexpected(!old?old.error():!next?next.error():reservation.error());
    if(old->generation==UINT64_MAX||next->generation!=old->generation+1||reservation->length!=0)
        return std::unexpected("invalid replacement generation or backup reservation");
    r.old_file=*old;r.new_file=*next;r.reservation=*reservation;
    static std::atomic<uint64_t> counter=0;
    const auto nonce=std::to_string(GetCurrentProcessId())+":"+std::to_string(++counter)+":"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+":"+r.new_file.hash;
    r.transaction=sha256_hex(nonce).substr(0,32);
    const auto suffix=L".recovery."+std::to_wstring(GetCurrentProcessId())+L"."+
        std::wstring(r.transaction.begin(),r.transaction.end())+L".receipt";
    r.receipts={fs::path(r.target.native()+suffix)};
    if(!same_path(r.target,r.alias))r.receipts.push_back(fs::path(r.alias.native()+suffix));
    const auto bytes=serialize(r);
    if(bytes.size()>receipt_limit)return std::unexpected("receipt exceeds bounded size");
    std::vector<fs::path> written;
    for(const auto& path:r.receipts)
    {
        if(auto made=write_private(path,bytes);!made)
        {
            std::string why="could not publish required recovery receipt "+utf8(path)+": "+made.error();
            for(const auto& old_path:written)
                if(auto cleanup=remove_receipt(old_path,r.transaction);!cleanup.empty())why+="; "+pending(old_path,cleanup);
            return std::unexpected(why);
        }
        written.push_back(path);
    }
    return r;
#else
    return std::unexpected("native replacement receipts require Windows");
#endif
}

std::string finish_settings_recovery(const settings_recovery_record& record)
{
#ifdef _WIN32
    for(const auto& path:record.receipts)
        if(auto error=remove_receipt(path,record.transaction);!error.empty())return pending(path,error);
#endif
    return {};
}
} // namespace tortilla
