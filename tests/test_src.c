// tests/test_src.c
// test file for the EncodeSource tests

#define ENC(s) s // so this file also compiles without txtcnv

// ENC("not encoded, because it's in a comment")

const char greeting[] = ENC("Hello");
const char color[] = ENC("[COLOR red=255 green=0 blue=128]");
const char joined[] = ENC("Good morning, "
                          "Crono!");
const char* plain = "ENC(\"not encoded either\")";
