#include "json.h"

#include <charconv>

namespace tortilla::json
{
namespace
{

bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

size_t skip_space(std::string_view s, size_t at)
{
    while (at < s.size() && space(s[at])) ++at;
    return at;
}

// -- checking: once, over the whole text --------------------------------------

constexpr int k_deepest = 256;

bool hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool digit(char c)
{
    return c >= '0' && c <= '9';
}

// Past the value at `at` (no space before it), or npos when it isn't one.
size_t check(std::string_view s, size_t at, int depth);

size_t check_string(std::string_view s, size_t at)
{
    for (++at; at < s.size(); ++at)
    {
        const char c = s[at];
        if (c == '"') return at + 1;
        if (static_cast<unsigned char>(c) < 0x20) return std::string_view::npos;
        if (c != '\\') continue;
        if (++at >= s.size()) return std::string_view::npos;
        switch (s[at])
        {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't': break;
        case 'u':
            if (at + 4 >= s.size() || !hex(s[at + 1]) || !hex(s[at + 2]) || !hex(s[at + 3]) ||
                !hex(s[at + 4]))
                return std::string_view::npos;
            at += 4;
            break;
        default: return std::string_view::npos;
        }
    }
    return std::string_view::npos;
}

size_t check_number(std::string_view s, size_t at)
{
    if (at < s.size() && s[at] == '-') ++at;
    if (at >= s.size() || !digit(s[at])) return std::string_view::npos;
    if (s[at] == '0')
        ++at;
    else
        while (at < s.size() && digit(s[at])) ++at;
    if (at < s.size() && s[at] == '.')
    {
        if (++at >= s.size() || !digit(s[at])) return std::string_view::npos;
        while (at < s.size() && digit(s[at])) ++at;
    }
    if (at < s.size() && (s[at] == 'e' || s[at] == 'E'))
    {
        if (++at < s.size() && (s[at] == '+' || s[at] == '-')) ++at;
        if (at >= s.size() || !digit(s[at])) return std::string_view::npos;
        while (at < s.size() && digit(s[at])) ++at;
    }
    return at;
}

// An array's or object's entries, from past its bracket.
size_t check_entries(std::string_view s, size_t at, int depth, bool members)
{
    const char close = members ? '}' : ']';
    at = skip_space(s, at + 1);
    if (at < s.size() && s[at] == close) return at + 1;
    for (;;)
    {
        if (members)
        {
            if (at >= s.size() || s[at] != '"') return std::string_view::npos;
            at = check_string(s, at);
            if (at == std::string_view::npos) return at;
            at = skip_space(s, at);
            if (at >= s.size() || s[at] != ':') return std::string_view::npos;
            at = skip_space(s, at + 1);
        }
        at = check(s, at, depth + 1);
        if (at == std::string_view::npos) return at;
        at = skip_space(s, at);
        if (at >= s.size()) return std::string_view::npos;
        if (s[at] == close) return at + 1;
        if (s[at] != ',') return std::string_view::npos;
        at = skip_space(s, at + 1);
    }
}

size_t check(std::string_view s, size_t at, int depth)
{
    if (at >= s.size() || depth > k_deepest) return std::string_view::npos;
    auto word = [&](std::string_view w)
    { return s.substr(at, w.size()) == w ? at + w.size() : std::string_view::npos; };
    switch (s[at])
    {
    case '"': return check_string(s, at);
    case '{': return check_entries(s, at, depth, true);
    case '[': return check_entries(s, at, depth, false);
    case 't': return word("true");
    case 'f': return word("false");
    case 'n': return word("null");
    default: return check_number(s, at);
    }
}

// -- walking: over text already checked ---------------------------------------

// Past the value at `at`.
size_t skip(std::string_view s, size_t at)
{
    const char c = s[at];
    if (c == '"')
    {
        for (++at; s[at] != '"'; ++at)
            if (s[at] == '\\') ++at;
        return at + 1;
    }
    if (c == '{' || c == '[')
    {
        int depth = 0;
        for (; at < s.size(); ++at)
        {
            const char d = s[at];
            if (d == '"')
            {
                at = skip(s, at) - 1;
                continue;
            }
            if (d == '{' || d == '[')
                ++depth;
            else if ((d == '}' || d == ']') && --depth == 0)
                return at + 1;
        }
        return at;
    }
    while (at < s.size() && !space(s[at]) && s[at] != ',' && s[at] != '}' && s[at] != ']') ++at;
    return at;
}

void put_utf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80)
        out += char(cp);
    else if (cp < 0x800)
    {
        out += char(0xc0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3f));
    }
    else if (cp < 0x10000)
    {
        out += char(0xe0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    }
    else
    {
        out += char(0xf0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3f));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    }
}

