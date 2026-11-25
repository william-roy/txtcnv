// txtcnv.cpp

#include "txtcnv.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>
#include <string_view>

namespace TXTCNV {

// UTF-8 Helpers ---------------------------------------------------------------

static bool is_valid_utf8(std::string_view str)
{
    size_t n = str.size();
    size_t i = 0;

    // the k-th byte from the current one (0 past the end, which is never a
    // valid continuation byte)
    auto b = [&](size_t k) -> uint8_t 
    { 
        return i + k < n ? static_cast<uint8_t>(str[i + k]) : 0; 
    };

    auto in = [](uint8_t c, uint8_t lo, uint8_t hi) 
    { 
        return lo <= c && c <= hi; 
    };

    // iterate through all bytes in str and confirm they are valid utf-8
    while (i < n)
    {
        uint8_t c = b(0);
        size_t len = 0;

        if (c <= 0x7F) 
        { 
            len = 1; 
        }
        else if (in(c, 0xC2, 0xDF) && in(b(1), 0x80, 0xBF))     
        { 
            len = 2; 
        }
        else if (in(c, 0xE0, 0xEF))
        {
            // no overlongs (E0) or surrogates (ED)
            uint8_t lo = (c == 0xE0) ? 0xA0 : 0x80;
            uint8_t hi = (c == 0xED) ? 0x9F : 0xBF;
            if (in(b(1), lo, hi) && in(b(2), 0x80, 0xBF)) 
            { 
                len = 3; 
            }
        }
        else if (in(c, 0xF0, 0xF4))
        {
            // no overlongs (F0) or code points past U+10FFFF (F4)
            uint8_t lo = (c == 0xF0) ? 0x90 : 0x80;
            uint8_t hi = (c == 0xF4) ? 0x8F : 0xBF;
            if (in(b(1), lo, hi) && in(b(2), 0x80, 0xBF) && 
                in(b(3), 0x80, 0xBF)) 
                { 
                    len = 4; 
                }
        }

        if (len == 0) return false;

        i += len;
    }

    return true;
}

// takes first byte in a utf-8 sequence, returns size of the character in bytes
static size_t utf8_char_size(char lead)
{
    uint8_t c = static_cast<uint8_t>(lead);
    if (c < 0xC0) return 1;
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    return 4;
}

// takes a utf-8 string, returns its length in unicode characters
static size_t utf8_len(std::string_view str)
{
    size_t n = 0;
    for (size_t i = 0; i < str.size(); i += utf8_char_size(str[i])) 
    {
        n++; 
    }
    return n;
}

// takes contents of utf-8 file and returns it without the BOM, if there is one
static std::string_view strip_bom(std::string_view str)
{
    if (str.starts_with("\xEF\xBB\xBF")) str.remove_prefix(3);
    return str;
}

// Text Helpers ----------------------------------------------------------------

// checks if c is an alphanumeric character
static bool is_alnum(char c)
{
    return ('0' <= c && c <= '9') || 
           ('A' <= c && c <= 'Z') || 
           ('a' <= c && c <= 'z');
}

// for validating labels and table IDs: [0-9A-Za-z]+
static bool is_label(std::string_view str)
{
    return !str.empty() && std::ranges::all_of(str, is_alnum);
}

static bool is_space(char c)
{
    return c == ' '  || 
           c == '\t' || 
           c == '\r' || 
           c == '\n';
}

// returns a string representation of a raw byte, e.g. "[$FF]"
static std::string raw_byte_str(uint8_t byte)
{
    return std::format("[${:02X}]", byte);
}

// returns a string of hex values
static std::string hex_str(const Bytes& bytes)
{
    std::string str;
    for (uint8_t byte : bytes) 
    { 
        str += std::format("{:02X}", byte);
    }
    return str;
}

// parses str as an integer
static bool parse_int(std::string_view str, int& out, int base = 10)
{
    const char* last = str.data() + str.size();
    auto [end, err] = std::from_chars(str.data(), last, out, base);
    return err == std::errc() && end == last;
}

// parses str as a byte
static bool parse_byte(std::string_view str, uint8_t& out, int base)
{
    int value = 0;
    
    if (!parse_int(str, value, base) || value < 0 || value > 255)
    {
        return false;
    }

    out = static_cast<uint8_t>(value);
    return true;
}

// parses a string of hex digits
static bool parse_hex(std::string_view str, Bytes& out)
{
    if (str.empty() || str.size() % 2 != 0) return false; // at least two chars

    out.clear();
    for (size_t i = 0; i < str.size(); i += 2)
    {
        uint8_t byte = 0;
        if (!parse_byte(str.substr(i, 2), byte, 16)) return false;
        out.push_back(byte);
    }

    return true;
}

// Splits text into lines (at LF, CR or CRLF)
static std::vector<std::string_view> split_lines(std::string_view str)
{
    std::vector<std::string_view> lines;
    size_t start = 0;

    for (size_t i = 0; i <= str.size(); i++)
    {
        if (i < str.size() && str[i] != '\r' && str[i] != '\n') continue;

        lines.push_back(str.substr(start, i - start));
        if (i + 1 < str.size() && str[i] == '\r' && str[i + 1] == '\n') i++;
        start = i + 1;
    }

    return lines;
}

// ParamFormat -----------------------------------------------------------------

std::string ParamFormat::format_value(uint8_t value) const
{
    std::string digits;

    switch (byte_format)
    {
        case 'X': digits = std::format("{:02X}", value); break;
        case 'D': digits = std::format("{}", value); break;
        case 'B': digits = std::format("{:08b}", value); break;
    }

    return prefix + digits + suffix;
}

bool ParamFormat::parse_value(std::string_view text, uint8_t& value) const
{
    if (text.size() < prefix.size() + suffix.size() ||
        !text.starts_with(prefix) || !text.ends_with(suffix))
    {
        return false;
    }

    std::string_view digits = text.substr(prefix.size(),
        text.size() - prefix.size() - suffix.size());
    int base = (byte_format == 'X') ? 16 : (byte_format == 'D') ? 10 : 2;

    return parse_byte(digits, value, base);
}

// Table -----------------------------------------------------------------------

// Returns entry with exactly this hex sequence
const Entry* Table::find_hex(const Bytes& hex) const
{
    auto it = entries_.find(hex);
    return (it != entries_.end()) ? &it->second : nullptr;
}

// Returns number of entries of a given type
size_t Table::count(EntryType type) const
{
    size_t n = 0;
    for (const auto& [hex, entry] : entries_)
    {
        if (entry.type == type) n++;
    }
    return n;
}

// Returns the entry matching the most bytes at the start of data
const Entry* Table::longest_match(const uint8_t* data, size_t size) const
{
    for (size_t len = std::min(size, max_hex_len_); len > 0; len--)
    {
        if (const Entry* entry = find_hex(Bytes(data, data + len))) 
        { 
            return entry; 
        }
    }
    return nullptr;
}

// Checks if any entry's hex begins with prefix and continues past it
bool Table::has_longer_entry_starting_with(const Bytes& prefix) const
{
    // entries are sorted by hex, so ones starting with prefix come right after
    auto it = entries_.upper_bound(prefix);
    return it != entries_.end() && it->first.size() > prefix.size() &&
        std::equal(prefix.begin(), prefix.end(), it->first.begin());
}

// Returns the normal entry with this text
const Entry* Table::find_normal(const std::string& text) const
{
    auto it = by_text_.find(text);
    return (it != by_text_.end()) ? it->second : nullptr;
}

// Returns the non-normal entry with this label
const Entry* Table::find_non_normal(const std::string& label) const
{
    auto it = by_label_.find(label);
    return (it != by_label_.end()) ? it->second : nullptr;
}

// Adds an entry, failing on a duplicate hex sequence or label
bool Table::add(Entry entry, std::string& error)
{
    // ensure unique hex sequence
    if (entries_.count(entry.hex))
    {
        error = "duplicate hex sequence " + hex_str(entry.hex);
        return false;
    }

    // ensure unique label
    if (entry.type != EntryType::Normal && by_label_.count(entry.label))
    {
        error = "duplicate label [" + entry.label + "]";
        return false;
    }

    Bytes hex = entry.hex;
    const Entry& e = entries_.emplace(std::move(hex), std::move(entry)).first->second;
    max_hex_len_ = std::max(max_hex_len_, e.hex.size());

    if (e.type != EntryType::Normal)
    {
        by_label_[e.label] = &e;
        if (e.type == EntryType::Switch) switches_.push_back(&e);
        return true;
    }

    // empty normal entries are only used for skipping bytes in decoding
    if (e.text.empty()) return true;

    // try to add to by_text_ if text isn't there yet
    auto [it, inserted] = by_text_.try_emplace(e.text, &e);

    // replace old existing entry if new hex is shorter
    if (!inserted && it->second->hex.size() >= e.hex.size()) it->second = &e;

    // update max text length in chars
    max_text_chars_ = std::max(max_text_chars_, utf8_len(e.text));

    return true;
}

// Sets each table switch's target based on its label (a table ID)
bool Table::resolve_switches(const std::unordered_map<std::string, int>& ids, 
                             std::string& error)
{
    for (auto & [hex, entry] : entries_)
    {
        if (entry.type != EntryType::Switch) continue;

        auto it = ids.find(entry.label);
        if (it == ids.end())
        {
            error = "table '" + id_ + "' switches to unknown table '" + 
                    entry.label + "'";
            return false;
        }
        entry.target = it->second;
    }
    return true;
}

// Table file (.tbl) parsing ---------------------------------------------------

// Parses a param string like "palette=$%X"
static bool tbl_parse_param_format(std::string_view str, ParamFormat& param,
                                   std::string& error)
{
    std::string* part = &param.prefix;
    bool found = false;

    for (size_t i = 0; i < str.size(); i++)
    {
        char c = str[i];

        if (is_space(c) || c == '[' || c == ']')
        {
            error = "parameter '" + std::string(str) + 
                    "' can't contain spaces or brackets";
            return false;
        }

        if (c != '%') 
        { 
            *part += c; 
            continue; 
        }

        char byte_format = (i + 1 < str.size()) ? str[++i] : '\0';

        if (byte_format == '%')
        {
            *part += '%';
            continue;
        }

        if (!found && (byte_format == 'X' || 
                       byte_format == 'D' || 
                       byte_format == 'B'))
        {
            param.byte_format = byte_format;
            part = &param.suffix;
            found = true;
            continue;
        }

        found = false;
        break;
    }

    if (!found)
    {
        error = "parameter '" + std::string(str) + 
                "' needs exactly one placeholder (%X, %D or %B)";
        return false;
    }

    return true;
}

// Parses a table entry (normal or non-normal)
static bool tbl_parse_entry(std::string_view line, Entry& entry, 
                            std::string& error)
{
    switch (line[0])
    {
        case '$': entry.type = EntryType::Control; break;
        case '/': entry.type = EntryType::End; break;
        case '!': entry.type = EntryType::Switch; break;
        default:  entry.type = EntryType::Normal; break;
    }

    if (entry.type != EntryType::Normal) line.remove_prefix(1);

    size_t equals = line.find('=');
    if (equals == std::string_view::npos)
    {
        error = "unrecognized line (expected 'hex=text')";
        return false;
    }

    if (!parse_hex(line.substr(0, equals), entry.hex))
    {
        error = "invalid hex sequence '" + 
                std::string(line.substr(0, equals)) +
                "' (it must be an even number of hex digits)";
        return false;
    }

    std::string_view rest = line.substr(equals + 1);

    if (entry.type == EntryType::Normal)
    {
        if (rest.find_first_of("[]") != std::string_view::npos)
        {
            error = "text can't contain '[' or ']'";
            return false;
        }

        entry.text = rest;
        return true;
    }

    // [label]
    size_t close = rest.find(']');

    if (!rest.starts_with('[') || 
        close == std::string_view::npos || 
        !is_label(rest.substr(1, close - 1)))
    {
        error = "expected a [label] of letters and digits after '='";
        return false;
    }

    entry.label = rest.substr(1, close - 1);
    rest.remove_prefix(close + 1);

    // line breaks
    while (rest.starts_with("\\n"))
    {
        entry.linebreaks++;
        rest.remove_prefix(2);
    }

    // ,parameter,parameter...
    std::vector<std::string_view> params;
    if (!rest.empty())
    {
        if (rest[0] != ',')
        {
            error = "unexpected '" + std::string(rest) + "' after the label";
            return false;
        }

        rest.remove_prefix(1);
        for (;;)
        {
            size_t comma = rest.find(',');
            params.push_back(rest.substr(0, comma));
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
    }

    switch (entry.type)
    {
        case EntryType::End:
            if (!params.empty())
            {
                error = "end tokens can't have parameters";
                return false;
            }
            break;

        case EntryType::Switch:
        {
            if (params.size() != 1 || 
                !parse_int(params[0], entry.match_limit) || 
                entry.match_limit < FALL_BACK)
            {
                error = "a table switch needs one fallback condition:" 
                        "-1, 0 or a number of matches";
                return false;
            }
            break;
        }

        case EntryType::Control:
            for (std::string_view str : params)
            {
                ParamFormat param;
                if (!tbl_parse_param_format(str, param, error)) return false;
                entry.params.push_back(param);
            }
            break;

        case EntryType::Normal:
            break;
    }

    return true;
}

// Table stack -----------------------------------------------------------------
//
// The LIFO stack of tables that encoding and decoding passes switch between.
// Active table on top, starting table on bottom.

static constexpr int STARTING_TABLE = 0;

class TableStack {
private:
    struct Level {
        int table;
        int matches_left;  // or NO_LIMIT
        auto operator<=>(const Level&) const = default;
    };

    std::vector<Level> levels_ = {{STARTING_TABLE, NO_LIMIT}};

    bool contains(int table) const
    {
        return std::ranges::any_of(levels_, [&](const Level& l) { return l.table == table; });
    }

public:
    int top() const { return levels_.back().table; }
    size_t depth() const { return levels_.size(); }

    // fall back to the table below
    void fall_back()
    {
        if (depth() == 1) return;
        levels_.pop_back();
        count_match();
    }

    void count_match()
    {
        Level& level = levels_.back();
        if (level.matches_left == NO_LIMIT) return;

        // fall back if this was the last one allowed
        level.matches_left--;
        if (level.matches_left == 0) fall_back();
    }

    // follows a table switch entry
    void follow(const Entry& sw)
    {
        if (sw.match_limit == FALL_BACK && depth() > 1)
        {
            fall_back();
            return;
        }

        // the starting table has nothing to fall back to, so there a
        // FALL_BACK switch goes to its target with no limit
        int limit = (sw.match_limit == FALL_BACK) ? NO_LIMIT : sw.match_limit;
        levels_.push_back({sw.target, limit});
    }

    // A table switch can't put a table on the stack twice. This stops
    // switches that go round in circles
    bool can_follow(const Entry& sw) const
    {
        if (sw.match_limit == FALL_BACK && depth() > 1) return true;
        return !contains(sw.target);
    }

    // Falls back to the starting table
    void reset() { levels_.resize(1); }

    auto operator<=>(const TableStack&) const = default;
};

// Encoding --------------------------------------------------------------------
//
// A shortest path search over the potential states of the decoder. Each state
// holds position in the script, table stack, and partial matches. Each move 
// writes one table entry (or raw byte or table switch) and costs its size in 
// bytes, or falls back to the table below. 
// 
// Moves only allow what the decoder would read back the same way. A single byte 
// written could potentially be part of a longer match based on the later bytes.
// Likewise, a fallback condition could potentially be wrong if subesequent 
// bytes would match an entry in the active table.

enum class ItemType { Char, Code, Raw };

// One piece of the text to encode
struct Item {
    ItemType type = ItemType::Char;
    std::string text;               // Char: the (UTF-8) character
    std::string label;              // Code: the label
    std::vector<std::string> args;  // Code: the parameters
    uint8_t byte = 0;               // Raw: the byte
};

// breaks text into characters, [codes] and raw [$XX] bytes
static bool lex_script(std::string_view str, std::vector<Item>& items, 
                       std::string& error)
{
    for (size_t i = 0; i < str.size(); )
    {
        char c = str[i];

        if (c == '\r' || c == '\n') // newline
        { 
            i++; 
            continue; 
        }

        if (c == ']') // illegal bracket
        {
            error = "unexpected ']' in the text";
            return false;
        }

        if (c != '[')
        {
            size_t len = std::min(utf8_char_size(c), str.size() - i);

            Item item;
            item.type = ItemType::Char;
            item.text = str.substr(i, len);
            items.push_back(std::move(item));

            i += len;
            continue;
        }

        size_t close = str.find_first_of("[]", i + 1);

        if (close == std::string_view::npos || str[close] != ']')
        {
            error = "missing ']' after '" + 
                    std::string(str.substr(i, 20)) + "'";
            return false;
        }

        std::string_view token = str.substr(i + 1, close - i - 1);
        i = close + 1;

        // [$XX]
        Bytes raw;
        if (token.starts_with('$') && token.size() == 3 && 
            parse_hex(token.substr(1), raw))
        {
            Item item;
            item.type = ItemType::Raw;
            item.byte = raw[0];
            items.push_back(std::move(item));
            continue;
        }

        // [label arg arg...]
        Item item;
        item.type = ItemType::Code;
        std::istringstream words{std::string(token)};
        words >> item.label;

        for (std::string arg; words >> arg;)
        {
            item.args.push_back(arg);
        }

        if (!is_label(item.label))
        {
            error = "invalid code '[" + std::string(token) + "]'";
            return false;
        }

        items.push_back(std::move(item));
    }

    return true;
}

class Encoder {
private:

    // partial byte sequence matches (potential paths or dead ends)
    struct PartialMatch {
        int table;
        Bytes prefix;  // bytes written so far that the table might match
        auto operator<=>(const PartialMatch&) const = default;
    };

    // the state the decoder would be in when reading
    struct DecoderState {
        size_t pos = 0;   // number of script items written
        TableStack stack;
        std::vector<PartialMatch> partial_matches;
        auto operator<=>(const DecoderState&) const = default;

        // adds a partial match
        void open_partial_match(const std::vector<Table>& tables, int table,
                                const Bytes& prefix)
        {
            if (tables[table].has_longer_entry_starting_with(prefix))
            {
                partial_matches.push_back({table, prefix});
            }
        }

        // add written bytes to each partial match
        bool advance_partial_matches(const std::vector<Table>& tables,
                                     const Bytes& bytes)
        {
            std::vector<PartialMatch> open;

            for (PartialMatch m : partial_matches)
            {
                const Table& table = tables[m.table];
                bool still_open = true;

                for (uint8_t byte : bytes)
                {
                    m.prefix.push_back(byte);
                    
                    // return false if partial match becomes whole
                    if (table.find_hex(m.prefix)) return false;

                    if (!table.has_longer_entry_starting_with(m.prefix))
                    {
                        still_open = false;
                        break;
                    }
                }

                if (still_open) open.push_back(std::move(m));
            }

            partial_matches = std::move(open);
            return true;
        }

        // updates partial matches for an entry of the active table
        bool read_entry(const std::vector<Table>& tables, const Bytes& hex,
                        const Bytes& params)
        {
            // return false if partial match becomes whole
            if (!advance_partial_matches(tables, hex)) return false;

            // a longer entry of the active table can still take the next bytes
            open_partial_match(tables, stack.top(), hex);

            return advance_partial_matches(tables, params);
        }
    };

    // one step of the search, and the decoder state it leads to
    struct Step {
        DecoderState state;
        size_t cost;        // bytes written so far
        int prev;           // the step this one was reached from
        Bytes bytes;        // bytes written by this move
        bool is_switch;     // the move wrote a table switch
        bool stale = false; // if a cheaper way to the same state was found
    };

    // the step before the first one
    static constexpr int NO_STEP = -1;

    const std::vector<Table>& tables_;
    const std::vector<Item>& items_;

    std::vector<Step> steps_;
    std::map<DecoderState, int> cheapest_step_;  // cheapest step for each state
    std::priority_queue<std::pair<size_t, int>, std::vector<std::pair<size_t, int>>,
        std::greater<>> queue_;  // (cost, step), cheapest first
        
    size_t furthest_ = 0;

    // adds a step to the search
    void add_step(DecoderState state, int prev, Bytes bytes, bool is_switch = false)
    {
        // the same partial matches in a different order are the same state
        std::vector<PartialMatch>& matches = state.partial_matches;
        std::ranges::sort(matches);
        matches.erase(std::ranges::unique(matches).begin(), matches.end());

        size_t cost = (prev == NO_STEP ? 0 : steps_[prev].cost) + bytes.size();
        int id = static_cast<int>(steps_.size());

        // check if a step as cheap pointing to the same state already exists
        auto [it, inserted] = cheapest_step_.try_emplace(state, id);
        if (!inserted)
        {
            if (steps_[it->second].cost <= cost) return;
            steps_[it->second].stale = true;
            it->second = id;
        }

        steps_.push_back({std::move(state), cost, prev, std::move(bytes), is_switch});
        queue_.push({cost, id});
    }

    // Write moves ------------------------------------------------------------- 
    // 1. Copies the state of the step it starts from
    // 2. Changes the state to what the decoder would be doing after the move
    // 3. Adds it as a new step

    // writes a normal entry, control code, or end token, add one match
    void write_entry(int step, const DecoderState& from, size_t next_pos,
                     const Entry& entry, const Bytes& params = {})
    {
        DecoderState next = from;
        next.pos = next_pos;

        if (!next.read_entry(tables_, entry.hex, params)) return;
        next.stack.count_match();

        Bytes bytes = entry.hex;
        bytes.insert(bytes.end(), params.begin(), params.end());
        add_step(std::move(next), step, std::move(bytes));
    }

    // write a table switch and follows it, leaving the decoder at next_pos
    void write_switch(int step, const DecoderState& from, size_t next_pos,
                      const Entry& sw)
    {
        if (!from.stack.can_follow(sw)) return;

        DecoderState next = from;
        next.pos = next_pos;

        if (!next.read_entry(tables_, sw.hex, {})) return;
        next.stack.follow(sw);

        add_step(std::move(next), step, sw.hex, true);
    }

    // fall back to the table below, without writing anything
    void fall_back(int step, const DecoderState& from)
    {
        if (from.stack.depth() == 1) return;

        DecoderState next = from;
        next.open_partial_match(tables_, from.stack.top(), {});
        next.stack.fall_back();

        add_step(std::move(next), step, {});
    }

    // write a raw byte
    void write_raw(int step, const DecoderState& from, uint8_t byte)
    {
        if (from.stack.depth() > 1) return;

        DecoderState next = from;
        next.pos++;

        next.open_partial_match(tables_, from.stack.top(), {});
        if (!next.advance_partial_matches(tables_, {byte})) return;

        add_step(std::move(next), step, {byte});
    }

    // true if there is an item at pos and it has this type
    bool has_item_of_type(size_t pos, ItemType type) const
    {
        return pos < items_.size() && items_[pos].type == type;
    }

    // the param bytes for the control code at pos
    // ("[Color palette=$A4 index=52]") or as the raw bytes after it
    // ("[Color][$A4][$34]"). raw_items_used is how many of those
    bool read_param_bytes(const Entry& entry, size_t pos, Bytes& out, 
                          size_t& raw_items_used) const
    {
        const Item& item = items_[pos];
        out.clear();
        raw_items_used = 0;

        if (!item.args.empty())
        {
            if (item.args.size() != entry.params.size()) return false;

            for (size_t i = 0; i < item.args.size(); i++)
            {
                uint8_t value;
                if (!entry.params[i].parse_value(item.args[i], value)) 
                { 
                    return false;
                }
                out.push_back(value);
            }
            return true;
        }

        for (size_t i = 1; i <= entry.params.size(); i++)
        {
            if ((pos + i) >= items_.size() || items_[pos + i].type != ItemType::Raw) 
            { 
                return false; 
            }
            out.push_back(items_[pos + i].byte);
        }

        raw_items_used = entry.params.size();
        return true;
    }

    // try to write a normal entry
    void try_text(int step, const DecoderState& from)
    {
        const Table& table = tables_[from.stack.top()];
        std::string text;

        for (size_t k = 0; k < table.max_text_chars() &&
            has_item_of_type(from.pos + k, ItemType::Char); k++)
        {
            text += items_[from.pos + k].text;

            if (const Entry* entry = table.find_normal(text))
            {
                write_entry(step, from, from.pos + k + 1, *entry);
            }
        }
    }

    // try to write a non-normal control code
    void try_code(int step, const DecoderState& from)
    {
        if (!has_item_of_type(from.pos, ItemType::Code)) return;

        const Item& item = items_[from.pos];
        const Entry* entry = tables_[from.stack.top()].find_non_normal(item.label);
        if (!entry) return;

        if (entry->type == EntryType::Switch)
        {
            // switches take no parameters
            if (item.args.empty()) { write_switch(step, from, from.pos + 1, *entry); }
            return;
        }

        Bytes params;
        size_t raw_items_used;
        if (read_param_bytes(*entry, from.pos, params, raw_items_used))
        {
            write_entry(step, from, from.pos + 1 + raw_items_used, *entry, params);
        }
    }

    // try to write a table switch
    void try_switches(int step, const DecoderState& from)
    {
        for (const Entry* sw : tables_[from.stack.top()].switches())
        {
            write_switch(step, from, from.pos, *sw);
        }
    }

    // try to write a raw byte
    void try_raw(int step, const DecoderState& from)
    {
        if (!has_item_of_type(from.pos, ItemType::Raw)) return;
        write_raw(step, from, items_[from.pos].byte);
    }

    // the script text a step wrote, e.g. "He", [COLOR red=255] or a table switch
    std::string step_text(const Step& step) const
    {
        if (step.is_switch)
        {
            return "switch to table " + tables_[step.state.stack.top()].id();
        }

        size_t from = steps_[step.prev].state.pos;
        std::string text;

        for (size_t i = from; i < step.state.pos; i++)
        {
            const Item& item = items_[i];

            if (item.type == ItemType::Char) text += item.text;
            if (item.type == ItemType::Raw) text += raw_byte_str(item.byte);
            if (item.type == ItemType::Code)
            {
                text += "[" + item.label;
                for (const std::string& arg : item.args) text += " " + arg;
                text += "]";
            }
        }

        // quote normal text, so spaces at either end can be seen
        return (items_[from].type == ItemType::Char) ? "\"" + text + "\"" : text;
    }

    // all the moves from a step
    void expand(int step)
    {
        // a copy, because adding steps can move the ones in steps_
        const DecoderState from_state = steps_[step].state;

        try_text(step, from_state);
        try_code(step, from_state);
        try_switches(step, from_state);
        try_raw(step, from_state);
        fall_back(step, from_state);
    }

public:
    Encoder(const std::vector<Table>& tables, const std::vector<Item>& items)
        : tables_(tables), items_(items) {}

    // finds the shortest encoding, or returns false
    bool run(Bytes& out, Stats& stats, bool verbose = false)
    {
        add_step(DecoderState{}, NO_STEP, {});

        while (!queue_.empty())
        {
            int id = queue_.top().second;
            queue_.pop();

            if (steps_[id].stale) continue;

            // the first step to reach the end is the cheapest
            if (steps_[id].state.pos == items_.size())
            {
                std::vector<int> path;
                for (int i = id; i != NO_STEP; i = steps_[i].prev) 
                { 
                    path.push_back(i); 
                }

                for (auto it = path.rbegin(); it != path.rend(); it++)
                {
                    const Step& step = steps_[*it];

                    // the text this step wrote, and the bytes it became
                    if (verbose && !step.bytes.empty())
                    {
                        std::cerr << std::format("0x{:X}: {} -> {}\n", out.size(),
                                                 step_text(step), hex_str(step.bytes));
                    }

                    out.insert(out.end(), step.bytes.begin(), step.bytes.end());

                    if (step.is_switch)
                    {
                        stats.switches++;
                        stats.switch_bytes += step.bytes.size();
                    }
                }
                return true;
            }

            furthest_ = std::max(furthest_, steps_[id].state.pos);
            expand(id);
        }

        return false;
    }
    
    std::string failure() const; // why the search failed
};

std::string Encoder::failure() const
{
    const Item& item = items_[furthest_];

    if (item.type == ItemType::Char)
    {
        std::string text;
        for (size_t i = furthest_; i < items_.size() && i < furthest_ + 10 &&
            items_[i].type == ItemType::Char; i++)
        {
            text += items_[i].text;
        }

        return "no table entry for '" + text + "' (or no way to switch to a " 
               "table with one that would have matching decode)";
    }

    if (item.type == ItemType::Raw)
    {
        return "can't insert raw byte " + raw_byte_str(item.byte) + ": "
               "the decoder would read it as part of a table entry "
               "(or as the parameter of a control code)";
    }

    // codes
    const Entry* found = nullptr;
    for (const Table& table : tables_)
    {
        if (const Entry* entry = table.find_non_normal(item.label)) found = entry;
    }

    if (!found) return "unknown code [" + item.label + "]";

    // switches take no parameters
    Bytes params;
    size_t raw_items_used;
    bool wrong_params = (found->type == EntryType::Switch)
        ? !item.args.empty()
        : !read_param_bytes(*found, furthest_, params, raw_items_used);

    if (wrong_params)
    {
        return "wrong parameters for [" + item.label + "]: it takes " + 
               std::to_string(found->params.size()) +
               " (given in the code, or as that many [$XX] bytes after it)";
    }

    return "can't reach [" + item.label + "] from the active table";
}

// TextCodec -------------------------------------------------------------------

bool TextCodec::fail(std::string message)
{
    error_ = std::move(message);
    return false;
}

bool TextCodec::LoadTableFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail("could not open '" + path + "'");

    std::stringstream content;
    content << file.rdbuf();

    return LoadTable(content.str());
}

bool TextCodec::LoadTable(const std::string& content)
{
    std::string_view str = strip_bom(content); // ignore BOM

    if (!is_valid_utf8(str)) return fail("the table is not valid UTF-8");

    std::vector<Table> tables;
    std::unordered_map<std::string, int> ids;
    std::string error;
    int line_num = 0;

    auto add_table = [&](std::string_view id)
    {
        if (!ids.try_emplace(std::string(id), static_cast<int>(tables.size())).second) 
        { 
            return false; 
        }

        tables.emplace_back(std::string(id));
        return true;
    };

    for (std::string_view line : split_lines(str))
    {
        line_num++;
        auto line_error = [&](const std::string& message)
        {
            return fail("line " + std::to_string(line_num) + ": " + message);
        };

        // ignore blank lines and comments
        if (std::ranges::all_of(line, is_space) || line[0] == '#') 
        { 
            continue; 
        }
        
        if (line[0] == '@') // @TableID
        {
            std::string_view id = line.substr(1);
            
            if (!is_label(id)) 
            { 
                return line_error("a table ID must be letters and digits only"); 
            }
            
            if (!add_table(id)) 
            { 
                return line_error("duplicate table ID '" + std::string(id) + "'"); 
            }

            continue;
        }

        // entries before the first table ID go in a table named "default"
        if (tables.empty()) add_table("default");

        Entry entry;
        if (!tbl_parse_entry(line, entry, error) || 
            !tables.back().add(std::move(entry), error))
        {
            return line_error(error);
        }
    }

    if (tables.empty()) return fail("the table has no entries");

    for (Table& table : tables)
    {
        if (!table.resolve_switches(ids, error)) return fail(error);
    }

    tables_ = std::move(tables);
    error_.clear();
    return true;
}

bool TextCodec::Encode(Bytes& data, const std::string& text, bool verbose)
{    
    if (tables_.empty()) return fail("no table loaded");

    data.clear();
    stats_ = {};

    std::string_view str = strip_bom(text); // ignore BOM
    if (!is_valid_utf8(str)) return fail("text is not valid UTF-8");

    std::vector<Item> items;
    std::string error;
    if (!lex_script(str, items, error)) return fail(error);

    Encoder encoder(tables_, items);
    if (!encoder.run(data, stats_, verbose)) return fail(encoder.failure());

    return true;
}

// Decoding --------------------------------------------------------------------
//
// Decoding is much simpler than encoding and is all done in this function, 
// performed with a greedy matching algorithm.

bool TextCodec::Decode(std::string& text, const Bytes& data, bool verbose)
{
    if (tables_.empty()) return fail("no table loaded");
    text.clear();
    stats_ = {};

    TableStack stack;
    size_t pos = 0;
    size_t start = 0; // where the current entry's bytes begin

    // the bytes read since start, and the text they became
    auto verbose_print = [&](const std::string& result)
    {
        if (!verbose) return;
        Bytes read(data.begin() + start, data.begin() + pos);
        std::cerr << std::format("0x{:X}: {} -> {}\n", start, hex_str(read), result);
    };

    while (pos < data.size())
    {
        start = pos;

        // the longest match in the active table, fall back while there is none
        const Entry* entry = nullptr;
        size_t fallbacks = 0;
        for (;;)
        {
            entry = tables_[stack.top()].longest_match(&data[pos], data.size() - pos);
            if (entry || stack.depth() == 1) break;
            stack.fall_back();
            fallbacks++;
        }

        size_t size = entry ? entry->hex.size() + entry->params.size() : 1;

        // no match or a control code cut short by the end of the data
        if (!entry || pos + size > data.size())
        {
            text += raw_byte_str(data[pos]); // write as raw hex
            stats_.unmatched.push_back(pos);
            stack.reset();
            pos++;
            verbose_print(raw_byte_str(data[start]) + " (no match)");
            continue;
        }

        stats_.fallbacks += fallbacks;

        const uint8_t* params = data.data() + pos + entry->hex.size();
        pos += size;

        if (entry->type == EntryType::Normal)
        {
            text += entry->text;
            stack.count_match();
            verbose_print("\"" + entry->text + "\"");
            continue;
        }

        if (entry->type == EntryType::Switch)
        {
            stack.follow(*entry);
            stats_.switches++;
            stats_.switch_bytes += entry->hex.size();
            verbose_print("switch to table " + entry->label);
            if (!mark_table_switches) continue;
        }
        else
        {
            stack.count_match();
        }

        // [label], with the parameters if it's a control code
        std::string code = "[" + entry->label;
        for (size_t i = 0; i < entry->params.size(); i++)
        {
            code += " " + entry->params[i].format_value(params[i]);
        }
        code += "]";
        text += code;

        if (entry->type != EntryType::Switch) verbose_print(code);

        // write any line breaks after the label
        text.append(entry->linebreaks, '\n');
    }

    return true;
}

} // TXTCNV
