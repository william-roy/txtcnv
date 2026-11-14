// tests/tests.cpp

#include <iostream>
#include <string>

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

        Bytes data = {0x00, 0xF8, 0x00, 0x01, 0xF9, 0x01, 0x03};
        std::string text;
        Bytes back;
        CHECK(codec.Decode(text, data));
        CHECK(codec.Encode(back, text));
        CHECK(back == data);
    }

    std::cout << (n_tests - n_failed) << "/" << n_tests << " checks passed\n";

    return n_failed ? 1 : 0;
}
