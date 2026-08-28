#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <algorithm>
#include "janus_engine.hpp"
#include "stream_io.hpp"
#include "notation_prefilter.hpp"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #define PSAPI_VERSION 1
    #include <windows.h>
    #include <psapi.h>
#endif

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

static size_t get_peak_memory_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<size_t>(pmc.PeakWorkingSetSize);
    }
#endif
    return 0;
}

static std::string iso8601_timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm_utc;
#ifdef _WIN32
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        return "unknown";
    }
    return std::string(buf);
}

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned char>(c));
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static const char* algo_name(uint8_t algo_id) {
    switch (algo_id) {
        case ALGO_BROTLI_L11: return "brotli-l11";
        case ALGO_LZMA_ULTRA: return "lzma-ultra";
        default:              return "store";
    }
}

static const char* prefilter_name(uint8_t pref_id) {
    switch (pref_id) {
        case PREFILTER_RLE:      return "rle";
        case PREFILTER_DELTA:    return "delta";
        case PREFILTER_NOTATION: return "notation";
        default:                 return "none";
    }
}

struct SegmentLogEntry {
    size_t index;
    uint64_t payload_offset;
    uint64_t original_size;
    uint64_t compressed_size;
    const char* algorithm;
    const char* prefilter;
    uint32_t segment_crc32;
    bool notation_applied = false;
    uint64_t notation_size_before = 0;
    uint64_t notation_size_after = 0;
};

struct BenchmarkLog {
    std::string mode;
    std::string input;
    uint64_t original_size = 0;
    uint64_t compressed_size = 0;
    double compression_time_s = -1.0;
    double decompression_time_s = -1.0;
    size_t peak_memory_bytes = 0;
    bool has_segments = false;
    std::vector<SegmentLogEntry> segments;
};

static bool write_benchmark_log(const std::string& path, const BenchmarkLog& log) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;

    double ratio = 0.0;
    if (log.original_size > 0) {
        ratio = static_cast<double>(log.compressed_size) / static_cast<double>(log.original_size);
    }

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"timestamp\": \"" << json_escape(iso8601_timestamp()) << "\",\n";
    oss << "  \"mode\": \"" << json_escape(log.mode) << "\",\n";
    oss << "  \"input\": \"" << json_escape(log.input) << "\",\n";
    oss << "  \"original_size\": " << log.original_size << ",\n";
    oss << "  \"compressed_size\": " << log.compressed_size << ",\n";
    oss << std::fixed << std::setprecision(4) << "  \"ratio\": " << ratio << ",\n";
    if (log.compression_time_s >= 0.0) {
        oss << std::setprecision(3) << "  \"compression_time_s\": " << log.compression_time_s << ",\n";
    }
    if (log.decompression_time_s >= 0.0) {
        oss << std::setprecision(3) << "  \"decompression_time_s\": " << log.decompression_time_s << ",\n";
    }
    oss << "  \"peak_memory_bytes\": " << log.peak_memory_bytes;
    if (log.has_segments) {
        oss << ",\n  \"segments\": [\n";
        for (size_t i = 0; i < log.segments.size(); ++i) {
            const SegmentLogEntry& s = log.segments[i];
            oss << "    {\"index\": " << s.index
                << ", \"payload_offset\": " << s.payload_offset
                << ", \"original_size\": " << s.original_size
                << ", \"compressed_size\": " << s.compressed_size
                << ", \"algorithm\": \"" << s.algorithm
                << "\", \"prefilter\": \"" << s.prefilter
                << "\", \"notation_applied\": " << (s.notation_applied ? "true" : "false")
                << ", \"notation_size_before\": " << s.notation_size_before
                << ", \"notation_size_after\": " << s.notation_size_after
                << ", \"crc32\": " << s.segment_crc32 << "}";
            if (i + 1 < log.segments.size()) oss << ",";
            oss << "\n";
        }
        oss << "  ]";
    }
    oss << "\n}\n";

    out << oss.str();
    return out.good();
}

