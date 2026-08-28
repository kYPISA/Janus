#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <stdexcept>

namespace janus {

enum class NotationToken : uint8_t {
    null_value = 0x00,
    boolean_true = 0x01,
    boolean_false = 0x02,
    object_begin = 0x03,
    object_end = 0x04,
    array_begin = 0x05,
    array_end = 0x06,
    key_value_separator = 0x07,
    item_separator = 0x08,
    tuple_begin = 0x09,
    tuple_end = 0x0A,
    generic_begin = 0x0B,
    generic_end = 0x0C,
    assignment = 0x0D,
    statement_terminator = 0x0E,
    explicit_newline = 0x0F,
    indent = 0x10,
    dedent = 0x11,

    literal_zero = 0x20,
    literal_one = 0x21,
    literal_minus_one = 0x22,
    literal_empty_string = 0x23,
    literal_empty_array = 0x24,
    literal_empty_object = 0x25,
    literal_two = 0x26,
    literal_three = 0x27,
    literal_four = 0x28,
    literal_five = 0x29,
    literal_six = 0x2A,
    literal_seven = 0x2B,
    literal_eight = 0x2C,
    literal_nine = 0x2D,
    literal_ten = 0x2E,
    literal_eleven = 0x2F,

    key_id = 0x40,
    key_name = 0x41,
    key_type = 0x42,
    key_value = 0x43,
    key_data = 0x44,
    key_status = 0x45,
    key_error = 0x46,
    key_result = 0x47,
    key_timestamp = 0x48,
    key_version = 0x49,
    key_config = 0x4A,
    key_index = 0x4B,
    key_length = 0x4C,
    key_size = 0x4D,
    key_count = 0x4E,
    key_total = 0x4F,

    keyword_class = 0x80,
    keyword_function = 0x81,
    keyword_return = 0x82,
    keyword_if = 0x83,
    keyword_else = 0x84,
    keyword_for = 0x85,
    keyword_while = 0x86,
    keyword_import = 0x87,
    keyword_include = 0x88,
    keyword_public = 0x89,
    keyword_private = 0x8A,
    keyword_static = 0x8B,
    keyword_const = 0x8C,
    keyword_var = 0x8D,
    keyword_let = 0x8E,
    keyword_true = 0x8F,
    keyword_false = 0x90,
    keyword_null = 0x91,
    keyword_undefined = 0x92,
    keyword_this = 0x93,
    keyword_self = 0x94,

    literal_string = 0xF0,
    literal_integer = 0xF1,
    literal_float = 0xF2,
    literal_double = 0xF3,
    reference = 0xF4,
    repetition = 0xF5,

    adaptive_begin = 0xC0,
    adaptive_end = 0xEF,

    core_end = 0xBF
};

enum class WhitespaceMode {
    preserve,
    normalize,
    ignore
};

struct NotationConfig {
    bool enable_adaptive_vocab = false;
    size_t max_adaptive_tokens = 48;
    WhitespaceMode whitespace_mode = WhitespaceMode::ignore;
};

enum class NotationError {
    ok,
    invalid_token,
    unbalanced_container,
    truncated_stream,
    adaptive_vocab_overflow,
    reference_out_of_bounds,
    malformed_varint,
    literal_too_large,
    already_finalized
};

class NotationException : public std::runtime_error {
public:
    explicit NotationException(NotationError err, const std::string& message);
    NotationError error_code() const;

private:
    NotationError error_code_;
};

class VarintCodec {
public:
    static size_t encode_unsigned(uint32_t value, uint8_t* output, size_t output_capacity);
    static size_t decode_unsigned(const uint8_t* input, size_t input_length, uint32_t& output_value);
    static size_t encode_signed(int32_t value, uint8_t* output, size_t output_capacity);
    static size_t decode_signed(const uint8_t* input, size_t input_length, int32_t& output_value);
};

class NotationStream {
public:
    void append_byte(uint8_t byte);
    void append_bytes(const uint8_t* data, size_t length);
    const std::vector<uint8_t>& data() const;
    void clear();

private:
    std::vector<uint8_t> buffer_;
};

}
