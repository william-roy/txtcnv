// source_encoder.cpp
//
// For encoding strings within C/C++ source code.
//
// ENC("...") is replaced with encoded nul-terminated string literal:
// print(ENC("Hello"));  ->  print("\x11\x28\x2F\x2F\x32" /* Hello */);
//
// ENC_BYTES("...") is replaced with encoded byte array:
// const char hi[] = ENC_BYTES("Hi");  ->  const char hi[] = {'\x11', '\x2C'} /* Hi */;


#include "source_encoder.hpp"

#include <iostream>
#include <algorithm>
#include <regex>
#include <format>

namespace TXTCNV {

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

// verbose summary of the source command
void SourceEncoder::print_stats()
{
    std::cerr << stats.literals << " ENC()/ENC_BYTES() strings encoded\n";
    std::cerr << "Table switches written: " << stats.switches
              << " (" << stats.switch_bytes << " bytes)\n";
}


bool SourceEncoder::encode(TXTCNV::TextCodec& codec, std::string& out,
                          const std::string& source, std::string& error, bool verbose)
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

        std::string encoded = as_bytes ? c_byte_array(data) : c_hex_literal(data);

        out.append(copied_up_to, match[0].first);
        out += encoded + " " + c_comment(text);

        // keep the line numbers the same as in the source
        out.append(std::count(match[0].first, match[0].second, '\n'), '\n');
        copied_up_to = match[0].second;

        // verbose output
        if (verbose)
        {
            std::string shown = std::regex_replace(text, std::regex("\n"), "\\n");
            std::cerr << "line " << line << ": " 
                      << match[1] << "(\"" << shown << "\") -> " << encoded << "\n";
        }
    }

    out.append(copied_up_to, source.cend());
    return true;
}


} // TXTCNV

