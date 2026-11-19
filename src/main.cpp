// txtcnv command line tool

#include "txtcnv.hpp"

#include <cctype>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <regex>
#include <format>


#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

// reads a whole file, or stdin if the path is "-"
static bool read_input(const std::string& path, std::string& out)
{
    std::stringstream ss;

    if (path == "-")
    {
        ss << std::cin.rdbuf();
    }
    else
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        ss << file.rdbuf();
    }

    out = ss.str();
    return true;
}

// writes to a file, or to stdout if the path is empty
static bool write_output(const std::string& path, const char* data, size_t size)
{
    if (path.empty())
    {
        std::cout.write(data, size);
        std::cout.flush();
        return static_cast<bool>(std::cout);
    }

    std::ofstream file(path, std::ios::binary);
    return static_cast<bool>(file.write(data, size));
}

// reads a byte count or offset, in decimal or hex (with a 0x prefix)
static bool parse_size(const std::string& text, size_t& out)
{
    // require a digit first, so stoull doesn't accept "-5"
    if (text.empty() || !std::isdigit(static_cast<unsigned char>(text[0])))
    {
        return false;
    } 

    int base = (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) ? 16 : 10;

    try
    {
        size_t used = 0;
        out = std::stoull(text, &used, base);
        return used == text.size(); // reject trailing junk like "12abc"
    }
    catch (...)
    {
        return false;
    }
}

// Source files ----------------------------------------------------------------
//
// For encoding strings within C/C++ source code.
//
// ENC("...") is replaced with encoded nul-terminated string literal:
// print(ENC("Hello"));  ->  print("\x11\x28\x2F\x2F\x32" /* Hello */);
//
// ENC_BYTES("...") is replaced with encoded byte array:
// const char hi[] = ENC_BYTES("Hi");  ->  const char hi[] = {'\x11', '\x2C'} /* Hi */;

static const std::regex SOURCE_PATTERN(
    R"(//[^\n]*)"               // comment
    R"(|/\*[\s\S]*?\*/)"        // block comment
    R"(|"(?:[^"\\\n]|\\.)*")"   // string
    R"(|'(?:[^'\\\n]|\\.)*')"   // character
    R"(|\b(ENC_BYTES|ENC)\s*\(\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+)\))" // macro
    R"(|\b(ENC_BYTES|ENC)\s*\(\s*")"    // malformed macro
);

