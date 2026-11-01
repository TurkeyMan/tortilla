// A JSON reader over the text as it is, for the forges' replies: a value is
// a view of its own text, and looking into it walks that text, so nothing is
// copied until a string is asked for. parse() checks the whole text once;
// the walks after it trust it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tortilla::json
{

class value
{
public:
    enum class type
    {
        missing,  // what a lookup that finds nothing gives
        null,
        boolean,
        number,
        string,
        array,
        object,
    };

    value() = default;
    type kind() const;
    explicit operator bool() const { return !text_.empty(); }  // present (null included)
    bool is_null() const { return kind() == type::null; }

    // A member, or an element; missing when there's none, or this isn't an
    // object (an array).
    value operator[](std::string_view key) const;
    value operator[](size_t index) const;

    // A string's text, unescaped; `fallback` for anything else.
    std::string str(std::string_view fallback = {}) const;
    // Whether it's a string equal to `s`, without unescaping one that has
    // no escapes.
    bool str_is(std::string_view s) const;
    std::optional<std::int64_t> integer() const;
    bool boolean(bool fallback = false) const;
    // The value's own text, as it is in the reply.
    std::string_view text() const { return text_; }

    // for (json::value e : v.elements()), for (auto [key, v] : v.members()):
    // empty for anything but an array (an object). A member's key is still
    // escaped: compare it with key_is.
    class iterator;
    struct range
    {
        std::string_view text;  // the array's or object's, brackets and all
        bool members = false;
        iterator begin() const;
        iterator end() const;
    };
    range elements() const;
    range members() const;

private:
    friend class iterator;
    friend std::optional<value> parse(std::string_view);
    explicit value(std::string_view text)
        : text_(text)
    {
    }
    std::string_view text_;
};

// An element (its key empty) or a member.
struct entry
{
    std::string_view key;  // still escaped: compare it with key_is
    value val;
    operator value() const { return val; }
};

class value::iterator
{
public:
    entry operator*() const { return {key_, value_}; }
    iterator& operator++();
    bool operator==(const iterator& other) const { return at_ == other.at_; }

private:
    friend struct value::range;
    iterator(std::string_view text, size_t at, bool members);
    void read();
    std::string_view text_;
    size_t at_ = 0;  // where this entry starts; text_.size() at the end
    bool members_ = false;
    std::string_view key_;
    value value_;
};

// A member key from members() (escaped) against plain text.
bool key_is(std::string_view escaped, std::string_view key);

// The whole text as one value, viewing it: the text must outlive what
// comes from it. nullopt when it isn't JSON.
std::optional<value> parse(std::string_view text);

// `s` as a JSON string, quotes and all, for building a request's body.
std::string quoted(std::string_view s);

}  // namespace tortilla::json