static bool read_entire_file(const std::string& path, std::vector<uint8_t>& data) {
    StreamReader reader(path);
    if (!reader.is_open()) return false;
    size_t size = reader.file_size();
    data.resize(size);
    if (size > 0 && reader.read(data.data(), size) != size) {
        return false;
    }
    return true;
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

static bool raw_brotli_compress_file(const std::string& input_file, const std::string& output_file,
                                     const std::string& log_path) {
    std::vector<uint8_t> src;
    if (!read_entire_file(input_file, src)) {
        std::cout << "[Janus Engine] Error: Cannot open input file.\n";
        return false;
    }

    std::cout << "[Janus Engine] Raw Brotli (quality 11) compressing " << input_file
              << " (" << format_bytes(src.size()) << ")...\n";

    Clock::time_point start_time = Clock::now();
    std::vector<uint8_t> payload = compress_brotli_l11(src.data(), src.size());
    if (!src.empty() && payload.empty()) {
        std::cout << "[Janus Engine] Error: Raw Brotli compression failed.\n";
        return false;
    }

    StreamWriter writer(output_file);
    if (!writer.is_open()) {
        std::cout << "[Janus Engine] Error: Cannot create output file.\n";
        return false;
    }

    uint64_t orig_size = src.size();
    writer.write(reinterpret_cast<const uint8_t*>(&orig_size), sizeof(orig_size));
    if (!payload.empty()) {
        writer.write(payload.data(), payload.size());
    }
    writer.flush();

    double elapsed = std::chrono::duration<double>(Clock::now() - start_time).count();
    double speed_mbps = 0.0;
    if (elapsed > 0.0) speed_mbps = (src.size() / (1024.0 * 1024.0)) / elapsed;
    size_t output_size = sizeof(orig_size) + payload.size();

    std::cout << "[Janus Engine] Raw Brotli compression finished.\n";
    std::cout << "[Janus Engine] Output: " << output_file << "\n";
    std::cout << "[Janus Engine] Original: " << format_bytes(src.size()) << "\n";
    std::cout << "[Janus Engine] Compressed: " << format_bytes(output_size) << "\n";
    std::cout << "[Janus Engine] Time: " << std::fixed << std::setprecision(2) << elapsed << "s\n";
    std::cout << "[Janus Engine] Speed: " << std::setprecision(2) << speed_mbps << " MB/s\n";

    if (!log_path.empty()) {
        BenchmarkLog log;
        log.mode = "raw-brotli";
        log.input = input_file;
        log.original_size = src.size();
        log.compressed_size = output_size;
        log.compression_time_s = elapsed;
        log.peak_memory_bytes = get_peak_memory_bytes();
        if (write_benchmark_log(log_path, log)) {
            std::cout << "[Janus Engine] Log: " << log_path << "\n";
        } else {
            std::cout << "[Janus Engine] Warning: Could not write log file " << log_path << "\n";
        }
    }

    return true;
}

static bool raw_brotli_decompress_file(const std::string& input_file, const std::string& output_file) {
    std::vector<uint8_t> data;
    if (!read_entire_file(input_file, data)) {
        std::cout << "[Janus Engine] Error: Cannot open input file.\n";
        return false;
    }

    const size_t prefix_size = sizeof(uint64_t);
    if (data.size() < prefix_size) {
        std::cout << "[Janus Engine] Error: Input is too small to be a raw Brotli file.\n";
        return false;
    }

    uint64_t orig_size = 0;
    std::memcpy(&orig_size, data.data(), sizeof(orig_size));

    Clock::time_point start_time = Clock::now();

    std::vector<uint8_t> dest;
    if (orig_size > 0) {
        if (data.size() <= prefix_size) {
            std::cout << "[Janus Engine] Error: Raw Brotli decompression failed.\n";
            return false;
        }
        dest = decompress_brotli(data.data() + prefix_size, data.size() - prefix_size,
                                 static_cast<size_t>(orig_size));
        if (dest.empty()) {
            std::cout << "[Janus Engine] Error: Raw Brotli decompression failed.\n";
            return false;
        }
    }

    double elapsed = std::chrono::duration<double>(Clock::now() - start_time).count();
    double speed_mbps = 0.0;
    if (elapsed > 0.0) speed_mbps = (orig_size / (1024.0 * 1024.0)) / elapsed;

    StreamWriter writer(output_file);
    if (!writer.is_open()) {
        std::cout << "[Janus Engine] Error: Cannot create output file.\n";
        return false;
    }
    if (!dest.empty()) {
        writer.write(dest.data(), dest.size());
    }
    writer.flush();

    std::cout << "[Janus Engine] Raw Brotli decompression finished.\n";
    std::cout << "[Janus Engine] Output: " << output_file << "\n";
    std::cout << "[Janus Engine] Size: " << format_bytes(static_cast<size_t>(orig_size)) << "\n";
    std::cout << "[Janus Engine] Time: " << std::fixed << std::setprecision(2) << elapsed << "s\n";
    std::cout << "[Janus Engine] Speed: " << std::setprecision(2) << speed_mbps << " MB/s\n";

    return true;
}

static bool raw_lzma_compress_file(const std::string& input_file, const std::string& output_file,
                                   const std::string& log_path) {
    std::vector<uint8_t> src;
    if (!read_entire_file(input_file, src)) {
        std::cout << "[Janus Engine] Error: Cannot open input file.\n";
        return false;
    }

    std::cout << "[Janus Engine] Raw LZMA (level 9) compressing " << input_file
              << " (" << format_bytes(src.size()) << ")...\n";

    Clock::time_point start_time = Clock::now();
    std::vector<uint8_t> payload = compress_lzma_ultra(src);
    if (payload.size() < LZMA_PROPS_SIZE) {
        std::cout << "[Janus Engine] Error: Raw LZMA compression failed.\n";
        return false;
    }

    StreamWriter writer(output_file);
    if (!writer.is_open()) {
        std::cout << "[Janus Engine] Error: Cannot create output file.\n";
        return false;
    }

    uint64_t orig_size = src.size();
    writer.write(payload.data(), LZMA_PROPS_SIZE);
    writer.write(reinterpret_cast<const uint8_t*>(&orig_size), sizeof(orig_size));
    writer.write(payload.data() + LZMA_PROPS_SIZE, payload.size() - LZMA_PROPS_SIZE);
    writer.flush();

    double elapsed = std::chrono::duration<double>(Clock::now() - start_time).count();
    double speed_mbps = 0.0;
    if (elapsed > 0.0) speed_mbps = (src.size() / (1024.0 * 1024.0)) / elapsed;
    size_t output_size = LZMA_PROPS_SIZE + sizeof(orig_size) + (payload.size() - LZMA_PROPS_SIZE);

    std::cout << "[Janus Engine] Raw LZMA compression finished.\n";
    std::cout << "[Janus Engine] Output: " << output_file << "\n";
    std::cout << "[Janus Engine] Original: " << format_bytes(src.size()) << "\n";
    std::cout << "[Janus Engine] Compressed: " << format_bytes(output_size) << "\n";
    std::cout << "[Janus Engine] Time: " << std::fixed << std::setprecision(2) << elapsed << "s\n";
    std::cout << "[Janus Engine] Speed: " << std::setprecision(2) << speed_mbps << " MB/s\n";

    if (!log_path.empty()) {
        BenchmarkLog log;
        log.mode = "raw-lzma";
        log.input = input_file;
        log.original_size = src.size();
        log.compressed_size = output_size;
        log.compression_time_s = elapsed;
        log.peak_memory_bytes = get_peak_memory_bytes();
        if (write_benchmark_log(log_path, log)) {
            std::cout << "[Janus Engine] Log: " << log_path << "\n";
        } else {
            std::cout << "[Janus Engine] Warning: Could not write log file " << log_path << "\n";
        }
    }

    return true;
}

static uint64_t read_raw_lzma_original_size(const std::vector<uint8_t>& data) {
    uint64_t orig_size = 0;
    std::memcpy(&orig_size, data.data() + LZMA_PROPS_SIZE, sizeof(orig_size));
    return orig_size;
}

static bool decode_raw_lzma_payload(const std::vector<uint8_t>& data, uint64_t orig_size,
                                    std::vector<uint8_t>& dest) {
    dest.clear();
    if (orig_size == 0) {
        return true;
    }
    const size_t prefix_size = LZMA_PROPS_SIZE + sizeof(uint64_t);
    dest.resize(static_cast<size_t>(orig_size));
    SizeT dest_len = static_cast<SizeT>(orig_size);
    SizeT src_len = static_cast<SizeT>(data.size() - prefix_size);
    ELzmaStatus status;
    SRes res = LzmaDecode(dest.data(), &dest_len,
                          data.data() + prefix_size, &src_len,
                          data.data(), LZMA_PROPS_SIZE,
                          LZMA_FINISH_END, &status, &g_Alloc);
    return res == SZ_OK && dest_len == orig_size;
}

static bool raw_lzma_decompress_file(const std::string& input_file, const std::string& output_file) {
    std::vector<uint8_t> data;
    if (!read_entire_file(input_file, data)) {
        std::cout << "[Janus Engine] Error: Cannot open input file.\n";
        return false;
    }

    const size_t prefix_size = LZMA_PROPS_SIZE + sizeof(uint64_t);
    if (data.size() < prefix_size) {
        std::cout << "[Janus Engine] Error: Input is too small to be a raw LZMA file.\n";
        return false;
    }

    uint64_t orig_size = read_raw_lzma_original_size(data);

    Clock::time_point start_time = Clock::now();

    std::vector<uint8_t> dest;
    if (!decode_raw_lzma_payload(data, orig_size, dest)) {
        std::cout << "[Janus Engine] Error: Raw LZMA decompression failed.\n";
        return false;
    }

    double elapsed = std::chrono::duration<double>(Clock::now() - start_time).count();
    double speed_mbps = 0.0;
    if (elapsed > 0.0) speed_mbps = (orig_size / (1024.0 * 1024.0)) / elapsed;

    StreamWriter writer(output_file);
    if (!writer.is_open()) {
        std::cout << "[Janus Engine] Error: Cannot create output file.\n";
        return false;
    }
    if (!dest.empty()) {
        writer.write(dest.data(), dest.size());
    }
    writer.flush();

    std::cout << "[Janus Engine] Raw LZMA decompression finished.\n";
    std::cout << "[Janus Engine] Output: " << output_file << "\n";
    std::cout << "[Janus Engine] Size: " << format_bytes(static_cast<size_t>(orig_size)) << "\n";
    std::cout << "[Janus Engine] Time: " << std::fixed << std::setprecision(2) << elapsed << "s\n";
    std::cout << "[Janus Engine] Speed: " << std::setprecision(2) << speed_mbps << " MB/s\n";

    return true;
}

bool compress_file(const std::string& input_file, const std::string& output_file, const std::string& log_path = "") {
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

    writer.write(reinterpret_cast<uint8_t*>(&header), sizeof(JanusHeader));

    std::vector<JanusSegmentDescriptor> descriptors(segments.size());
    writer.write(reinterpret_cast<uint8_t*>(descriptors.data()), sizeof(JanusSegmentDescriptor) * segments.size());

    std::vector<SegmentLogEntry> segment_log;

    Clock::time_point start_time = Clock::now();
    size_t total_processed = 0;
    size_t payload_offset = sizeof(JanusHeader) + sizeof(JanusSegmentDescriptor) * segments.size();

    for (size_t seg_idx = 0; seg_idx < segments.size(); ++seg_idx) {
        const auto& seg = segments[seg_idx];

        std::vector<uint8_t> segment_data(seg.size);
        reader.seek(seg.offset);
        reader.read(segment_data.data(), seg.size);

        size_t sample_size = std::min(static_cast<size_t>(4096), seg.size);
        JanusAlgoID chosen_algo = JanusEngine::select_compression_algorithm(input_file, segment_data.data(), sample_size);

        std::vector<uint8_t> prefiltered_data;
        JanusPrefilterID pref_id = JanusEngine::evaluate_and_apply_prefilter(segment_data, prefiltered_data);

        std::vector<uint8_t> notation_vocab;
        bool notation_applied = false;
        uint64_t notation_size_after = seg.size;

        std::vector<uint8_t> compressed_payload;
        JanusAlgoID payload_algo = chosen_algo;

        if (chosen_algo == ALGO_LZMA_ULTRA) {
            std::vector<uint8_t> candidate = compress_lzma_ultra(prefiltered_data);
            if (!candidate.empty() && candidate.size() < prefiltered_data.size()) {
                compressed_payload = std::move(candidate);
            } else {
                payload_algo = ALGO_STORE;
            }
        } else if (chosen_algo == ALGO_BROTLI_L11) {
            std::vector<uint8_t> candidate = compress_brotli_l11(prefiltered_data.data(), prefiltered_data.size());
            if (!candidate.empty() && candidate.size() < prefiltered_data.size()) {
                compressed_payload = std::move(candidate);
            } else {
                payload_algo = ALGO_STORE;
            }
        }

        if (compressed_payload.empty()) {
            compressed_payload = prefiltered_data;
        }

        if (pref_id == PREFILTER_NONE && seg.size >= 64) {
            NotationPrefilterResult notation_result;
            if (notation_encode_segment(segment_data.data(), seg.size, notation_result)) {
                double orig_entropy = JanusEngine::calculate_entropy(segment_data.data(), segment_data.size());
                double body_entropy = JanusEngine::calculate_entropy(notation_result.body.data(),
                                                                    notation_result.body.size());
                double orig_bits = orig_entropy * static_cast<double>(seg.size);
                double notation_bits = body_entropy * static_cast<double>(notation_result.body.size()) +
                                       8.0 * static_cast<double>(notation_result.vocab.size());
                if (notation_bits < orig_bits * 0.99) {
                    std::vector<uint8_t> notation_compressed;
                    if (chosen_algo == ALGO_LZMA_ULTRA) {
                        notation_compressed = compress_lzma_ultra(notation_result.body);
                    } else {
                        notation_compressed = compress_brotli_l11(notation_result.body.data(),
                                                                  notation_result.body.size());
                    }
                    std::vector<uint8_t> notation_stream;
                    JanusAlgoID notation_algo = chosen_algo;
                    if (!notation_compressed.empty() &&
                        notation_compressed.size() < notation_result.body.size()) {
                        notation_stream = std::move(notation_compressed);
                    } else {
                        notation_stream = notation_result.body;
                        notation_algo = ALGO_STORE;
                    }
                    size_t notation_payload_size = notation_stream.size() +
                                                   notation_result.vocab.size() + 16;
                    if (notation_payload_size < compressed_payload.size()) {
                        uint64_t notation_total = static_cast<uint64_t>(notation_result.body.size()) +
                                                  static_cast<uint64_t>(notation_result.vocab.size());
                        prefiltered_data = std::move(notation_result.body);
                        notation_vocab = std::move(notation_result.vocab);
                        compressed_payload = std::move(notation_stream);
                        payload_algo = notation_algo;
                        pref_id = PREFILTER_NOTATION;
                        notation_applied = true;
                        notation_size_after = notation_total;
                        std::cout << "[Janus Engine] Notation prefilter applied to segment " << (seg_idx + 1)
                                  << ": " << format_bytes(seg.size) << " -> "
                                  << format_bytes(static_cast<size_t>(notation_size_after)) << "\n";
                    }
                }
            }
        }

        std::vector<uint8_t> segment_payload;
        if (pref_id == PREFILTER_NOTATION) {
            notation_append_varint(segment_payload, static_cast<uint32_t>(notation_vocab.size()));
            if (!notation_vocab.empty()) {
                segment_payload.insert(segment_payload.end(), notation_vocab.begin(), notation_vocab.end());
            }
            notation_append_varint(segment_payload, static_cast<uint32_t>(prefiltered_data.size()));
            segment_payload.insert(segment_payload.end(), compressed_payload.begin(), compressed_payload.end());
        } else {
            segment_payload = std::move(compressed_payload);
        }

        if (segment_payload.size() > 0xFFFFFFFFull || seg.size > 0xFFFFFFFFull) {
            std::cout << "[Janus Engine] Error: Segment " << (seg_idx + 1)
                      << " too large for this container version.\n";
            return false;
        }

        descriptors[seg_idx].algo_id = static_cast<uint8_t>(payload_algo);
        descriptors[seg_idx].prefilter_id = static_cast<uint8_t>(pref_id);
        descriptors[seg_idx].compressed_size = static_cast<uint32_t>(segment_payload.size());
        descriptors[seg_idx].original_size = static_cast<uint32_t>(seg.size);
        descriptors[seg_idx].segment_crc32 = calculate_crc32(segment_data.data(), segment_data.size());

        SegmentLogEntry seg_entry;
        seg_entry.index = seg_idx;
        seg_entry.payload_offset = payload_offset;
        seg_entry.original_size = seg.size;
        seg_entry.compressed_size = segment_payload.size();
        seg_entry.algorithm = algo_name(static_cast<uint8_t>(payload_algo));
        seg_entry.prefilter = prefilter_name(static_cast<uint8_t>(pref_id));
        seg_entry.segment_crc32 = descriptors[seg_idx].segment_crc32;
        seg_entry.notation_applied = notation_applied;
        seg_entry.notation_size_before = seg.size;
        seg_entry.notation_size_after = notation_size_after;
        segment_log.push_back(seg_entry);

        writer.write(segment_payload.data(), segment_payload.size());
        payload_offset += segment_payload.size();

        total_processed += seg.size;
        print_progress("Compressing", total_processed, total_size, start_time);
    }

    clear_progress_line();

    writer.flush();

    size_t output_size = writer.tellp();

    writer.seekp(sizeof(JanusHeader));
    writer.write(reinterpret_cast<uint8_t*>(descriptors.data()), sizeof(JanusSegmentDescriptor) * descriptors.size());
    writer.flush();

    auto elapsed = std::chrono::duration<double>(Clock::now() - start_time).count();
    double speed_mbps = (total_size / (1024.0 * 1024.0)) / elapsed;

    std::cout << "[Janus Engine] Compression finished.\n";
    std::cout << "[Janus Engine] Output: " << output_file << "\n";
    std::cout << "[Janus Engine] Original: " << format_bytes(total_size) << "\n";
    std::cout << "[Janus Engine] Compressed: " << format_bytes(output_size) << "\n";
    std::cout << "[Janus Engine] Time: " << std::fixed << std::setprecision(2) << elapsed << "s\n";
    std::cout << "[Janus Engine] Speed: " << std::setprecision(2) << speed_mbps << " MB/s\n";

    if (!log_path.empty()) {
        BenchmarkLog log;
        log.mode = "janus-container";
        log.input = input_file;
        log.original_size = total_size;
        log.compressed_size = output_size;
        log.compression_time_s = elapsed;
        log.peak_memory_bytes = get_peak_memory_bytes();
        log.has_segments = true;
        log.segments = std::move(segment_log);
        if (write_benchmark_log(log_path, log)) {
            std::cout << "[Janus Engine] Log: " << log_path << "\n";
        } else {
            std::cout << "[Janus Engine] Warning: Could not write log file " << log_path << "\n";
        }
    }

    return true;
}

bool decompress_file(const std::string& input_file, const std::string& output_file, const std::string& log_path = "") {
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

    if (header.version != 0x02 && header.version != 0x03) {
        std::cout << "[Janus Engine] Error: Unsupported container version "
                  << static_cast<int>(header.version) << ".\n";
        return false;
    }

    uint64_t total_orig_size = 0;
    std::vector<JanusSegmentDescriptor> descriptors(header.segment_count);

    if (header.version == 0x02) {
        uint64_t legacy_total_orig_size = 0;
        reader.read(reinterpret_cast<uint8_t*>(&legacy_total_orig_size), sizeof(legacy_total_orig_size));
        total_orig_size = legacy_total_orig_size;

        std::vector<JanusSegmentDescriptorV2> legacy_descriptors(header.segment_count);
        reader.read(reinterpret_cast<uint8_t*>(legacy_descriptors.data()),
                    sizeof(JanusSegmentDescriptorV2) * header.segment_count);
        for (size_t i = 0; i < header.segment_count; ++i) {
            if (legacy_descriptors[i].compressed_size > 0xFFFFFFFFu ||
                legacy_descriptors[i].original_size > 0xFFFFFFFFu) {
                std::cout << "[Janus Engine] Error: Segment " << (i + 1)
                          << " too large for this build.\n";
                return false;
            }
            descriptors[i].algo_id = legacy_descriptors[i].algo_id;
            descriptors[i].prefilter_id = legacy_descriptors[i].prefilter_id;
            descriptors[i].compressed_size = static_cast<uint32_t>(legacy_descriptors[i].compressed_size);
            descriptors[i].original_size = static_cast<uint32_t>(legacy_descriptors[i].original_size);
            descriptors[i].segment_crc32 = legacy_descriptors[i].segment_crc32;
        }
    } else {
        reader.read(reinterpret_cast<uint8_t*>(descriptors.data()),
                    sizeof(JanusSegmentDescriptor) * header.segment_count);
        for (size_t i = 0; i < header.segment_count; ++i) {
            total_orig_size += descriptors[i].original_size;
        }
    }

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

        std::vector<uint8_t> original_data;

        if (desc.prefilter_id == PREFILTER_NOTATION) {
            bool notation_ok = true;
            size_t position = 0;
            uint32_t metadata_length = 0;
            uint32_t notation_length = 0;
            try {
                position += janus::VarintCodec::decode_unsigned(compressed_payload.data() + position,
                                                                compressed_payload.size() - position, metadata_length);
                if (position + metadata_length >= compressed_payload.size()) {
                    notation_ok = false;
                }
                if (notation_ok) {
                    const uint8_t* metadata = compressed_payload.data() + position;
                    position += metadata_length;
                    position += janus::VarintCodec::decode_unsigned(compressed_payload.data() + position,
                                                                    compressed_payload.size() - position, notation_length);
                    if (notation_length == 0 || position >= compressed_payload.size()) {
                        notation_ok = false;
                    }
                    if (notation_ok) {
                        std::vector<uint8_t> stream_vec(compressed_payload.begin() + position,
                                                        compressed_payload.end());
                        std::vector<uint8_t> notation_body;
                        if (desc.algo_id == ALGO_LZMA_ULTRA) {
                            notation_body = decompress_lzma(stream_vec, notation_length);
                        } else if (desc.algo_id == ALGO_BROTLI_L11) {
                            notation_body = decompress_brotli(stream_vec.data(), stream_vec.size(), notation_length);
                        } else if (desc.algo_id == ALGO_STORE) {
                            if (stream_vec.size() == notation_length) {
                                notation_body = std::move(stream_vec);
                            }
                        }
                        if (notation_body.empty() ||
                            !notation_decode_segment(notation_body.data(), notation_body.size(),
                                                     metadata, metadata_length, original_data)) {
                            notation_ok = false;
                        }
                    }
                }
            } catch (const janus::NotationException&) {
                notation_ok = false;
            } catch (const std::exception&) {
                notation_ok = false;
            }
            if (!notation_ok || original_data.empty()) {
                std::cout << "[Janus Engine] Error: Decompression failed at segment " << (seg_idx + 1) << "\n";
                return false;
            }
        } else {
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

            if (desc.prefilter_id == PREFILTER_RLE) {
                original_data = JanusEngine::undo_rle(prefiltered_data.data(), prefiltered_data.size());
            } else if (desc.prefilter_id == PREFILTER_DELTA) {
                original_data = JanusEngine::undo_delta(prefiltered_data.data(), prefiltered_data.size());
            } else {
                original_data = std::move(prefiltered_data);
            }
        }

        uint32_t check_crc = calculate_crc32(original_data.data(), original_data.size());
        if (check_crc != desc.segment_crc32) {
            std::cout << "[Janus Engine] Error: CRC32 mismatch at segment " << (seg_idx + 1) << "\n";
            return false;
        }

        writer.write(original_data.data(), original_data.size());

        total_processed += desc.original_size;
        print_progress("Decompressing", total_processed, total_orig_size, start_time);
    }

    clear_progress_line();

    writer.flush();

    auto elapsed = std::chrono::duration<double>(Clock::now() - start_time).count();
    double speed_mbps = (total_orig_size / (1024.0 * 1024.0)) / elapsed;

    std::cout << "[Janus Engine] Decompression finished.\n";
    std::cout << "[Janus Engine] Output: " << output_file << "\n";
    std::cout << "[Janus Engine] Size: " << format_bytes(total_orig_size) << "\n";
    std::cout << "[Janus Engine] Time: " << std::fixed << std::setprecision(2) << elapsed << "s\n";
    std::cout << "[Janus Engine] Speed: " << std::setprecision(2) << speed_mbps << " MB/s\n";

    if (!log_path.empty()) {
        BenchmarkLog log;
        log.mode = "janus-container-decompress";
        log.input = input_file;
        log.original_size = total_orig_size;
        log.compressed_size = reader.file_size();
        log.decompression_time_s = elapsed;
        log.peak_memory_bytes = get_peak_memory_bytes();
        if (write_benchmark_log(log_path, log)) {
            std::cout << "[Janus Engine] Log: " << log_path << "\n";
        } else {
            std::cout << "[Janus Engine] Warning: Could not write log file " << log_path << "\n";
        }
    }

    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Janus Engine v2.0\n";
        std::cout << "Usage:\n";
        std::cout << "  Compress:   janus -c <input_path> [output_path] [--log <log.json>]\n";
        std::cout << "  Decompress: janus -d <input_path> [output_path] [--log <log.json>]\n";
        std::cout << "  Debug / benchmark (hidden):\n";
        std::cout << "    janus --raw-brotli <input_path> <output_path> [--log <log.json>]\n";
        std::cout << "    janus --raw-brotli -d <input_path> <output_path>\n";
        std::cout << "    janus --raw-lzma <input_path> <output_path> [--log <log.json>]\n";
        std::cout << "    janus --raw-lzma -d <input_path> <output_path>\n";
        return 0;
    }

    std::string mode = argv[1];
    bool raw_decompress = false;
    std::string log_path;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--log" && i + 1 < argc) {
            log_path = argv[++i];
        } else if (arg == "-d" && (mode == "--raw-brotli" || mode == "--raw-lzma")) {
            raw_decompress = true;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.empty()) {
        std::cout << "Missing input path.\n";
        return 1;
    }

    std::string input_path = positional[0];
    std::string output_path;

    if (mode == "-c") {
        output_path = (positional.size() > 1) ? positional[1] : input_path + ".jan";
        compress_file(input_path, output_path, log_path);
    } else if (mode == "-d") {
        output_path = (positional.size() > 1) ? positional[1] : input_path + ".unjan";
        decompress_file(input_path, output_path, log_path);
    } else if (mode == "--raw-brotli") {
        if (positional.size() < 2) {
            std::cout << "Missing output path.\n";
            return 1;
        }
        if (raw_decompress) {
            raw_brotli_decompress_file(input_path, positional[1]);
        } else {
            raw_brotli_compress_file(input_path, positional[1], log_path);
        }
    } else if (mode == "--raw-lzma") {
        if (positional.size() < 2) {
            std::cout << "Missing output path.\n";
            return 1;
        }
        if (raw_decompress) {
            raw_lzma_decompress_file(input_path, positional[1]);
        } else {
            raw_lzma_compress_file(input_path, positional[1], log_path);
        }
    } else {
        std::cout << "Unknown mode. Use -c for compression or -d for decompression.\n";
    }

    return 0;
}
