#include "janus_engine.hpp"
#include "thread_pool.hpp"
#include "lib/janus_notation_decoder.hpp"
#include "C/CpuArch.h"
#include <iostream>
#include <stdexcept>
#include <limits>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    try {
        // Serialized fields are not necessarily aligned, even on x86/ARM64.
        alignas(UInt64) Byte packed[24] = {};
        SetUi16(packed + 1, 0x1234u)
        SetUi32(packed + 5, 0x89ABCDEFu)
        SetUi64(packed + 11, UInt64{0x0123456789ABCDEF})
        require(GetUi16(packed + 1) == 0x1234u &&
                GetUi32(packed + 5) == 0x89ABCDEFu &&
                GetUi64(packed + 11) == UInt64{0x0123456789ABCDEF},
                "unaligned integer round trip failed");
        const Byte expected16[] = {0x34, 0x12};
        const Byte expected32[] = {0xEF, 0xCD, 0xAB, 0x89};
        const Byte expected64[] = {0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01};
        require(std::memcmp(packed + 1, expected16, sizeof(expected16)) == 0 &&
                std::memcmp(packed + 5, expected32, sizeof(expected32)) == 0 &&
                std::memcmp(packed + 11, expected64, sizeof(expected64)) == 0,
                "serialized integer byte order changed");
        for (const auto offset : {0, 3, 4, 9, 10, 19, 20, 21, 22, 23})
            require(packed[offset] == 0, "serialized integer overwrote adjacent bytes");

        uint32_t value = 0;
        const uint8_t overflowing[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F};
        bool rejected = false;
        try { janus::VarintCodec::decode_unsigned(overflowing, sizeof(overflowing), value); }
        catch (const janus::NotationException&) { rejected = true; }
        require(rejected, "overflowing varint accepted");
        const uint8_t maximum[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
        require(janus::VarintCodec::decode_unsigned(maximum, sizeof(maximum), value) == 5 &&
                value == std::numeric_limits<uint32_t>::max(), "maximum varint rejected");
        // Emit a valid prefix before the hostile expansion: errors must clear it.
        const uint8_t expansion[] = {'x', 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
        janus::NotationDecoder decoder(16);
        decoder.feed_encoded_data(expansion, sizeof(expansion));
        decoder.finalize();
        size_t output_size = 0;
        decoder.get_decoded_data(output_size);
        require(decoder.has_error() && output_size == 0, "notation expansion accepted");
        const uint8_t duplicate_vocab[] = {2, 0xC0, 1, 'a', 0xC0, 1, 'b'};
        janus::NotationDecoder vocabulary;
        vocabulary.set_vocab_header(duplicate_vocab, sizeof(duplicate_vocab));
        require(vocabulary.has_error(), "duplicate vocabulary accepted");
        for (const auto& bytes : {std::vector<uint8_t>{1}, std::vector<uint8_t>{0, 'a'}, std::vector<uint8_t>{255, 'a'}}) {
            rejected = false;
            try { JanusEngine::undo_rle(bytes.data(), bytes.size(), 1); }
            catch (const std::exception&) { rejected = true; }
            require(rejected, "malformed RLE accepted");
        }
        for (size_t chunk : {size_t{0}, JANUS_MAX_SEGMENT + 1}) {
            rejected = false;
            try { (void)JanusEngine::calculate_segments(1, chunk); }
            catch (const std::exception&) { rejected = true; }
            require(rejected, "invalid segment target accepted");
        }
        const size_t chunk = JANUS_SEGMENT_SIZE_SINGLE;
        for (size_t size : {size_t{0}, size_t{1}, chunk, chunk + 1, chunk * 2 + 1}) {
            const auto segments = JanusEngine::calculate_segments(size);
            require(segments.size() == size / chunk + (size % chunk != 0), "invalid segment count");
            size_t offset = 0;
            for (const auto& segment : segments) {
                require(segment.offset == offset && segment.size > 0 && segment.size <= chunk,
                        "segments exceed target or leave a gap");
                offset += segment.size;
            }
            require(offset == size, "segment coverage mismatch");
        }
        {
            ReorderBuffer<int> results(1);
            ThreadPool pool(1);
            pool.submit([&] {
                try { throw std::runtime_error("worker failure"); }
                catch (...) { results.fail(std::current_exception()); }
            });
            rejected = false;
            try { (void)results.take(0); } catch (const std::exception&) { rejected = true; }
            require(rejected, "worker failure was lost");
        }
        std::cout << "Security regressions passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}