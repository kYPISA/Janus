#include "janus_notation_decoder.hpp"
#include <cstring>

namespace janus {

NotationDecoder::NotationDecoder()
    : error_code_(NotationError::ok), input_position_(0), finalized_(false) {}

void NotationDecoder::set_vocab_header(const uint8_t* header, size_t length) {
    adaptive_vocab_.clear();
    if (length == 0 || header == nullptr) {
        return;
    }
    size_t position = 0;
    uint32_t count = 0;
    position += VarintCodec::decode_unsigned(header + position, length - position, count);
    for (uint32_t i = 0; i < count; ++i) {
        if (position >= length) {
            set_error(NotationError::truncated_stream, "adaptive vocab truncated at opcode");
            return;
        }
        uint8_t opcode = header[position++];
        uint32_t text_length = 0;
        size_t consumed = VarintCodec::decode_unsigned(header + position, length - position, text_length);
        position += consumed;
        if (position + text_length > length) {
            set_error(NotationError::truncated_stream, "adaptive vocab truncated at text");
            return;
        }
        adaptive_vocab_[opcode] = std::string(
            reinterpret_cast<const char*>(header + position), text_length);
        position += text_length;
    }
}

void NotationDecoder::feed_encoded_data(const uint8_t* data, size_t length) {
    if (finalized_) {
        set_error(NotationError::already_finalized, "decoder already finalized");
        return;
    }
    if (data != nullptr && length > 0) {
        pending_input_.insert(pending_input_.end(), data, data + length);
    }
}

void NotationDecoder::finalize() {
    if (finalized_) {
        return;
    }
    finalized_ = true;
    process_all_tokens();
}

const uint8_t* NotationDecoder::get_decoded_data(size_t& out_length) const {
    out_length = output_.size();
    if (out_length == 0) {
        return nullptr;
    }
    return output_.data();
}

bool NotationDecoder::has_error() const {
    return error_code_ != NotationError::ok;
}

NotationError NotationDecoder::get_error_code() const {
    return error_code_;
}

void NotationDecoder::process_all_tokens() {
    input_position_ = 0;
    while (!has_error() && input_position_ < pending_input_.size()) {
        process_next_token();
    }
}

void NotationDecoder::process_next_token() {
    if (input_position_ >= pending_input_.size()) {
        set_error(NotationError::truncated_stream, "unexpected end of token stream");
        return;
    }
    uint8_t opcode = pending_input_[input_position_++];

    if (opcode <= static_cast<uint8_t>(NotationToken::core_end)) {
        if (opcode == static_cast<uint8_t>(NotationToken::literal_minus_one)) {
            emit_text("-1");
            return;
        }
        if (opcode >= static_cast<uint8_t>(NotationToken::literal_zero) &&
            opcode <= static_cast<uint8_t>(NotationToken::literal_eleven)) {
            char buffer[4];
            int written = std::snprintf(buffer, sizeof(buffer), "%u", opcode - static_cast<uint8_t>(NotationToken::literal_zero));
            emit_text(std::string(buffer, written));
            return;
        }
        emit_byte(opcode);
        return;
    }
    if (opcode >= static_cast<uint8_t>(NotationToken::adaptive_begin) &&
        opcode <= static_cast<uint8_t>(NotationToken::adaptive_end)) {
        auto it = adaptive_vocab_.find(opcode);
        if (it == adaptive_vocab_.end()) {
            set_error(NotationError::invalid_token, "adaptive opcode without vocab entry");
            return;
        }
        emit_text(it->second);
        return;
    }

    switch (static_cast<NotationToken>(opcode)) {
        case NotationToken::literal_string: {
            uint32_t length = 0;
            input_position_ += read_varint_unsigned(length);
            if (has_error()) return;
            if (input_position_ + length > pending_input_.size()) {
                set_error(NotationError::truncated_stream, "literal string truncated");
                return;
            }
            emit_bytes(pending_input_.data() + input_position_, length);
            input_position_ += length;
            break;
        }
        case NotationToken::literal_integer: {
            int32_t value = 0;
            input_position_ += read_varint_signed(value);
            if (has_error()) return;
            char buffer[16];
            int written = std::snprintf(buffer, sizeof(buffer), "%d", value);
            if (written < 0 || written >= static_cast<int>(sizeof(buffer))) {
                set_error(NotationError::literal_too_large, "integer literal too large");
                return;
            }
            emit_text(std::string(buffer, written));
            break;
        }
        case NotationToken::literal_float: {
            if (input_position_ + 4 > pending_input_.size()) {
                set_error(NotationError::truncated_stream, "float literal truncated");
                return;
            }
            float value = 0.0f;
            std::memcpy(&value, pending_input_.data() + input_position_, sizeof(value));
            input_position_ += sizeof(value);
            char buffer[32];
            int written = std::snprintf(buffer, sizeof(buffer), "%g", value);
            if (written < 0 || written >= static_cast<int>(sizeof(buffer))) {
                set_error(NotationError::literal_too_large, "float literal too large");
                return;
            }
            emit_text(std::string(buffer, written));
            break;
        }
        case NotationToken::literal_double: {
            if (input_position_ + 8 > pending_input_.size()) {
                set_error(NotationError::truncated_stream, "double literal truncated");
                return;
            }
            double value = 0.0;
            std::memcpy(&value, pending_input_.data() + input_position_, sizeof(value));
            input_position_ += sizeof(value);
            char buffer[64];
            int written = std::snprintf(buffer, sizeof(buffer), "%g", value);
            if (written < 0 || written >= static_cast<int>(sizeof(buffer))) {
                set_error(NotationError::literal_too_large, "double literal too large");
                return;
            }
            emit_text(std::string(buffer, written));
            break;
        }
        case NotationToken::reference: {
            uint32_t offset = 0;
            input_position_ += read_varint_unsigned(offset);
            if (has_error()) return;
            set_error(NotationError::invalid_token, "references not implemented in module 2.1");
            break;
        }
        case NotationToken::repetition: {
            uint32_t count = 0;
            input_position_ += read_varint_unsigned(count);
            if (has_error()) return;
            if (count == 0) {
                return;
            }
            if (input_position_ >= pending_input_.size()) {
                set_error(NotationError::truncated_stream, "repetition token truncated");
                return;
            }
            size_t output_start = output_.size();
            process_next_token();
            if (has_error()) return;
            size_t output_end = output_.size();
            size_t segment_length = output_end - output_start;
            for (uint32_t i = 1; i < count; ++i) {
                emit_bytes(output_.data() + output_start, segment_length);
            }
            break;
        }
        default: {
            set_error(NotationError::invalid_token, "unknown opcode");
            break;
        }
    }
}

size_t NotationDecoder::read_varint_unsigned(uint32_t& value) {
    try {
        size_t consumed = VarintCodec::decode_unsigned(
            pending_input_.data() + input_position_,
            pending_input_.size() - input_position_, value);
        return consumed;
    } catch (const NotationException& ex) {
        set_error(ex.error_code(), "varint decode failed");
        return 0;
    }
}

size_t NotationDecoder::read_varint_signed(int32_t& value) {
    try {
        size_t consumed = VarintCodec::decode_signed(
            pending_input_.data() + input_position_,
            pending_input_.size() - input_position_, value);
        return consumed;
    } catch (const NotationException& ex) {
        set_error(ex.error_code(), "varint decode failed");
        return 0;
    }
}

void NotationDecoder::emit_text(const std::string& text) {
    if (!text.empty()) {
        output_.insert(output_.end(), text.begin(), text.end());
    }
}

void NotationDecoder::emit_byte(uint8_t byte) {
    output_.push_back(byte);
}

void NotationDecoder::emit_bytes(const uint8_t* data, size_t length) {
    if (length > 0) {
        output_.insert(output_.end(), data, data + length);
    }
}

void NotationDecoder::set_error(NotationError err, const std::string& message) {
    if (error_code_ == NotationError::ok) {
        error_code_ = err;
    }
    (void)message;
}

}
