// source_encoder.hpp

#pragma once

#include "txtcnv.hpp"

namespace TXTCNV {

struct SourceStats {
    size_t literals = 0;      // ENC("...") and ENC_BYTES("...") strings replaced
    size_t switches = 0;      // table switches written
    size_t switch_bytes = 0;  // number of bytes those switches take up
};

class SourceEncoder {
private:
    SourceStats stats;
public:
    bool encode(TXTCNV::TextCodec& codec, std::string& out,
                const std::string& source, std::string& error, bool verbose = false);
    void print_stats();
};

} // TXTCNV