// tests/tests.cpp

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <algorithm>

#include "txtcnv.hpp"

using TXTCNV::Bytes;
using TXTCNV::TextCodec;

static int n_tests = 0;
static int n_failed = 0;

#define CHECK(cond) \
    do { \
        n_tests++; \
        if (!(cond)) { \
            n_failed++; \
            std::cerr << "FAILED (line " << __LINE__ << "): " #cond "\n"; \
        } \
    } while (0)

// test_01.tbl: three tables (HIRA, KATA, KANJI) with table switches
// test_02.tbl: letters, numbers, control codes, an end token, and collisions
static const char* SWITCH_TBL = "tests/test_01.tbl";
static const char* MAIN_TBL   = "tests/test_02.tbl";

// test_src.c: C source with ENC("...") strings to encode with test_02.tbl
static const char* SOURCE_FILE = "tests/test_src.c";

// source encoding is part of the command line tool, so it's tested by running
// it (the makefile builds it before the tests). Its output goes in obj/
#ifdef _WIN32
static const std::string TXTCNV_EXE = "bin\\txtcnv.exe";
#else
static const std::string TXTCNV_EXE = "./bin/txtcnv";
#endif
static const char* SOURCE_OUT = "obj/test_src_out.c";
static const char* SOURCE_LOG = "obj/test_src_log.txt";
static const char* EMPTY_SRC  = "obj/test_src_empty.c";

static std::string read_file(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream content;
    content << file.rdbuf();
    return content.str();
}

// runs "txtcnv -v source <table> <source> SOURCE_OUT", returns true on success
static bool run_source_command(const char* table, const char* source)
{
    std::string cmd = TXTCNV_EXE + " -v source " + table + " " + source + " " +
                      SOURCE_OUT + " 2> " + SOURCE_LOG;
    return std::system(cmd.c_str()) == 0;
}

static std::string decode(TextCodec& codec, const Bytes& data)
{
    std::string text;
    codec.Decode(text, data);
    return text;
}

static Bytes encode(TextCodec& codec, const std::string& text)
{
    Bytes data;
    codec.Encode(data, text);
    return data;
}

int main()
{
    TextCodec codec;

    // Loading -----------------------------------------------------------------
    {
        CHECK(codec.LoadTableFile(MAIN_TBL));
        CHECK(codec.Tables().size() == 2);
    }

    // Decoding ----------------------------------------------------------------
    {
        codec.LoadTableFile(MAIN_TBL);

        // normal entries
        CHECK(decode(codec, {0x11, 0x28, 0x2F, 0x2F, 0x32}) == "Hello");

        // non-normal: control code with parameters and line breaks
        CHECK(decode(codec, {0xFC, 0xFF, 0x00, 0x80}) == "[COLOR red=255 green=0 blue=128]\n\n");

        // end token
        CHECK(decode(codec, {0xFF}) == "[END]\n\n");

        // the longest hex sequence wins (FF12 over FF)
        CHECK(decode(codec, {0xFF, 0x12}) == "Five");

        // raw hex (no matching entry)
        CHECK(decode(codec, {0x11, 0x50}) == "H[$50]");

        // table switches
        codec.LoadTableFile(SWITCH_TBL);
        CHECK(decode(codec, {0xF8, 0x00, 0x01, 0x02, 0xF9, 0x01, 0x03}) == "アイウ意[PlayerName]");
    }

    // Encoding ----------------------------------------------------------------
    {
        codec.LoadTableFile(MAIN_TBL);

        CHECK(encode(codec, "Hello") == Bytes({0x11, 0x28, 0x2F, 0x2F, 0x32}));
        CHECK(encode(codec, "[COLOR red=255 green=0 blue=128]") == Bytes({0xFC, 0xFF, 0x00, 0x80}));

        // text collisions: the shortest encoding is used (FF12 FF13)
        CHECK(encode(codec, "FiveSixSeven") == Bytes({0xFF, 0x12, 0xFF, 0x13}));

        // text that isn't in the table can't be encoded
        Bytes data;
        CHECK(!codec.Encode(data, "Hello@"));

        // confirm table switches are added where they're needed
        codec.LoadTableFile(SWITCH_TBL);
        CHECK(encode(codec, "アイウ意[PlayerName]") == Bytes({0xF8, 0x00, 0x01, 0x02, 0xF9, 0x01, 0x03}));
    }

    // Round trips -------------------------------------------------------------

    // text -> bytes -> text
    {
        codec.LoadTableFile(MAIN_TBL);

        std::string text = "Crono... Crono!"
                           "[COLOR red=255 green=0 blue=0]\n\n"
                           "Good morning, Crono![END]\n\n";
        Bytes data;
        std::string back;
        CHECK(codec.Encode(data, text));
        CHECK(codec.Decode(back, data));
        CHECK(back == text);
    }

    // bytes -> text -> bytes
    {
        codec.LoadTableFile(SWITCH_TBL);

        Bytes data = {
            0x00, 0xF8, 0x00, 0x01, 
            0xF9, 0x01, 0x03
        };
        std::string text;
        Bytes back;
        CHECK(codec.Decode(text, data));
        CHECK(codec.Encode(back, text));
        CHECK(back == data);
    }

    // Source files ------------------------------------------------------------
    {
        CHECK(run_source_command(MAIN_TBL, SOURCE_FILE));

        std::string source = read_file(SOURCE_FILE);
        std::string out = read_file(SOURCE_OUT);

        // only the three ENC("...") and two ENC_BYTES("...") strings
        CHECK(read_file(SOURCE_LOG).find("5 ENC()/ENC_BYTES() strings encoded")
              != std::string::npos);

        CHECK(out.find("greeting[] = \"\\x11\\x28\\x2F\\x2F\\x32\" "
                       "/* Hello */;")
                       != std::string::npos);
        CHECK(out.find("color[] = \"\\xFC\\xFF\\x00\\x80\" "
                       "/* [COLOR red=255 green=0 blue=128] */;")
                       != std::string::npos);

        // ENC_BYTES gives a byte array (no NUL byte at the end)
        CHECK(out.find("greeting_bytes[] = {'\\x11', '\\x28', '\\x2F', '\\x2F', '\\x32'} "
                       "/* Hello */;")
                       != std::string::npos);
        CHECK(out.find("color_bytes[] = {'\\xFC', '\\xFF', '\\x00', '\\x80'} "
                       "/* [COLOR red=255 green=0 blue=128] */;")
                       != std::string::npos);

        // line numbers stay the same
        CHECK(std::count(out.begin(), out.end(), '\n') ==
              std::count(source.begin(), source.end(), '\n'));

        // an empty ENC_BYTES("") can't be a C array
        std::ofstream(EMPTY_SRC) << "const char empty[] = ENC_BYTES(\"\");";
        CHECK(!run_source_command(MAIN_TBL, EMPTY_SRC));
    }

    std::cout << (n_tests - n_failed) << "/" << n_tests << " checks passed\n";

    return n_failed ? 1 : 0;
}
