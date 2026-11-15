# txtcnv

Converts between binary data and text using UTF-8 encoded table files (.tbl). 
For use in translation, homebrew, hacking, reverse engineering, etc.

Mostly follows Nightcrawler's **Table File Format 1.0** (see
[Table File Format.txt](Table%20File%20Format.txt)) with Level 2 compliance. 
Text collisions are resolved with an optimal path search, so the encoder always 
writes the fewest bytes possible, including any table switches it needs.

The spec is a bit ambiguous in some areas, so I took some liberties based on 
what made sense to me. Some caveats apply:

- **The starting table** is the first table in the file. Entries before the
  first `@TableID` line go in a table named `default`.
- **Line breaks** in the text are ignored when inserting, so be warned an 
  entry can match across one. If you need line breaks preserved, use a control 
  code like `$F1=[BR]\n`.
- **Raw bytes (`[$XX]`)** are only inserted where the decoder would also read
  them as raw bytes. If a table on the stack would read the byte as an entry,
  encoding fails instead of writing data that decodes differently.
- **Labels** must be unique within a table, duplicate *text* in normal entries 
  is allowed (shortest hex wins).
- **A -1 switch in the starting table** switches indefinitely, because there's
  no table to fall back to, making it functionally the same as a 0 switch.

The encoder never writes bytes that would decode to different text, so
encode → decode always gives back the original text (sans line breaks).

## Library Usage

You can simply drop txtcnv.hpp and txtcnv.cpp into your project, there are no 
dependencies (aside from C++20).

```cpp
TXTCNV::TextCodec codec;
if (!codec.LoadTableFile("game.tbl")) std::cerr << codec.Error() << "\n";

std::string text;
TXTCNV::Bytes data;

// Binary to text
if (!codec.Decode(text, data)) std::cerr << codec.Error() << "\n";

// Text to binary
if (!codec.Encode(data, text)) std::cerr << codec.Error() << "\n";

```

## Command Line Usage

First, build the command line tool (see below). Then run:

```
txtcnv [options] encode <table.tbl> <input text> [output file]
txtcnv [options] decode <table.tbl> <input file> [output file]

  -v           verbose output (printed to stderr)
  -s seek      start decoding at <seek> bytes offset
  -l len       stop decoding after <len> bytes
               (numbers can be decimal or hex, e.g. 4096 or 0x1000)
  -m           mark decoded table switches in text as [TableID]
               (table switches are worked out when encoding, so they aren't needed)

The output goes to stdout unless an output file is given, so it can be
piped or redirected. Use - as the input to read from stdin.
Follows the Table File Format 1.0 with Level 2 compliance.
```

For example:

```
# Decode the contents of script.bin into script.txt 
txtcnv decode game.tbl script.bin > script.txt

# Pipe the encoded contents of script.txt to xxd
txtcnv encode game.tbl script.txt | xxd

# Encode the piped-in contents of script.txt into script.bin
cat script.txt | txtcnv encode game.tbl - script.bin

# Encode the piped in string and pipe it to stdout
echo "Good morning, Crono!" | txtcnv encode game.tbl - | xxd
```

## Building

```
make        # build ./bin/txtcnv
make test   # build and run the tests
```

---

## References

- [Nightcrawler](https://transcorp.romhacking.net)
- [TCRF Data Crystal TBL files](https://datacrystal.tcrf.net/wiki/Category:TBL_Files)
- [retrotool](https://github.com/danielburgess/SFCRetroTools)
- [Zophar's Domain](https://www.zophar.net/documents/transdocs/table-files.html)