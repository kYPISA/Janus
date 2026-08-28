
#include <janus_notation.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using namespace janus;

static bool failed = false;

static void assert_true(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL: " << name << std::endl;
        failed = true;
    } else {
        std::cerr << "PASS: " << name << std::endl;
    }
}

static void assert_equal(size_t expected, size_t actual, const char* name) {
    if (expected != actual) {
        std::cerr << "FAIL: " << name << " expected=" << expected << " actual=" << actual << std::endl;
        failed = true;
    } else {
        std::cerr << "PASS: " << name << std::endl;
    }
}

static void assert_memory_equal(const uint8_t* expected, const uint8_t* actual, size_t length, const char* name) {
    if (length == 0) {
        std::cerr << "PASS: " << name << std::endl;
        return;
    }
    if (expected == nullptr || actual == nullptr || std::memcmp(expected, actual, length) != 0) {
        std::cerr << "FAIL: " << name << std::endl;
        failed = true;
    } else {
        std::cerr << "PASS: " << name << std::endl;
    }
}

static void test_varint_roundtrip() {
    std::vector<int32_t> values = {0, -1, 1, -2, 2, 127, -128, 128, -129, 16383, -16384, 16384, -16385};
    for (int32_t value : values) {
        uint8_t buffer[8];
        size_t encoded_length = VarintCodec::encode_signed(value, buffer, sizeof(buffer));
        int32_t decoded_value = 0;
        size_t decoded_length = VarintCodec::decode_signed(buffer, encoded_length, decoded_value);
        assert_equal(static_cast<size_t>(value == decoded_value ? 0 : 1), 0, "varint roundtrip");
        assert_equal(encoded_length, decoded_length, "varint length match");
    }
}

static void test_core_tokens() {
    NotationConfig config;
    NotationEncoder encoder(config);
    encoder.feed_token(NotationToken::object_begin);
    encoder.feed_token(NotationToken::array_begin);
    encoder.feed_token(NotationToken::boolean_true);
    encoder.feed_token(NotationToken::array_end);
    encoder.feed_token(NotationToken::object_end);
    encoder.finalize();

    size_t encoded_length = 0;
    const uint8_t* encoded = encoder.get_encoded_data(encoded_length);
    assert_equal(5u, encoded_length, "core tokens encoded length");

    NotationDecoder decoder;
    decoder.feed_encoded_data(encoded, encoded_length);
    decoder.finalize();
    size_t decoded_length = 0;
    const uint8_t* decoded = decoder.get_decoded_data(decoded_length);
    assert_equal(5u, decoded_length, "core tokens decoded length");
    assert_memory_equal(encoded, decoded, decoded_length, "core tokens roundtrip");
    assert_true(!decoder.has_error(), "core tokens no error");
}

static void test_literal_string() {
    NotationConfig config;
    NotationEncoder encoder(config);
    const char* text = "Alice";
    encoder.feed_literal_string(text, std::strlen(text));
    encoder.finalize();

    size_t encoded_length = 0;
    const uint8_t* encoded = encoder.get_encoded_data(encoded_length);
    assert_equal(7u, encoded_length, "literal string encoded length");

    NotationDecoder decoder;
    decoder.feed_encoded_data(encoded, encoded_length);
    decoder.finalize();
    size_t decoded_length = 0;
    const uint8_t* decoded = decoder.get_decoded_data(decoded_length);
    assert_equal(5u, decoded_length, "literal string decoded length");
    assert_memory_equal(reinterpret_cast<const uint8_t*>(text), decoded, decoded_length, "literal string content");
    assert_true(!decoder.has_error(), "literal string no error");
}

static void test_literal_integer() {
    NotationConfig config;
    NotationEncoder encoder(config);
    encoder.feed_literal_integer(42);
    encoder.feed_literal_integer(-1);
    encoder.feed_literal_integer(0);
    encoder.feed_literal_integer(11);
    encoder.feed_literal_integer(-12345);
    encoder.finalize();

    size_t encoded_length = 0;
    const uint8_t* encoded = encoder.get_encoded_data(encoded_length);
    assert_true(encoded_length > 0, "literal integer encoded");

    NotationDecoder decoder;
    decoder.feed_encoded_data(encoded, encoded_length);
    decoder.finalize();
    size_t decoded_length = 0;
    const uint8_t* decoded = decoder.get_decoded_data(decoded_length);
    const char* expected = "42-1011-12345";
    assert_equal(std::strlen(expected), decoded_length, "literal integer decoded length");
    assert_memory_equal(reinterpret_cast<const uint8_t*>(expected), decoded, decoded_length, "literal integer content");
    assert_true(!decoder.has_error(), "literal integer no error");
}

static void test_repetition() {
    NotationConfig config;
    NotationEncoder encoder(config);
    encoder.feed_token(NotationToken::repetition);
    uint8_t count = 5;
    encoder.feed_raw_bytes(&count, 1);
    encoder.feed_token(NotationToken::literal_one);
    encoder.finalize();

    size_t encoded_length = 0;
    const uint8_t* encoded = encoder.get_encoded_data(encoded_length);
    assert_equal(3u, encoded_length, "repetition encoded length");

    NotationDecoder decoder;
    decoder.feed_encoded_data(encoded, encoded_length);
    decoder.finalize();
    size_t decoded_length = 0;
    const uint8_t* decoded = decoder.get_decoded_data(decoded_length);
    assert_equal(5u, decoded_length, "repetition decoded length");
    uint8_t expected[5] = {0x31, 0x31, 0x31, 0x31, 0x31};
    assert_memory_equal(expected, decoded, decoded_length, "repetition content");
    assert_true(!decoder.has_error(), "repetition no error");
}

