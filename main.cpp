#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <cstdint>
#include <cstring>
#include "janus_engine.hpp"
#include "stream_io.hpp"

extern "C" {
    #include "LzmaEnc.h"
    #include "LzmaDec.h"
    #include "7zTypes.h"
}

#include "brotli-1.1.0/c/include/brotli/encode.h"
#include "brotli-1.1.0/c/include/brotli/decode.h"

using Clock = std::chrono::steady_clock;

static void* SzAlloc(ISzAllocPtr p, size_t size) { (void)p; return malloc(size); }
static void SzFree(ISzAllocPtr p, void* address) { (void)p; free(address); }
static ISzAlloc g_Alloc = { SzAlloc, SzFree };

uint32_t calculate_crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-static_cast<int32_t>(crc & 1)));
        }
    }
    return ~crc;
}

std::string format_bytes(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    size_t unit_index = 0;
    double value = static_cast<double>(bytes);
    while (value >= 1024.0 && unit_index < 3) {
        value /= 1024.0;
        unit_index++;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << value << " " << units[unit_index];
    return oss.str();
}

void print_progress(const std::string& label, size_t processed, size_t total, Clock::time_point start_time) {
    if (total == 0) return;

    double ratio = static_cast<double>(processed) / static_cast<double>(total);
    double percentage = ratio * 100.0;

    auto now = Clock::now();
    double elapsed_seconds = std::chrono::duration<double>(now - start_time).count();
    double speed_mbps = 0.0;
    if (elapsed_seconds > 0.0) {
        speed_mbps = (processed / (1024.0 * 1024.0)) / elapsed_seconds;
    }

    double eta_seconds = 0.0;
    if (speed_mbps > 0.0) {
        eta_seconds = ((total - processed) / (1024.0 * 1024.0)) / speed_mbps;
    }

    std::ostringstream oss;
    oss << "\r" << label << " " << std::fixed << std::setprecision(1) << percentage << "%"
        << " | " << format_bytes(processed) << " / " << format_bytes(total)
        << " | " << std::setprecision(2) << speed_mbps << " MB/s"
        << " | ETA " << std::setprecision(1) << eta_seconds << "s";

    std::cout << oss.str() << std::flush;
}

void clear_progress_line() {
    std::cout << "\r" << std::string(80, ' ') << "\r" << std::flush;
}

std::vector<uint8_t> compress_lzma_ultra(const std::vector<uint8_t>& src) {
    CLzmaEncHandle enc = LzmaEnc_Create(&g_Alloc);
    CLzmaEncProps props;
    LzmaEncProps_Init(&props);

    props.level = 9;
    props.dictSize = 1 << 26;
    props.fb = 273;
    props.algo = 1;
    props.mc = 128;
    props.lc = 4;
    props.writeEndMark = 1;
    LzmaEnc_SetProps(enc, &props);

    size_t propsSize = LZMA_PROPS_SIZE;
    std::vector<uint8_t> outBuf(propsSize + src.size() + src.size() / 3 + 128);
    LzmaEnc_WriteProperties(enc, outBuf.data(), &propsSize);

    SizeT destLen = outBuf.size() - propsSize;
    SizeT srcLen = src.size();

    SRes res = LzmaEnc_MemEncode(enc, outBuf.data() + propsSize, &destLen, src.data(), srcLen,
                                 1, NULL, &g_Alloc, &g_Alloc);
    LzmaEnc_Destroy(enc, &g_Alloc, &g_Alloc);

    if (res == SZ_OK) {
        outBuf.resize(propsSize + destLen);
        return outBuf;
    }
    return {};
}

std::vector<uint8_t> decompress_lzma(const std::vector<uint8_t>& src, size_t orig_size) {
    if (src.size() < LZMA_PROPS_SIZE) return {};

    std::vector<uint8_t> dest(orig_size);
    SizeT destLen = orig_size;
    SizeT srcLen = src.size() - LZMA_PROPS_SIZE;

    ELzmaStatus status;
    SRes res = LzmaDecode(dest.data(), &destLen,
                          src.data() + LZMA_PROPS_SIZE, &srcLen,
                          src.data(), LZMA_PROPS_SIZE,
                          LZMA_FINISH_END, &status, &g_Alloc);

    if (res == SZ_OK) {
        dest.resize(destLen);
        return dest;
    }
    return {};
}

std::vector<uint8_t> compress_brotli_l11(const uint8_t* src, size_t size) {
    if (size == 0) return {};

    size_t max_compressed_size = BrotliEncoderMaxCompressedSize(size);
    std::vector<uint8_t> dest(max_compressed_size);
    size_t encoded_size = max_compressed_size;

    int result = BrotliEncoderCompress(
        11,
        24,
        BROTLI_MODE_GENERIC,
        size,
        src,
        &encoded_size,
        dest.data()
    );

    if (result == BROTLI_TRUE) {
        dest.resize(encoded_size);
        return dest;
    }

    return {};
}

std::vector<uint8_t> decompress_brotli(const uint8_t* src, size_t src_size, size_t expected_size) {
    if (src_size == 0 || expected_size == 0) return {};

    std::vector<uint8_t> dest(expected_size);
    size_t decoded_size = expected_size;

    BrotliDecoderResult result = BrotliDecoderDecompress(
        src_size,
        src,
        &decoded_size,
        dest.data()
    );

    if (result == BROTLI_DECODER_RESULT_SUCCESS && decoded_size == expected_size) {
        return dest;
    }

    return {};
}

bool compress_file(const std::string& input_file, const std::string& output_file) {
    StreamReader reader(input_file);
    if (!reader.is_open()) {
        std::cout << "[Janus Engine] Error: Cannot open input file.\n";
        return false;
    }

    size_t total_size = reader.file_size();

    std::vector<uint8_t> header_sample(std::min(static_cast<size_t>(8192), total_size));
    reader.read(header_sample.data(), header_sample.size());
    reader.seek(0);

    FileFormat format = JanusEngine::detect_format(input_file, header_sample.data(), header_sample.size());
    std::vector<SegmentBoundary> segments;

    if (format == FORMAT_SAFETENSORS) {
        std::vector<uint8_t> full_file(total_size);
        reader.read(full_file.data(), total_size);
        segments = JanusEngine::split_safetensors(full_file.data(), total_size);
        reader.seek(0);
    } else if (format == FORMAT_GGUF) {
        std::vector<uint8_t> full_file(total_size);
        reader.read(full_file.data(), total_size);
        segments = JanusEngine::split_gguf(full_file.data(), total_size);
        reader.seek(0);
    } else {
        segments = JanusEngine::calculate_segments(total_size);
    }

    StreamWriter writer(output_file);
    if (!writer.is_open()) {
        std::cout << "[Janus Engine] Error: Cannot create output file.\n";
        return false;
    }

    JanusHeader header;
    header.magic = JANUS_MAGIC;
    header.version = JANUS_VERSION;
    header.container_mode = 0x02;
    header.segment_count = static_cast<uint16_t>(segments.size());
    header.total_orig_size = total_size;

    writer.write(reinterpret_cast<uint8_t*>(&header), sizeof(JanusHeader));

    std::vector<JanusSegmentDescriptor> descriptors(segments.size());
    writer.write(reinterpret_cast<uint8_t*>(descriptors.data()), sizeof(JanusSegmentDescriptor) * segments.size());

    Clock::time_point start_time = Clock::now();
    size_t total_processed = 0;

    for (size_t seg_idx = 0; seg_idx < segments.size(); ++seg_idx) {
        const auto& seg = segments[seg_idx];

        std::vector<uint8_t> segment_data(seg.size);
        reader.seek(seg.offset);
        reader.read(segment_data.data(), seg.size);

        size_t sample_size = std::min(static_cast<size_t>(4096), seg.size);
        JanusAlgoID chosen_algo = JanusEngine::select_compression_algorithm(input_file, segment_data.data(), sample_size);

        std::vector<uint8_t> prefiltered_data;
        JanusPrefilterID pref_id = JanusEngine::evaluate_and_apply_prefilter(segment_data, prefiltered_data);

        std::vector<uint8_t> compressed_payload;

        if (chosen_algo == ALGO_LZMA_ULTRA) {
            compressed_payload = compress_lzma_ultra(prefiltered_data);
        } else if (chosen_algo == ALGO_BROTLI_L11) {
            compressed_payload = compress_brotli_l11(prefiltered_data.data(), prefiltered_data.size());
        }

        if (compressed_payload.empty() || compressed_payload.size() >= prefiltered_data.size()) {
            compressed_payload = prefiltered_data;
            chosen_algo = ALGO_STORE;
        }

        descriptors[seg_idx].offset = writer.tellp();
        descriptors[seg_idx].algo_id = static_cast<uint8_t>(chosen_algo);
        descriptors[seg_idx].prefilter_id = static_cast<uint8_t>(pref_id);
        descriptors[seg_idx].compressed_size = compressed_payload.size();
        descriptors[seg_idx].original_size = seg.size;
        descriptors[seg_idx].segment_crc32 = calculate_crc32(segment_data.data(), segment_data.size());

        writer.write(compressed_payload.data(), compressed_payload.size());

        total_processed += seg.size;
        print_progress("Compressing", total_processed, total_size, start_time);
    }

    clear_progress_line();

    writer.flush();

    writer.seekp(sizeof(JanusHeader));
    writer.write(reinterpret_cast<uint8_t*>(descriptors.data()), sizeof(JanusSegmentDescriptor) * descriptors.size());
    writer.flush();

    auto elapsed = std::chrono::duration<double>(Clock::now() - start_time).count();
    double speed_mbps = (total_size / (1024.0 * 1024.0)) / elapsed;

    std::cout << "[Janus Engine] Compression finished.\n";
    std::cout << "[Janus Engine] Output: " << output_file << "\n";
    std::cout << "[Janus Engine] Original: " << format_bytes(total_size) << "\n";
    std::cout << "[Janus Engine] Time: " << std::fixed << std::setprecision(2) << elapsed << "s\n";
    std::cout << "[Janus Engine] Speed: " << std::setprecision(2) << speed_mbps << " MB/s\n";

    return true;
}

bool decompress_file(const std::string& input_file, const std::string& output_file) {
    StreamReader reader(input_file);
    if (!reader.is_open()) {
        std::cout << "[Janus Engine] Error: Cannot open archive file.\n";
        return false;
    }

    JanusHeader header;
    reader.read(reinterpret_cast<uint8_t*>(&header), sizeof(JanusHeader));

    if (header.magic != JANUS_MAGIC) {
        std::cout << "[Janus Engine] Error: Invalid header signature.\n";
        return false;
    }

    std::vector<JanusSegmentDescriptor> descriptors(header.segment_count);
    reader.read(reinterpret_cast<uint8_t*>(descriptors.data()), sizeof(JanusSegmentDescriptor) * header.segment_count);

    StreamWriter writer(output_file);
    if (!writer.is_open()) {
        std::cout << "[Janus Engine] Error: Cannot create output file.\n";
        return false;
    }

    Clock::time_point start_time = Clock::now();
    size_t total_processed = 0;

    for (size_t seg_idx = 0; seg_idx < header.segment_count; ++seg_idx) {
        const auto& desc = descriptors[seg_idx];

        std::vector<uint8_t> compressed_payload(desc.compressed_size);
        reader.read(compressed_payload.data(), desc.compressed_size);

        std::vector<uint8_t> prefiltered_data;

        if (desc.algo_id == ALGO_LZMA_ULTRA) {
            prefiltered_data = decompress_lzma(compressed_payload, desc.original_size);
        } else if (desc.algo_id == ALGO_BROTLI_L11) {
            prefiltered_data = decompress_brotli(compressed_payload.data(), compressed_payload.size(), desc.original_size);
        } else if (desc.algo_id == ALGO_STORE) {
            prefiltered_data = compressed_payload;
        }

        if (prefiltered_data.empty()) {
            std::cout << "[Janus Engine] Error: Decompression failed at segment " << (seg_idx + 1) << "\n";
            return false;
        }

        std::vector<uint8_t> original_data;
        if (desc.prefilter_id == PREFILTER_RLE) {
            original_data = JanusEngine::undo_rle(prefiltered_data.data(), prefiltered_data.size());
        } else if (desc.prefilter_id == PREFILTER_DELTA) {
            original_data = JanusEngine::undo_delta(prefiltered_data.data(), prefiltered_data.size());
        } else {
            original_data = prefiltered_data;
        }

        uint32_t check_crc = calculate_crc32(original_data.data(), original_data.size());
        if (check_crc != desc.segment_crc32) {
            std::cout << "[Janus Engine] Error: CRC32 mismatch at segment " << (seg_idx + 1) << "\n";
            return false;
        }

        writer.write(original_data.data(), original_data.size());

        total_processed += desc.original_size;
        print_progress("Decompressing", total_processed, header.total_orig_size, start_time);
    }

    clear_progress_line();

    writer.flush();

    auto elapsed = std::chrono::duration<double>(Clock::now() - start_time).count();
    double speed_mbps = (header.total_orig_size / (1024.0 * 1024.0)) / elapsed;

    std::cout << "[Janus Engine] Decompression finished.\n";
    std::cout << "[Janus Engine] Output: " << output_file << "\n";
    std::cout << "[Janus Engine] Size: " << format_bytes(header.total_orig_size) << "\n";
    std::cout << "[Janus Engine] Time: " << std::fixed << std::setprecision(2) << elapsed << "s\n";
    std::cout << "[Janus Engine] Speed: " << std::setprecision(2) << speed_mbps << " MB/s\n";

    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Janus Engine v2.0\n";
        std::cout << "Usage:\n";
        std::cout << "  Compress:   janus -c <input_path> [output_path]\n";
        std::cout << "  Decompress: janus -d <input_path> [output_path]\n";
        return 0;
    }

    std::string mode = argv[1];
    std::string input_path = argv[2];
    std::string output_path;

    if (mode == "-c") {
        output_path = (argc > 3) ? argv[3] : input_path + ".janus";
        compress_file(input_path, output_path);
    } else if (mode == "-d") {
        output_path = (argc > 3) ? argv[3] : input_path + ".unjanus";
        decompress_file(input_path, output_path);
    } else {
        std::cout << "Unknown mode. Use -c for compression or -d for decompression.\n";
    }

    return 0;
}