// one string literal
// (pattern ends in )" so it needs R"re(...)re" instead of R"(...)")
static const std::regex STRING_LITERAL(R"re("((?:[^"\\\n]|\\.)*)")re");

struct SourceStats {
    size_t literals = 0;      // ENC("...") and ENC_BYTES("...") strings replaced
    size_t switches = 0;      // table switches written
    size_t switch_bytes = 0;  // number of bytes those switches take up
};

// adds a string literal's contents to text, replacing the escapes
static bool unescape(const std::string& str, std::string& text, std::string& error)
{
    for (size_t i = 0; i < str.size(); i++)
    {
        if (str[i] != '\\')
        {
            text += str[i];
            continue;
        }

        char escaped = str[++i]; // the regex makes sure a character follows

        switch (escaped)
        {
            case '\\': 
            case '"': 
            case '\'': 
                text += escaped; 
                break;
            case 'n': 
                text += '\n'; 
                break;
            case 't': 
                text += '\t'; 
                break;
            default:
                error = std::string("unsupported escape '\\") + escaped +
                        "' (write raw bytes as [$XX])";
                return false;
        }
    }
    return true;
}

// returns bytes as a string literal, e.g. "\x11\x28"
static std::string c_hex_literal(const TXTCNV::Bytes& bytes)
{
    std::string str = "\"";
    for (uint8_t byte : bytes)
    {
        str += std::format("\\x{:02X}", byte);
    }
    return str + "\"";
}

// returns bytes as an array initializer, e.g. {'\x11', '\x28'}
// (char literals, so values over 0x7F fit in a char array in C++)
static std::string c_byte_array(const TXTCNV::Bytes& bytes)
{
    std::string str = "{";
    for (size_t i = 0; i < bytes.size(); i++)
    {
        if (i > 0) str += ", ";
        str += std::format("'\\x{:02X}'", bytes[i]);
    }
    return str + "}";
}

// returns text as a C comment, e.g. /* Hello */
// (newlines are written as \n, and "*/" as "*\/" so it doesn't end the comment)
static std::string c_comment(const std::string& text)
{
    std::string str = std::regex_replace(text, std::regex("\n"), "\\n");
    str = std::regex_replace(str, std::regex(R"(\*/)"), R"(*\/)");
    return "/* " + str + " */";
}

// replaces every ENC("...") and ENC_BYTES("...") in source with its encoding
static bool encode_source(TXTCNV::TextCodec& codec, std::string& out,
                          const std::string& source, SourceStats& stats,
                          std::string& error)
{
    out.clear();
    stats = {};
    auto copied_up_to = source.cbegin();

    std::sregex_iterator it(source.begin(), source.end(), SOURCE_PATTERN), end;
    for (; it != end; ++it)
    {
        const std::smatch& match = *it;
        bool is_macro = match[1].matched;
        bool is_malformed = match[3].matched;

        if (!is_macro && !is_malformed) continue;

        size_t line = 1 + std::count(source.cbegin(), match[0].first, '\n');
        auto line_error = [&](const std::string& message)
        {
            error = "line " + std::to_string(line) + ": " + message;
            return false;
        };

        if (is_malformed)
        {
            return line_error("expected \"...\" strings and then ')' after " +
                              match[3].str() + "(");
        }

        // join strings, e.g. ENC("Good " "morning")
        std::string text;
        const std::string strings = match[2].str();
        std::sregex_iterator s(strings.begin(), strings.end(), STRING_LITERAL);

        for (; s != end; ++s)
        {
            std::string escape_error;
            if (!unescape((*s)[1].str(), text, escape_error))
            {
                return line_error(escape_error);
            }
        }

        TXTCNV::Bytes data;
        if (!codec.Encode(data, text)) return line_error(codec.Error());

        // {} isn't a valid array initializer in C before C23
        bool as_bytes = (match[1] == "ENC_BYTES");
        if (as_bytes && data.empty())
        {
            return line_error("ENC_BYTES(\"\") would be an empty array");
        }

        stats.literals++;
        stats.switches += codec.LastStats().switches;
        stats.switch_bytes += codec.LastStats().switch_bytes;

        out.append(copied_up_to, match[0].first);
        out += (as_bytes ? c_byte_array(data) : c_hex_literal(data)) + " " +
               c_comment(text);

        // keep the line numbers the same as in the source
        out.append(std::count(match[0].first, match[0].second, '\n'), '\n');
        copied_up_to = match[0].second;
    }

    out.append(copied_up_to, source.cend());
    return true;
}

// Command line ----------------------------------------------------------------

// verbose summary of the source command
static void print_source_stats(const SourceStats& stats, size_t in_size,
                               size_t out_size)
{
    std::cerr << stats.literals << " ENC()/ENC_BYTES() strings encoded ("
              << in_size << " bytes of source -> "
              << out_size << " bytes)\n";
    std::cerr << "Table switches written: " << stats.switches
              << " (" << stats.switch_bytes << " bytes)\n";
}

// verbose summary of an encode or decode
static void print_stats(const TXTCNV::Stats& stats, const std::string& command,
                        size_t in_size, size_t out_size, size_t input_offset)
{
    if (command == "encode")
    {
        std::cerr << in_size << " bytes of text -> "
                  << out_size << " bytes of data\n";
        std::cerr << "Table switches written: " << stats.switches
                  << " (" << stats.switch_bytes << " bytes)\n";
        return;
    }

    std::cerr << in_size << " bytes of data -> " 
              << out_size << " bytes of text\n";
    std::cerr << "Table switches read: " << stats.switches
              << " (" << stats.switch_bytes << " bytes), fallbacks: " 
              << stats.fallbacks << "\n";

    std::cerr << "Unmatched bytes: " << stats.unmatched.size() << "\n";

    // where they are, so the table (or the range being dumped) can be fixed
    const size_t max_shown = 20;
    for (size_t i = 0; i < stats.unmatched.size() && i < max_shown; i++)
    {
        std::cerr << "  at offset 0x" << std::hex 
                  << std::uppercase << input_offset + stats.unmatched[i]
                  << std::dec << "\n";
    }

    if (stats.unmatched.size() > max_shown)
    {
        std::cerr << "  ... and " 
                  << stats.unmatched.size() - max_shown << " more\n";
    }
}

int main(int argc, char* argv[])
{
    #ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);

    // stop Windows translating line endings and corrupting binary data
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    #endif

    TXTCNV::TextCodec codec;
    
    std::vector<std::string> args;
    bool verbose = false;
    size_t decode_skip = 0;
    size_t decode_len = std::string::npos; // npos = decode to end of the input

    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];

        if (arg == "-v") 
        {
            verbose = true;
        }
        else if (arg == "-m") 
        { 
            codec.mark_table_switches = true; 
        }
        else if (arg == "-s" || arg == "-l")
        {
            size_t& value = (arg == "-s") ? decode_skip : decode_len;

            if (i + 1 >= argc || !parse_size(argv[++i], value))
            {
                std::cerr << "ERROR: " << arg << 
                             " needs a number of bytes (e.g. 256 or 0xFF)\n";
                return 1;
            }
        }
        else 
        { 
            args.push_back(arg); 
        }
    }

    const std::string command = args.empty() ? "" : args[0];

    bool known_command = command == "encode" || 
                         command == "decode" ||
                         command == "source";

    if (!known_command || args.size() < 3 || args.size() > 4)
    {
        std::cerr <<
        "usage:\n"
        "  txtcnv [options] encode <table.tbl> <input text> [output file]\n"
        "  txtcnv [options] decode <table.tbl> <input file> [output file]\n"
        "  txtcnv [options] source <table.tbl> <source file> [output file]\n\n"

        "options:\n"
        "  -v           verbose output (printed to stderr)\n"
        "  -s seek      start decoding at <seek> bytes offset\n"
        "  -l len       stop decoding after <len> bytes\n"
        "               (numbers can be decimal or hex, e.g. 4096 or 0x1000)\n"
        "  -m           mark decoded table switches in text as [TableID]\n"

        "Output goes to stdout unless an output file is given, so it can be\n"
        "piped or redirected. Use - as the input to read from stdin.\n\n"
        "Follows the Table File Format 1.0 draft with Level 2 compliance.\n"
        
        "The source command encodes the following in C/C++ source code:\n"
        "ENC(\"...\") = nul-terminated string, e.g. \"\\x11\\x2C\" /* Hi */\n"
        "ENC_BYTES(\"...\") = byte array, e.g. {'\\x11', '\\x2C'} /* Hi */\n\n";

        return 1;
    }

    const std::string& table_path = args[1];
    const std::string& input_path = args[2];
    const std::string output_path = (args.size() == 4) ? args[3] : "";

    if (!codec.LoadTableFile(table_path))
    {
        std::cerr << "ERROR: " << table_path << ": " << codec.Error() << "\n";
        return 1;
    }

    if (verbose)
    {
        for (const TXTCNV::Table& table : codec.Tables())
        {
            std::cerr << "Table '" << table.id() << "': " << table.size() << " entries ("
                      << table.count(TXTCNV::EntryType::Normal) << " normal, "
                      << table.count(TXTCNV::EntryType::Control) << " control, "
                      << table.count(TXTCNV::EntryType::End) << " end, "
                      << table.count(TXTCNV::EntryType::Switch) << " switch), longest hex "
                      << table.max_hex_len() << " byte(s)\n";
        }
    }

    std::string input;
    if (!read_input(input_path, input))
    {
        std::cerr << "ERROR: could not open '" << input_path << "'\n";
        return 1;
    }

    std::string output; // the converted output (text or binary)
    SourceStats source_stats;

    if (command == "encode")
    {
        TXTCNV::Bytes data;
        if (!codec.Encode(data, input))
        {
            std::cerr << "ERROR: " << codec.Error() << "\n";
            return 1;
        }
        output.assign(data.begin(), data.end());
    }
    else if (command == "source")
    {
        std::string error;
        if (!encode_source(codec, output, input, source_stats, error))
        {
            std::cerr << "ERROR: " << input_path << ": " << error << "\n";
            return 1;
        }
    }
    else // decode
    {
        if (decode_skip > input.size())
        {
            std::cerr << "ERROR: -s " << decode_skip << " is past the end of the input ("
                      << input.size() << " bytes)\n";
            return 1;
        }

        // keep only the chunk picked with -s and -l
        input = input.substr(decode_skip, decode_len);

        if (!codec.Decode(output, TXTCNV::Bytes(input.begin(), input.end())))
        {
            std::cerr << "ERROR: " << codec.Error() << "\n";
            return 1;
        }
    }

    if (!write_output(output_path, output.data(), output.size()))
    {
        std::cerr << "ERROR: could not write "
                  << (output_path.empty() ? "to stdout" : "'" + output_path + "'") << "\n";
        return 1;
    }

    if (verbose && command == "source")
    {
        print_source_stats(source_stats, input.size(), output.size());
    }
    else if (verbose)
    {
        print_stats(codec.LastStats(), command, input.size(), output.size(), decode_skip);
    }

    return 0;
}
