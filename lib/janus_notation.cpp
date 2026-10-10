#include "janus_notation.hpp"

namespace janus {

NotationException::NotationException(NotationError err, const std::string& message)
    : std::runtime_error(message), error_code_(err) {}

NotationError NotationException::error_code() const {
    return error_code_;
}

size_t VarintCodec::encode_unsigned(uint32_t value, uint8_t* output, size_t output_capacity) {
    size_t written = 0;
    while (value >= 0x80) {
        if (written >= output_capacity) {
            throw NotationException(NotationError::literal_too_large, "varint encode buffer exhausted");
        }
        output[written++] = static_cast<uint8_t>((value & 0x7F) | 0x80);
        value >>= 7;
    }
    if (written >= output_capacity) {
        throw NotationException(NotationError::literal_too_large, "varint encode buffer exhausted");
    }
    output[written++] = static_cast<uint8_t>(value);
    return written;
}

size_t VarintCodec::decode_unsigned(const uint8_t* input, size_t input_length, uint32_t& output_value) {
    if (!input && input_length) throw NotationException(NotationError::malformed_varint, "null varint input");
    output_value = 0;
    size_t shift = 0;
    size_t consumed = 0;
    while (consumed < input_length) {
        uint8_t byte = input[consumed++];
        if (shift == 28 && (byte & 0xF0) != 0)
            throw NotationException(NotationError::malformed_varint, "varint exceeds 32 bits");
        output_value |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return consumed;
        }
        shift += 7;
        if (shift >= 32) {
            throw NotationException(NotationError::malformed_varint, "varint exceeds 32 bits");
        }
    }
    throw NotationException(NotationError::truncated_stream, "varint truncated");
}

size_t VarintCodec::encode_signed(int32_t value, uint8_t* output, size_t output_capacity) {
    uint32_t zigzag = (static_cast<uint32_t>(value) << 1) ^ static_cast<uint32_t>(value >> 31);
    return encode_unsigned(zigzag, output, output_capacity);
}

size_t VarintCodec::decode_signed(const uint8_t* input, size_t input_length, int32_t& output_value) {
    uint32_t zigzag = 0;
    size_t consumed = decode_unsigned(input, input_length, zigzag);
    output_value = static_cast<int32_t>((zigzag >> 1) ^ (-static_cast<int32_t>(zigzag & 1)));
    return consumed;
}

void NotationStream::append_byte(uint8_t byte) {
    buffer_.push_back(byte);
}

void NotationStream::append_bytes(const uint8_t* data, size_t length) {
    buffer_.insert(buffer_.end(), data, data + length);
}

const std::vector<uint8_t>& NotationStream::data() const {
    return buffer_;
}

void NotationStream::clear() {
    buffer_.clear();
}

}