std::uint32_t hex4(std::string_view s)
{
    std::uint32_t v = 0;
    std::from_chars(s.data(), s.data() + 4, v, 16);
    return v;
}

// A string's inside (no quotes), escapes undone.
std::string unescape(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t at = 0; at < in.size(); ++at)
    {
        if (in[at] != '\\')
        {
            out += in[at];
            continue;
        }
        switch (in[++at])
        {
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u':
        {
            std::uint32_t cp = hex4(in.substr(at + 1));
            at += 4;
            // A pair of surrogates is one character; a lone one isn't text.
            if (cp >= 0xd800 && cp < 0xdc00 && at + 6 < in.size() && in.substr(at + 1, 2) == "\\u")
            {
                const std::uint32_t low = hex4(in.substr(at + 3));
                if (low >= 0xdc00 && low < 0xe000)
                {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    at += 6;
                }
            }
            put_utf8(out, cp >= 0xd800 && cp < 0xe000 ? 0xfffd : cp);
            break;
        }
        default: out += in[at];  // " \ /
        }
    }
    return out;
}

}  // namespace

value::type value::kind() const
{
    if (text_.empty()) return type::missing;
    switch (text_.front())
    {
    case '"': return type::string;
    case '{': return type::object;
    case '[': return type::array;
    case 't':
    case 'f': return type::boolean;
    case 'n': return type::null;
    default: return type::number;
    }
}

value value::operator[](std::string_view key) const
{
    for (auto [k, v] : members())
        if (key_is(k, key)) return v;
    return {};
}

value value::operator[](size_t index) const
{
    for (value v : elements())
        if (index-- == 0) return v;
    return {};
}

std::string value::str(std::string_view fallback) const
{
    if (kind() != type::string) return std::string(fallback);
    return unescape(text_.substr(1, text_.size() - 2));
}

bool value::str_is(std::string_view s) const
{
    return kind() == type::string && key_is(text_.substr(1, text_.size() - 2), s);
}

std::optional<std::int64_t> value::integer() const
{
    if (kind() != type::number) return std::nullopt;
    std::int64_t v = 0;
    const auto [end, ec] = std::from_chars(text_.data(), text_.data() + text_.size(), v);
    if (ec != std::errc{} || end != text_.data() + text_.size()) return std::nullopt;
    return v;
}

bool value::boolean(bool fallback) const
{
    return kind() == type::boolean ? text_.front() == 't' : fallback;
}

value::range value::elements() const
{
    return kind() == type::array ? range{text_, false} : range{};
}

value::range value::members() const
{
    return kind() == type::object ? range{text_, true} : range{};
}

value::iterator value::range::begin() const
{
    return text.empty() ? end() : iterator(text, skip_space(text, 1), members);
}

value::iterator value::range::end() const
{
    return iterator(text, text.size(), members);
}

value::iterator::iterator(std::string_view text, size_t at, bool members)
    : text_(text)
    , at_(at)
    , members_(members)
{
    read();
}

// The entry at at_, or the end at the closing bracket.
void value::iterator::read()
{
    if (at_ >= text_.size() || text_[at_] == ']' || text_[at_] == '}')
    {
        at_ = text_.size();
        return;
    }
    size_t at = at_;
    if (members_)
    {
        const size_t key_end = skip(text_, at);
        key_ = text_.substr(at + 1, key_end - at - 2);
        at = skip_space(text_, skip_space(text_, key_end) + 1);  // past the colon
    }
    value_ = value(text_.substr(at, skip(text_, at) - at));
}

value::iterator& value::iterator::operator++()
{
    size_t at = skip_space(text_, size_t(value_.text_.data() + value_.text_.size() - text_.data()));
    if (at < text_.size() && text_[at] == ',') at = skip_space(text_, at + 1);
    at_ = at;
    read();
    return *this;
}

bool key_is(std::string_view escaped, std::string_view key)
{
    if (escaped.find('\\') == std::string_view::npos) return escaped == key;
    return unescape(escaped) == key;
}

std::optional<value> parse(std::string_view text)
{
    const size_t first = skip_space(text, 0);
    const size_t end = check(text, first, 0);
    if (end == std::string_view::npos || skip_space(text, end) != text.size()) return std::nullopt;
    return value(text.substr(first, end - first));
}

std::string quoted(std::string_view s)
{
    std::string out = "\"";
    out.reserve(s.size() + 2);
    for (const char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                constexpr char digits[] = "0123456789abcdef";
                out += "\\u00";
                out += digits[c >> 4];
                out += digits[c & 15];
            }
            else
                out += c;
        }
    }
    return out + '"';
}

}  // namespace tortilla::json
