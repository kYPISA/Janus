#ifndef NOTATION_PREFILTER_HPP
#define NOTATION_PREFILTER_HPP

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "janus_notation.hpp"
#include "janus_notation_encoder.hpp"
#include "janus_notation_decoder.hpp"

struct NotationPrefilterResult {
    std::vector<uint8_t> body;
    std::vector<uint8_t> vocab;
};

inline bool notation_is_escape_byte(uint8_t byte) {
    return (byte >= 0x20 && byte <= 0x2F) || byte >= 0xC0;
}

inline bool notation_is_identifier_byte(uint8_t byte) {
    return std::isalnum(static_cast<unsigned char>(byte)) != 0 ||
           byte == '_' || byte == '-' || byte == '.';
}

inline bool notation_is_quote_byte(uint8_t byte) {
    return byte == '"' || byte == '\'' || byte == '`';
}

inline void notation_collect_frequencies(const uint8_t* data, size_t length,
                                         std::unordered_map<std::string, uint32_t>& frequencies) {
    size_t position = 0;
    while (position < length) {
        uint8_t current = data[position];
        if (notation_is_quote_byte(current)) {
            uint8_t quote = current;
            size_t start = position + 1;
            size_t end = start;
            while (end < length && data[end] != quote) {
                ++end;
            }
            size_t token_length = end - start;
            if (token_length > 0) {
                frequencies[std::string(reinterpret_cast<const char*>(data + start), token_length)]++;
            }
            position = end;
            if (position < length && data[position] == quote) {
                ++position;
            }
            continue;
        }
        if (notation_is_identifier_byte(current)) {
            size_t start = position;
            while (position < length && notation_is_identifier_byte(data[position])) {
                ++position;
            }
            size_t token_length = position - start;
            if (token_length >= 2 && token_length <= 64) {
                frequencies[std::string(reinterpret_cast<const char*>(data + start), token_length)]++;
            }
            continue;
        }
        ++position;
    }
}

inline void notation_build_vocab_set(const std::unordered_map<std::string, uint32_t>& frequencies,
                                     std::unordered_set<std::string>& vocab_set) {
    uint32_t max_freq = 0;
    for (const auto& entry : frequencies) {
        if (entry.second > max_freq) {
            max_freq = entry.second;
        }
    }
    uint32_t threshold = max_freq / 40;
    if (threshold < 2) {
        threshold = 2;
    }
    std::vector<std::pair<std::string, uint32_t>> ranked;
    ranked.reserve(frequencies.size());
    for (const auto& entry : frequencies) {
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
    size_t selected = std::min(ranked.size(), static_cast<size_t>(48));
    for (size_t i = 0; i < selected; ++i) {
        vocab_set.insert(ranked[i].first);
    }
}

inline bool notation_encode_segment(const uint8_t* data, size_t size, NotationPrefilterResult& out) {
    if (data == nullptr || size == 0) {
        return false;
    }
    try {
        janus::NotationConfig config;
        config.enable_adaptive_vocab = true;
        config.max_adaptive_tokens = 48;
        janus::NotationEncoder encoder(config);
        encoder.feed_data(data, size);

        std::unordered_map<std::string, uint32_t> frequencies;
        notation_collect_frequencies(data, size, frequencies);
        std::unordered_set<std::string> vocab_set;
        notation_build_vocab_set(frequencies, vocab_set);

        std::vector<uint8_t> gap;
        bool gap_dirty = false;

        auto flush_gap = [&]() {
            if (gap.empty()) {
                return;
            }
            if (gap_dirty) {
                encoder.feed_literal_string(gap.data(), gap.size());
            } else {
                std::string key(reinterpret_cast<const char*>(gap.data()), gap.size());
                if (key.size() >= 2 && vocab_set.count(key) > 0) {
                    encoder.feed_literal_string(gap.data(), gap.size());
                } else {
                    encoder.feed_raw_bytes(gap.data(), gap.size());
                }
            }
            gap.clear();
            gap_dirty = false;
        };

        auto vocab_token_matches = [&](const uint8_t* token, size_t token_length) -> bool {
            if (token_length < 4 || token_length > 16) {
                return false;
            }
            std::string key(reinterpret_cast<const char*>(token), token_length);
            return vocab_set.count(key) > 0;
        };

        auto emit_vocab_token = [&](const uint8_t* token, size_t token_length) -> bool {
            if (!vocab_token_matches(token, token_length)) {
                return false;
            }
            flush_gap();
            encoder.feed_literal_string(token, token_length);
            return true;
        };

        size_t position = 0;
        while (position < size) {
            uint8_t current = data[position];
            if (notation_is_quote_byte(current)) {
                size_t end = position + 1;
                while (end < size && data[end] != current) {
                    ++end;
                }
                size_t content_length = end - (position + 1);
                if (end < size && vocab_token_matches(data + position + 1, content_length)) {
                    gap.push_back(current);
                    gap_dirty = true;
                    flush_gap();
                    encoder.feed_literal_string(data + position + 1, content_length);
                    position = end;
                    continue;
                }
                gap.push_back(current);
                gap_dirty = true;
                ++position;
                continue;
            }
            if (notation_is_identifier_byte(current)) {
                size_t start = position;
                while (position < size && notation_is_identifier_byte(data[position])) {
                    ++position;
                }
                if (!emit_vocab_token(data + start, position - start)) {
                    gap.insert(gap.end(), data + start, data + position);
                    for (size_t k = start; k < position; ++k) {
                        if (notation_is_escape_byte(data[k])) {
                            gap_dirty = true;
                            break;
                        }
                    }
                }
                continue;
            }
            gap.push_back(current);
            if (notation_is_escape_byte(current)) {
                gap_dirty = true;
            }
            ++position;
        }
        flush_gap();
        encoder.finalize();

        size_t body_length = 0;
        size_t vocab_length = 0;
        const uint8_t* body = encoder.get_encoded_data(body_length);
        const uint8_t* vocab = encoder.get_vocab_header(vocab_length);

        if (body_length == 0 || body == nullptr) {
            return false;
        }
        out.body.assign(body, body + body_length);
        if (vocab != nullptr && vocab_length > 0) {
            out.vocab.assign(vocab, vocab + vocab_length);
        } else {
            out.vocab.clear();
        }
        return true;
    } catch (const janus::NotationException&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

inline bool notation_decode_segment(const uint8_t* body, size_t body_size,
                                    const uint8_t* vocab, size_t vocab_size,
                                    std::vector<uint8_t>& out, size_t max_output = JANUS_MAX_NOTATION) {
    if (body == nullptr || body_size == 0) {
        return false;
    }
    try {
        janus::NotationDecoder decoder(max_output);
        if (vocab != nullptr && vocab_size > 0) {
            decoder.set_vocab_header(vocab, vocab_size);
        }
        decoder.feed_encoded_data(body, body_size);
        decoder.finalize();
        if (decoder.has_error()) {
            return false;
        }
        size_t decoded_length = 0;
        const uint8_t* decoded = decoder.get_decoded_data(decoded_length);
        if (decoded_length == 0 || decoded == nullptr) {
            return false;
        }
        out.assign(decoded, decoded + decoded_length);
        return true;
    } catch (const janus::NotationException&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

inline void notation_append_varint(std::vector<uint8_t>& out, uint32_t value) {
    uint8_t buffer[8];
    size_t written = janus::VarintCodec::encode_unsigned(value, buffer, sizeof(buffer));
    out.insert(out.end(), buffer, buffer + written);
}

#endif
