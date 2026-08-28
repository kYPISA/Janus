#pragma once

#include "janus_notation.hpp"
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace janus {

class NotationEncoder {
public:
    explicit NotationEncoder(const NotationConfig& config);

    void reset();
    void feed_data(const uint8_t* data, size_t length);
    void feed_data(const char* data, size_t length);
    void feed_token(NotationToken token);
    void feed_literal_string(const uint8_t* data, size_t length);
    void feed_literal_string(const char* data, size_t length);
    void feed_literal_integer(int32_t value);
    void feed_literal_float(float value);
    void feed_literal_double(double value);
    void feed_raw_bytes(const uint8_t* data, size_t length);
    void finalize();

    const uint8_t* get_encoded_data(size_t& out_length) const;
    const uint8_t* get_vocab_header(size_t& out_length) const;
    bool is_finalized() const;

private:
    NotationConfig config_;
    NotationStream body_stream_;
    NotationStream vocab_stream_;
    bool finalized_;
    bool adaptive_vocab_built_;
    std::unordered_map<std::string, uint32_t> frequency_map_;
    std::unordered_map<std::string, uint8_t> adaptive_lookup_;

    void collect_frequencies(const uint8_t* data, size_t length);
    void build_adaptive_vocab_if_needed();
    bool is_core_token_string(NotationToken token, const std::string& text) const;
    void emit_literal_string(const uint8_t* data, size_t length);
};

}

