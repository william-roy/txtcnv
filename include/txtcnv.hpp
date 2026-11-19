// txtcnv - William Roy 2026
//
// Converts between binary data and text using table files (.tbl), as defined
// by "Table File Format.txt" (TBL 1.0 draft). Text collisions are resolved
// with an optimal path search (Level 2 compliance).
//
//     TXTCNV::TextCodec codec;
//     if (!codec.LoadTableFile("game.tbl")) 
//     { 
//          std::cerr << codec.Error() << "\n"; 
//     }
//
//     std::string text;
//     TXTCNV::Bytes data;
//
//     codec.Decode(text, data);    // hex-to-text (dumping)
//     codec.Encode(data, text);    // text-to-hex (inserting)

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <unordered_map>

namespace TXTCNV {

using Bytes = std::vector<uint8_t>;

enum class EntryType { 
    Normal, 
    Control, 
    End, 
    Switch 
};

// Format for how a control code parameter byte is written in text
// e.g. "palette=$%X" is {prefix = "palette=$", byte_format = 'X', suffix = ""}
struct ParamFormat {
    char byte_format = 'X';  // 'X' hex, 'D' decimal, 'B' binary
    std::string prefix;      // text before the placeholder
    std::string suffix;      // text after the placeholder

    // Returns a formatted string for a parameter byte
    // e.g. 0xA4 -> "palette=$A4"
    std::string format_value(uint8_t value) const;

    // Reads the parameter byte back out of text, e.g. "palette=$A4" -> 0xA4
    // Returns false if the text doesn't fit the pattern
    bool parse_value(std::string_view text, uint8_t& value) const;
};

constexpr int NO_LIMIT = 0;     // stay in the table until there's no match
constexpr int FALL_BACK = -1;   // go back to the table we switched from
constexpr int NO_TABLE = -1;    // a table index that doesn't refer to any table

// One line of a table file
struct Entry {
    EntryType type = EntryType::Normal;
    Bytes hex;

    // normal entries
    std::string text;           

    // non-normal entries
    std::string label;          // the label (or table id for switches)
    int linebreaks = 0;         // number of "\n"s after the label

    // control code
    std::vector<ParamFormat> params;  // one per parameter byte

    // table switches
    int match_limit = NO_LIMIT; // FALL_BACK, NO_LIMIT or a number of matches
    int target = NO_TABLE;      // index of the target table (set after loading)
};

class Table {
private:
    friend class TextCodec;

    bool add(Entry entry, std::string& error);
    bool resolve_switches(const std::unordered_map<std::string, int>& ids, 
                          std::string& error);

    std::string id_; // corresponds to "@TableID"
    
    // entries
    std::map<Bytes, Entry> entries_; // all entries, ordered by hex
    std::unordered_map<std::string, const Entry*> by_text_;  // normal
    std::unordered_map<std::string, const Entry*> by_label_; // non-normal
    std::vector<const Entry*> switches_;

    size_t max_hex_len_ = 0;
    size_t max_text_chars_ = 0;
    
public:
    explicit Table(std::string id) : id_(std::move(id)) {}

    // enable move operations
    Table(Table&&) = default;
    Table& operator=(Table&&) = default;

    // delete copy operations
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;


    // Returns table id
    const std::string& id() const { return id_; }

    // Returns number of entries
    size_t size() const { return entries_.size(); }

    // Returns number of entries of one type
    size_t count(EntryType type) const;

    // Returns length (in bytes) of the longest hex sequence
    size_t max_hex_len() const { return max_hex_len_; }

    // Returns the entry with exactly this hex sequence
    const Entry* find_hex(const Bytes& hex) const;

    // Returns the entry with the longest matching hex sequence
    const Entry* longest_match(const uint8_t* data, size_t size) const;

    // Returns true if there's an entry longer than prefix that starts with it
    bool has_longer_entry_starting_with(const Bytes& prefix) const;

    // Returns pointer to the normal entry used to insert this text
    const Entry* find_normal(const std::string& text) const;

    // Returns pointer to the non-normal entry with this label
    const Entry* find_non_normal(const std::string& label) const;

    // Returns table switch entries
    const std::vector<const Entry*>& switches() const { return switches_; }

    // Returns length (in UTF-8 characters) of the longest normal entry's text
    size_t max_text_chars() const { return max_text_chars_; }
};

struct Stats {
    size_t switches = 0;      // table switches read/written
    size_t switch_bytes = 0;  // number of bytes those switches take up
    size_t fallbacks = 0;     // number of times active table had no match
                              // and a table further down the stack did
    std::vector<size_t> unmatched;  // offsets of the bytes no table matched
                                    // (written as raw hex, "[$XX]")
};

class TextCodec {
private:
    bool fail(std::string message);

    std::vector<Table> tables_;
    std::string error_;
    Stats stats_;

public:
    // Loads in a table file (.tbl)
    // The first table in the file is the starting table
    bool LoadTableFile(const std::string& path);
    bool LoadTable(const std::string& content);

    // Hex-to-text
    bool Decode(std::string& text, const Bytes& data);

    // Text-to-hex
    bool Encode(Bytes& data, const std::string& text);

    // Returns the last error
    const std::string& Error() const { return error_; }

    // Returns the stats of the last Decode or Encode
    const Stats& LastStats() const { return stats_; }

    // Returns the currently loaded tables
    const std::vector<Table>& Tables() const { return tables_; }

    // If true, write "[TableID]" where table switches occur
    // Encoding accepts these markers but doesn't need them.
    bool mark_table_switches = false;
};

} // TXTCNV
