#include "janus_notation_encoder.hpp"
#include "janus_notation.hpp"
#include <cstring>
#include <cctype>
#include <algorithm>

namespace janus {

namespace {

bool is_identifier_char(uint8_t byte) {
    return std::isalnum(static_cast<unsigned char>(byte)) || byte == '_' || byte == '-' || byte == '.';
}

bool is_quote_char(uint8_t byte) {
    return byte == '"' || byte == '\'' || byte == '`';
}

}

NotationEncoder::NotationEncoder(const NotationConfig& config)
    : config_(config), finalized_(false), adaptive_vocab_built_(false) {
    if (config_.max_adaptive_tokens > 48) {
        throw NotationException(NotationError::adaptive_vocab_overflow,
                                "max adaptive tokens exceeds 48");
    }
}

void NotationEncoder::reset() {
    body_stream_.clear();
    vocab_stream_.clear();
    finalized_ = false;
    adaptive_vocab_built_ = false;
    adaptive_lookup_.clear();
    frequency_map_.clear();
}

void NotationEncoder::feed_data(const uint8_t* data, size_t length) {
    if (finalized_) {
        throw NotationException(NotationError::already_finalized,
                                "encoder already finalized");
    }
    if (data == nullptr || length == 0) {
        return;
    }
    collect_frequencies(data, length);
    if (config_.enable_adaptive_vocab) {
        build_adaptive_vocab_if_needed();
    }
}

void NotationEncoder::feed_data(const char* data, size_t length) {
    feed_data(reinterpret_cast<const uint8_t*>(data), length);
}

void NotationEncoder::feed_token(NotationToken token) {
    if (finalized_) {
        throw NotationException(NotationError::already_finalized,
                                "encoder already finalized");
    }
    body_stream_.append_byte(static_cast<uint8_t>(token));
}

void NotationEncoder::feed_literal_string(const uint8_t* data, size_t length) {
    if (finalized_) {
        throw NotationException(NotationError::already_finalized,
                                "encoder already finalized");
    }
    emit_literal_string(data, length);
}

void NotationEncoder::feed_literal_string(const char* data, size_t length) {
    feed_literal_string(reinterpret_cast<const uint8_t*>(data), length);
}

void NotationEncoder::feed_literal_integer(int32_t value) {
    if (finalized_) {
        throw NotationException(NotationError::already_finalized,
                                "encoder already finalized");
    }
    if (value >= 0 && value <= 11) {
        body_stream_.append_byte(static_cast<uint8_t>(NotationToken::literal_zero) +
                                 static_cast<uint8_t>(value));
        return;
    }
    if (value == -1) {
        body_stream_.append_byte(static_cast<uint8_t>(NotationToken::literal_minus_one));
        return;
    }
    body_stream_.append_byte(static_cast<uint8_t>(NotationToken::literal_integer));
    uint8_t varint_buffer[8];
    size_t encoded_length = VarintCodec::encode_signed(value, varint_buffer, sizeof(varint_buffer));
    body_stream_.append_bytes(varint_buffer, encoded_length);
}

void NotationEncoder::feed_literal_float(float value) {
    if (finalized_) {
        throw NotationException(NotationError::already_finalized,
                                "encoder already finalized");
    }
    body_stream_.append_byte(static_cast<uint8_t>(NotationToken::literal_float));
    uint8_t bytes[4];
    std::memcpy(bytes, &value, sizeof(value));
    body_stream_.append_bytes(bytes, sizeof(bytes));
}

void NotationEncoder::feed_literal_double(double value) {
    if (finalized_) {
        throw NotationException(NotationError::already_finalized,
                                "encoder already finalized");
    }
    body_stream_.append_byte(static_cast<uint8_t>(NotationToken::literal_double));
    uint8_t bytes[8];
    std::memcpy(bytes, &value, sizeof(value));
    body_stream_.append_bytes(bytes, sizeof(bytes));
}

void NotationEncoder::feed_raw_bytes(const uint8_t* data, size_t length) {
    if (finalized_) {
        throw NotationException(NotationError::already_finalized,
                                "encoder already finalized");
    }
    body_stream_.append_bytes(data, length);
}

void NotationEncoder::finalize() {
    if (finalized_) {
        return;
    }
    build_adaptive_vocab_if_needed();
    finalized_ = true;
}

const uint8_t* NotationEncoder::get_encoded_data(size_t& out_length) const {
    out_length = body_stream_.data().size();
    if (out_length == 0) {
        return nullptr;
    }
    return body_stream_.data().data();
}

const uint8_t* NotationEncoder::get_vocab_header(size_t& out_length) const {
    out_length = vocab_stream_.data().size();
    if (out_length == 0) {
        return nullptr;
    }
    return vocab_stream_.data().data();
}

bool NotationEncoder::is_finalized() const {
    return finalized_;
}

void NotationEncoder::collect_frequencies(const uint8_t* data, size_t length) {
    size_t position = 0;
    while (position < length) {
        uint8_t current = data[position];
        if (is_quote_char(current)) {
            uint8_t quote = current;
            size_t start = position + 1;
            size_t end = start;
            while (end < length && data[end] != quote) {
                ++end;
            }
            size_t token_length = end - start;
            if (token_length > 0) {
                std::string token(reinterpret_cast<const char*>(data + start), token_length);
                frequency_map_[token]++;
            }
            position = end;
            if (position < length && data[position] == quote) {
                ++position;
            }
            continue;
        }
        if (is_identifier_char(current)) {
            size_t start = position;
            while (position < length && is_identifier_char(data[position])) {
                ++position;
            }
            size_t token_length = position - start;
            if (token_length >= 2 && token_length <= 64) {
                std::string token(reinterpret_cast<const char*>(data + start), token_length);
                frequency_map_[token]++;
            }
            continue;
        }
        ++position;
    }
}

void NotationEncoder::build_adaptive_vocab_if_needed() {
    if (adaptive_vocab_built_ || !config_.enable_adaptive_vocab) {
        return;
    }
    uint32_t max_freq = 0;
    for (const auto& entry : frequency_map_) {
        if (entry.second > max_freq) {
            max_freq = entry.second;
        }
    }
    uint32_t threshold = max_freq / 40;
    if (threshold < 2) {
        threshold = 2;
    }
    std::vector<std::pair<std::string, uint32_t>> ranked;
    ranked.reserve(frequency_map_.size());
    for (const auto& entry : frequency_map_) {
        size_t token_length = entry.first.size();
        if (token_length < 4 || token_length > 16) {
            continue;
        }
        if (entry.second < threshold) {
            continue;
        }
        ranked.emplace_back(entry.first, entry.second);
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& left, const auto& right) {
                  if (left.second != right.second) {
                      return left.second > right.second;
                  }
                  return left.first < right.first;
              });
    size_t selected_count = std::min(ranked.size(), config_.max_adaptive_tokens);
    uint8_t count = static_cast<uint8_t>(selected_count);
    vocab_stream_.append_byte(count);
    uint8_t opcode = static_cast<uint8_t>(NotationToken::adaptive_begin);
    for (size_t i = 0; i < selected_count; ++i) {
        const std::string& text = ranked[i].first;
        adaptive_lookup_[text] = opcode;
        vocab_stream_.append_byte(opcode);
        uint8_t length_buffer[8];
        size_t encoded_length = VarintCodec::encode_unsigned(
            static_cast<uint32_t>(text.size()), length_buffer, sizeof(length_buffer));
        vocab_stream_.append_bytes(length_buffer, encoded_length);
        if (!text.empty()) {
            vocab_stream_.append_bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
        }
        ++opcode;
    }
    adaptive_vocab_built_ = true;
}

bool NotationEncoder::is_core_token_string(NotationToken token, const std::string& text) const {
    (void)token;
    (void)text;
    return false;
}

void NotationEncoder::emit_literal_string(const uint8_t* data, size_t length) {
    if (config_.enable_adaptive_vocab && adaptive_vocab_built_) {
        std::string key(reinterpret_cast<const char*>(data), length);
        auto it = adaptive_lookup_.find(key);
        if (it != adaptive_lookup_.end()) {
            body_stream_.append_byte(it->second);
            return;
        }
    }
    body_stream_.append_byte(static_cast<uint8_t>(NotationToken::literal_string));
    uint8_t length_buffer[8];
    size_t encoded_length = VarintCodec::encode_unsigned(
        static_cast<uint32_t>(length), length_buffer, sizeof(length_buffer));
    body_stream_.append_bytes(length_buffer, encoded_length);
    if (length > 0) {
        body_stream_.append_bytes(data, length);
    }
}

}
