#pragma once

#include "janus_notation.hpp"
#include <vector>
#include <unordered_map>
#include <string>

namespace janus {

struct AdaptiveVocabEntry {
    uint8_t opcode;
    std::string text;
};

class NotationDecoder {
public:
    NotationDecoder();

    void set_vocab_header(const uint8_t* header, size_t length);
    void feed_encoded_data(const uint8_t* data, size_t length);
    void finalize();

    const uint8_t* get_decoded_data(size_t& out_length) const;
    bool has_error() const;
    NotationError get_error_code() const;

private:
    std::vector<uint8_t> pending_input_;
    std::vector<uint8_t> output_;
    std::unordered_map<uint8_t, std::string> adaptive_vocab_;
    NotationError error_code_;
    size_t input_position_;
    bool finalized_;

    void process_all_tokens();
    void process_next_token();
    size_t read_varint_unsigned(uint32_t& value);
    size_t read_varint_signed(int32_t& value);
    void emit_text(const std::string& text);
    void emit_byte(uint8_t byte);
    void emit_bytes(const uint8_t* data, size_t length);
    void set_error(NotationError err, const std::string& message);
};

}
