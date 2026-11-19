// tests/test_src.c
// test file for the source command tests

// so this file also compiles without txtcnv
#define ENC(s) s
#define ENC_BYTES(s) s

// ENC("not encoded, because it's in a comment")
const char* plain = "ENC(\"not encoded either\")";

const char greeting[] = ENC("Hello");
const char color[] = ENC("[COLOR red=255 green=0 blue=128]");
const char joined[] = ENC("Good morning, "
                          "Crono!");

const char greeting_bytes[] = ENC_BYTES("Hello");
const char color_bytes[] = ENC_BYTES("[COLOR red=255 green=0 blue=128]");