static void test_malformed_truncated_integer() {
    uint8_t data[2] = {static_cast<uint8_t>(NotationToken::literal_integer), 0xFF};
    NotationDecoder decoder;
    decoder.feed_encoded_data(data, 2);
    decoder.finalize();
    assert_true(decoder.has_error(), "truncated integer reports error");
    assert_equal(static_cast<size_t>(NotationError::truncated_stream),
                 static_cast<size_t>(decoder.get_error_code()),
                 "truncated integer error code");
}

static void test_adaptive_vocab_roundtrip() {
    NotationConfig config;
    config.enable_adaptive_vocab = true;
    config.max_adaptive_tokens = 48;

    const char* json_text = R"({"active":true,"active":false,"score":98,"tags":["a","b"]})";
    size_t json_length = std::strlen(json_text);

    NotationEncoder encoder(config);
    encoder.feed_data(json_text, json_length);

    size_t header_length = 0;
    const uint8_t* header = encoder.get_vocab_header(header_length);
    assert_true(header_length >= 9 && header != nullptr, "adaptive vocab header present");

    encoder.feed_token(NotationToken::object_begin);
    encoder.feed_literal_string("active", 6);
    encoder.feed_token(NotationToken::key_value_separator);
    encoder.feed_token(NotationToken::boolean_true);
    encoder.feed_token(NotationToken::item_separator);
    encoder.feed_literal_string("score", 5);
    encoder.feed_token(NotationToken::key_value_separator);
    encoder.feed_literal_integer(98);
    encoder.feed_token(NotationToken::item_separator);
    encoder.feed_literal_string("tags", 4);
    encoder.feed_token(NotationToken::key_value_separator);
    encoder.feed_token(NotationToken::array_begin);
    encoder.feed_literal_string("a", 1);
    encoder.feed_token(NotationToken::item_separator);
    encoder.feed_literal_string("b", 1);
    encoder.feed_token(NotationToken::array_end);
    encoder.feed_token(NotationToken::object_end);
    encoder.finalize();

    size_t encoded_length = 0;
    const uint8_t* encoded = encoder.get_encoded_data(encoded_length);
    assert_true(encoded_length > 0, "adaptive encoded length");

    NotationDecoder decoder;
    decoder.set_vocab_header(header, header_length);
    decoder.feed_encoded_data(encoded, encoded_length);
    decoder.finalize();

    size_t decoded_length = 0;
    const uint8_t* decoded = decoder.get_decoded_data(decoded_length);
    assert_true(!decoder.has_error(), "adaptive decode no error");

    const uint8_t expected[] = {
        0x03, 'a','c','t','i','v','e', 0x07, 0x01, 0x08,
        's','c','o','r','e', 0x07, '9','8', 0x08,
        't','a','g','s', 0x07, 0x05, 'a', 0x08, 'b', 0x06, 0x04
    };
    assert_equal(sizeof(expected), decoded_length, "adaptive decoded length");
    assert_memory_equal(expected, decoded, decoded_length, "adaptive decoded content");
}

static void test_adaptive_vocab_reduces_size() {
    NotationConfig enabled;
    enabled.enable_adaptive_vocab = true;
    enabled.max_adaptive_tokens = 48;
    NotationConfig disabled;
    disabled.enable_adaptive_vocab = false;

    const char* json_text = R"({"active":true,"active":false,"active":true})";

    NotationEncoder enc_enabled(enabled);
    enc_enabled.feed_data(json_text, std::strlen(json_text));
    enc_enabled.feed_token(NotationToken::object_begin);
    enc_enabled.feed_literal_string("active", 6);
    enc_enabled.feed_token(NotationToken::key_value_separator);
    enc_enabled.feed_token(NotationToken::boolean_true);
    enc_enabled.feed_token(NotationToken::object_end);
    enc_enabled.finalize();

    NotationEncoder enc_disabled(disabled);
    enc_disabled.feed_token(NotationToken::object_begin);
    enc_disabled.feed_literal_string("active", 6);
    enc_disabled.feed_token(NotationToken::key_value_separator);
    enc_disabled.feed_token(NotationToken::boolean_true);
    enc_disabled.feed_token(NotationToken::object_end);
    enc_disabled.finalize();

    size_t enabled_length = 0;
    size_t disabled_length = 0;
    enc_enabled.get_encoded_data(enabled_length);
    enc_disabled.get_encoded_data(disabled_length);
    assert_true(enabled_length < disabled_length, "adaptive reduces encoded size");
}

int main() {
    test_varint_roundtrip();
    test_core_tokens();
    test_literal_string();
    test_literal_integer();
    test_repetition();
    test_malformed_truncated_integer();
    test_adaptive_vocab_roundtrip();
    test_adaptive_vocab_reduces_size();
    return failed ? 1 : 0;
}
