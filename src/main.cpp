// txtcnv command line tool

#include "txtcnv.hpp"

#include <cctype>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

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

// verbose summary of a conversion
static void print_stats(const TXTCNV::Stats& stats, const std::string& command,
                        size_t in_size, size_t out_size, size_t input_offset)
{
    if (command == "encode" || command == "source")
    {
        if (command == "encode")
        {
            std::cerr << in_size << " bytes of text -> "
                      << out_size << " bytes of data\n";
        }
        else
        {
            std::cerr << stats.literals << " ENC() strings encoded ("
                      << in_size << " bytes of source -> "
                      << out_size << " bytes)\n";
        }

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
    size_t decode_len = std::string::npos; // npos = decode to the end of the input

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

    bool known_command = command == "encode" || command == "decode" ||
                         command == "source";

    if (!known_command || args.size() < 3 || args.size() > 4)
    {
        std::cerr <<
        "usage:\n"
        "  txtcnv [options] encode <table.tbl> <input text> [output file]\n"
        "  txtcnv [options] decode <table.tbl> <input file> [output file]\n"
        "  txtcnv [options] source <table.tbl> <source file> [output file]\n\n"

        "source replaces each ENC(\"...\") in C/C++ source code with a string\n"
        "literal of the encoded bytes, e.g. \"\\x11\\x2C\" /* Hi */\n\n"

        "options:\n"
        "  -v           verbose output (printed to stderr)\n"
        "  -s seek      start decoding at <seek> bytes offset\n"
        "  -l len       stop decoding after <len> bytes\n"
        "               (numbers can be decimal or hex, e.g. 4096 or 0x1000)\n"
        "  -m           mark decoded table switches in text as [TableID]\n"
        "               (table switches are worked out when encoding, so they aren't needed)\n\n"

        "Output goes to stdout unless an output file is given, so it can be\n"
        "piped or redirected. Use - as the input to read from stdin.\n\n"
        "Follows the Table File Format 1.0 draft with Level 2 compliance.\n";

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
        if (!codec.EncodeSource(output, input))
        {
            std::cerr << "ERROR: " << input_path << ": " << codec.Error() << "\n";
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

    if (verbose) 
    {
        print_stats(codec.LastStats(), command, input.size(), output.size(), decode_skip);
    }

    return 0;
}
