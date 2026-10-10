#include "thread_pool.hpp"

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
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <future>
#include <atomic>
#include <stdexcept>
#include <limits>
#include <filesystem>
#include <memory>
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

#include <brotli/encode.h>
#include <brotli/decode.h>

using Clock = std::chrono::steady_clock;

std::string format_bytes(size_t bytes);

static void* SzAlloc(ISzAllocPtr p, size_t size) { (void)p; return malloc(size); }
static void SzFree(ISzAllocPtr p, void* address) { (void)p; free(address); }
static ISzAlloc g_Alloc = { SzAlloc, SzFree };

static std::atomic<size_t> g_bytes_processed{0};

void print_progress_atomic(const std::string& label, size_t total, Clock::time_point start_time) {
    if (total == 0) return;

    size_t processed = g_bytes_processed.load(std::memory_order_relaxed);
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
    StreamWriter out(path);
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

    const std::string json = oss.str();
    out.write(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    out.commit();
    return true;
}

static bool read_entire_file(const std::string& path, std::vector<uint8_t>& data) {
    StreamReader reader(path);
    if (!reader.is_open()) return false;
    size_t size = reader.file_size();
    if (size > JANUS_MAX_RAW) throw std::runtime_error("Raw codec input exceeds 128 MiB");
    data.resize(size);
    if (size > 0 && reader.read(data.data(), size) != size) {
        return false;
    }
    return true;
}

std::vector<uint8_t> compress_lzma_ultra(const std::vector<uint8_t>& src) {
    CLzmaEncHandle enc = LzmaEnc_Create(&g_Alloc);
    if (!enc) throw std::bad_alloc();
    auto destroy = [](void* p) { LzmaEnc_Destroy(static_cast<CLzmaEncHandle>(p), &g_Alloc, &g_Alloc); };
    std::unique_ptr<void, decltype(destroy)> owner(enc, destroy);
    CLzmaEncProps props;
    LzmaEncProps_Init(&props);

    props.level = 9;
    props.dictSize = 1 << 26;
    props.fb = 273;
    props.algo = 1;
    props.mc = 128;
    props.lc = 4;
    props.writeEndMark = 1;
    if (LzmaEnc_SetProps(enc, &props) != SZ_OK) throw std::runtime_error("Invalid LZMA properties");

    size_t propsSize = LZMA_PROPS_SIZE;
    std::vector<uint8_t> outBuf(propsSize + src.size() + src.size() / 3 + 128);
    if (LzmaEnc_WriteProperties(enc, outBuf.data(), &propsSize) != SZ_OK)
        throw std::runtime_error("Cannot write LZMA properties");

    SizeT destLen = outBuf.size() - propsSize;
    SizeT srcLen = src.size();

    SRes res = LzmaEnc_MemEncode(enc, outBuf.data() + propsSize, &destLen, src.data(), srcLen,
                                 1, NULL, &g_Alloc, &g_Alloc);

    if (res == SZ_OK) {
        outBuf.resize(propsSize + destLen);
        return outBuf;
    }
    return {};
}

std::vector<uint8_t> decompress_lzma(const std::vector<uint8_t>& src, size_t orig_size, bool exact = true) {
    if (src.size() < LZMA_PROPS_SIZE || orig_size == 0 || orig_size > JANUS_MAX_RAW) return {};
    const uint32_t dictionary = uint32_t(src[1]) | (uint32_t(src[2]) << 8) |
                                (uint32_t(src[3]) << 16) | (uint32_t(src[4]) << 24);
    if (dictionary > JANUS_MAX_NOTATION) return {};

    std::vector<uint8_t> dest(orig_size);
    SizeT destLen = orig_size;
    SizeT srcLen = src.size() - LZMA_PROPS_SIZE;

    ELzmaStatus status;
    SRes res = LzmaDecode(dest.data(), &destLen,
                          src.data() + LZMA_PROPS_SIZE, &srcLen,
                          src.data(), LZMA_PROPS_SIZE,
                          LZMA_FINISH_END, &status, &g_Alloc);

    if (res == SZ_OK && (!exact || destLen == orig_size) &&
        srcLen == src.size() - LZMA_PROPS_SIZE &&
        (status == LZMA_STATUS_FINISHED_WITH_MARK || status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)) {
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

std::vector<uint8_t> decompress_brotli(const uint8_t* src, size_t src_size, size_t expected_size, bool exact = true) {
    if (!src || src_size == 0 || expected_size == 0 || expected_size > JANUS_MAX_RAW) return {};
    std::vector<uint8_t> dest(expected_size);
    BrotliDecoderState* state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) return {};
    size_t available_in = src_size, available_out = expected_size, decoded_size = 0;
    const uint8_t* input = src;
    uint8_t* output = dest.data();
    const auto result = BrotliDecoderDecompressStream(state, &available_in, &input,
                                                      &available_out, &output, &decoded_size);
    BrotliDecoderDestroyInstance(state);
    if (result == BROTLI_DECODER_RESULT_SUCCESS && available_in == 0 && (!exact || decoded_size == expected_size)) {
        dest.resize(decoded_size);
        return dest;
    }
    return {};
}

static bool raw_brotli_compress_file(const std::string& input_file, const std::string& output_file,
                                     const std::string& log_path) {
    janus_validate_paths(input_file, output_file, log_path);
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
    writer.commit();

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
    janus_validate_paths(input_file, output_file);
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
    if (orig_size > JANUS_MAX_RAW || (orig_size == 0 && data.size() != prefix_size))
        throw std::runtime_error("Invalid raw Brotli size");

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
    writer.commit();

    std::cout << "[Janus Engine] Raw Brotli decompression finished.\n";
    std::cout << "[Janus Engine] Output: " << output_file << "\n";
    std::cout << "[Janus Engine] Size: " << format_bytes(static_cast<size_t>(orig_size)) << "\n";
    std::cout << "[Janus Engine] Time: " << std::fixed << std::setprecision(2) << elapsed << "s\n";
    std::cout << "[Janus Engine] Speed: " << std::setprecision(2) << speed_mbps << " MB/s\n";

    return true;
}

static bool raw_lzma_compress_file(const std::string& input_file, const std::string& output_file,
                                   const std::string& log_path) {
    janus_validate_paths(input_file, output_file, log_path);
    std::vector<uint8_t> src;
    if (!read_entire_file(input_file, src)) {
        std::cout << "[Janus Engine] Error: Cannot open input file.\n";
        return false;
    }

    std::cout << "[Janus Engine] Raw LZMA (level 9) compressing " << input_file
              << " (" << format_bytes(src.size()) << ")...\n";

    Clock::time_point start_time = Clock::now();
    std::vector<uint8_t> payload = compress_lzma_ultra(src);
    if (src.empty()) payload.resize(LZMA_PROPS_SIZE);
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
    writer.commit();

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
    const size_t prefix_size = LZMA_PROPS_SIZE + sizeof(uint64_t);
    if (data.size() < prefix_size || orig_size > JANUS_MAX_RAW) return false;
    if (orig_size == 0) {
        return data.size() == prefix_size;
    }
    std::vector<uint8_t> payload(data.begin(), data.begin() + LZMA_PROPS_SIZE);
    payload.insert(payload.end(), data.begin() + prefix_size, data.end());
    dest = decompress_lzma(payload, static_cast<size_t>(orig_size));
    return dest.size() == orig_size;
}

static bool raw_lzma_decompress_file(const std::string& input_file, const std::string& output_file) {
    janus_validate_paths(input_file, output_file);
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
    writer.commit();

    std::cout << "[Janus Engine] Raw LZMA decompression finished.\n";
    std::cout << "[Janus Engine] Output: " << output_file << "\n";
    std::cout << "[Janus Engine] Size: " << format_bytes(static_cast<size_t>(orig_size)) << "\n";
    std::cout << "[Janus Engine] Time: " << std::fixed << std::setprecision(2) << elapsed << "s\n";
    std::cout << "[Janus Engine] Speed: " << std::setprecision(2) << speed_mbps << " MB/s\n";

    return true;
}

bool compress_file(const std::string& input_file, const std::string& output_file,
                   const std::string& log_path = "", size_t thread_count = 0) {
    janus_validate_paths(input_file, output_file, log_path);
    if (thread_count > 16) throw std::runtime_error("At most 16 compression workers are supported");
    StreamReader reader(input_file);
    if (!reader.is_open()) {
        std::cout << "[Janus Engine] Error: Cannot open input file.\n";
        return false;
    }

    size_t total_size = reader.file_size();
    if (total_size > JANUS_MAX_OUTPUT) throw std::runtime_error("Input exceeds 8 GiB limit");

    // Default to a conservative number of threads: half the logical cores.
    // This leaves headroom for I/O and the notation dictionary work on the
    // main thread, and avoids oversubscription on machines with SMT.
    if (thread_count == 0) {
        size_t hardware = std::thread::hardware_concurrency();
        if (hardware == 0) hardware = 1;
        thread_count = std::min(size_t{2}, (hardware > 1) ? (hardware / 2) : 1);
    }

    const size_t target_chunk_size = (thread_count > 1)
                                         ? JANUS_SEGMENT_SIZE_MULTI
                                         : JANUS_SEGMENT_SIZE_SINGLE;

    std::vector<uint8_t> header_sample(std::min(static_cast<size_t>(8192), total_size));
    if (reader.read(header_sample.data(), header_sample.size()) != header_sample.size() || !reader.seek(0))
        throw std::runtime_error("Input read failed");

    FileFormat format = JanusEngine::detect_format(input_file, header_sample.data(), header_sample.size());
    std::vector<SegmentBoundary> segments;

    if (format == FORMAT_SAFETENSORS) {
        segments = JanusEngine::split_safetensors(header_sample.data(), total_size, target_chunk_size);
    } else if (format == FORMAT_GGUF) {
        segments = JanusEngine::split_gguf(header_sample.data(), total_size, target_chunk_size);
    } else {
        segments = JanusEngine::calculate_segments(total_size, target_chunk_size);
    }

    StreamWriter writer(output_file);
    if (!writer.is_open()) {
        std::cout << "[Janus Engine] Error: Cannot create output file.\n";
        return false;
    }

    JanusHeader header;
    if (segments.size() > std::numeric_limits<uint16_t>::max()) throw std::runtime_error("Too many segments");
    header.magic = JANUS_MAGIC;
    header.version = JANUS_VERSION;
    header.container_mode = 0x02;
    header.segment_count = static_cast<uint16_t>(segments.size());
    for (const auto& segment : segments)
        if (segment.size == 0 || segment.size > JANUS_MAX_SEGMENT) throw std::runtime_error("Segment exceeds 16 MiB limit");

    writer.write(reinterpret_cast<uint8_t*>(&header), sizeof(JanusHeader));

    std::vector<JanusSegmentDescriptor> descriptors(segments.size());
    const size_t descriptors_size = sizeof(JanusSegmentDescriptor) * descriptors.size();
    writer.write(reinterpret_cast<const uint8_t*>(descriptors.data()), descriptors_size);

    std::vector<SegmentLogEntry> segment_log;
    segment_log.reserve(segments.size());

    size_t payload_offset = sizeof(JanusHeader) + sizeof(JanusSegmentDescriptor) * segments.size();
    g_bytes_processed = 0;
    Clock::time_point start_time = Clock::now();

    struct SegmentResult {
        std::vector<uint8_t> segment_payload;
        JanusSegmentDescriptor descriptor{};
        SegmentLogEntry seg_entry{};
        std::string notation_message;
        bool error = false;
    };

    auto process_segment = [&](size_t seg_idx, std::vector<uint8_t> segment_data) -> SegmentResult {
        SegmentResult res;
        const size_t original_size = segment_data.size();
        const auto& seg = segments[seg_idx];

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
                        res.notation_message = "[Janus Engine] Notation prefilter applied to segment " +
                                               std::to_string(seg_idx + 1) + ": " + format_bytes(seg.size) +
                                               " -> " + format_bytes(static_cast<size_t>(notation_size_after)) + "\n";
                    }
                }
            }
        }

        if (pref_id == PREFILTER_NOTATION) {
            notation_append_varint(res.segment_payload, static_cast<uint32_t>(notation_vocab.size()));
            if (!notation_vocab.empty()) {
                res.segment_payload.insert(res.segment_payload.end(), notation_vocab.begin(), notation_vocab.end());
            }
            notation_append_varint(res.segment_payload, static_cast<uint32_t>(prefiltered_data.size()));
            res.segment_payload.insert(res.segment_payload.end(), compressed_payload.begin(), compressed_payload.end());
        } else {
            res.segment_payload = std::move(compressed_payload);
        }

        if (res.segment_payload.size() > 0xFFFFFFFFull || seg.size > 0xFFFFFFFFull) {
            res.error = true;
            res.notation_message = "[Janus Engine] Error: Segment " + std::to_string(seg_idx + 1) +
                                   " too large for this container version.\n";
            return res;
        }

        res.descriptor.algo_id = static_cast<uint8_t>(payload_algo);
        res.descriptor.prefilter_id = static_cast<uint8_t>(pref_id);
        res.descriptor.compressed_size = static_cast<uint32_t>(res.segment_payload.size());
        res.descriptor.original_size = static_cast<uint32_t>(seg.size);
        res.descriptor.segment_crc32 = calculate_crc32(segment_data.data(), segment_data.size());

        res.seg_entry.index = seg_idx;
        res.seg_entry.original_size = original_size;
        res.seg_entry.compressed_size = res.segment_payload.size();
        res.seg_entry.algorithm = algo_name(static_cast<uint8_t>(payload_algo));
        res.seg_entry.prefilter = prefilter_name(static_cast<uint8_t>(pref_id));
        res.seg_entry.segment_crc32 = res.descriptor.segment_crc32;
        res.seg_entry.notation_applied = notation_applied;
        res.seg_entry.notation_size_before = seg.size;
        res.seg_entry.notation_size_after = notation_size_after;
        return res;
    };


    if (thread_count <= 1) {
        for (size_t seg_idx = 0; seg_idx < segments.size(); ++seg_idx) {
            const auto& seg = segments[seg_idx];
            std::vector<uint8_t> segment_data(seg.size);
            if (!reader.seek(seg.offset) || reader.read(segment_data.data(), seg.size) != seg.size)
                throw std::runtime_error("Input read failed");

            SegmentResult res = process_segment(seg_idx, std::move(segment_data));
            if (res.error) {
                std::cout << res.notation_message;
                return false;
            }
            if (!res.notation_message.empty()) {
                std::cout << res.notation_message;
            }

            res.seg_entry.payload_offset = payload_offset;
            descriptors[seg_idx] = res.descriptor;
            segment_log.push_back(res.seg_entry);
            writer.write(res.segment_payload.data(), res.segment_payload.size());
            payload_offset += res.segment_payload.size();

            g_bytes_processed.fetch_add(res.seg_entry.original_size, std::memory_order_relaxed);
            print_progress_atomic("Compressing", total_size, start_time);
        }
    } else if (!segments.empty()) {
        // Cap worker count to the number of segments; extra threads would just
        // sit idle and waste scheduler time.
        thread_count = std::min(thread_count, segments.size());
        ReorderBuffer<SegmentResult> results(segments.size());
        ThreadPool pool(thread_count);
        size_t scheduled = 0;

        for (size_t seg_idx = 0; seg_idx < segments.size(); ++seg_idx) {
            while (scheduled < segments.size() && scheduled - seg_idx < thread_count * 2) {
                const size_t index = scheduled++;
                const auto& segment = segments[index];
                std::vector<uint8_t> input(segment.size);
                if (!reader.seek(segment.offset) || reader.read(input.data(), input.size()) != input.size())
                    throw std::runtime_error("Input read failed");
                pool.submit([&, index, input = std::move(input)]() mutable {
                    try {
                        SegmentResult res = process_segment(index, std::move(input));
                        g_bytes_processed.fetch_add(res.seg_entry.original_size, std::memory_order_relaxed);
                        results.put(index, std::move(res));
                    } catch (...) {
                        results.fail(std::current_exception());
                    }
                });
            }
            SegmentResult res = results.take(seg_idx);
            if (res.error) {
                std::cout << res.notation_message;
                return false;
            }
            if (!res.notation_message.empty()) {
                std::cout << res.notation_message;
            }

            res.seg_entry.payload_offset = payload_offset;
            descriptors[seg_idx] = res.descriptor;
            segment_log.push_back(res.seg_entry);
            writer.write(res.segment_payload.data(), res.segment_payload.size());
            payload_offset += res.segment_payload.size();

            g_bytes_processed.fetch_add(res.seg_entry.original_size, std::memory_order_relaxed);
            print_progress_atomic("Compressing", total_size, start_time);
        }
    }

    clear_progress_line();

    writer.flush();

    size_t output_size = writer.tellp();

    writer.seekp(sizeof(JanusHeader));
    writer.write(reinterpret_cast<const uint8_t*>(descriptors.data()), descriptors_size);
    writer.commit();

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
    janus_validate_paths(input_file, output_file, log_path);
    StreamReader reader(input_file);
    if (!reader.is_open()) {
        std::cout << "[Janus Engine] Error: Cannot open archive file.\n";
        return false;
    }

    auto read_exact = [&](void* data, size_t size) {
        if (reader.read(static_cast<uint8_t*>(data), size) != size)
            throw std::runtime_error("Truncated archive");
    };
    JanusHeader header{};
    read_exact(&header, sizeof(header));

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
    if (header.container_mode != 2) throw std::runtime_error("Unsupported container mode");
    const size_t table_size = size_t(header.segment_count) *
        (header.version == 2 ? sizeof(JanusSegmentDescriptorV2) : sizeof(JanusSegmentDescriptor));
    const size_t metadata_size = sizeof(header) + (header.version == 2 ? sizeof(uint64_t) : 0) + table_size;
    if (metadata_size > reader.file_size()) throw std::runtime_error("Truncated descriptor table");
    std::vector<JanusSegmentDescriptor> descriptors(header.segment_count);

    if (header.version == 0x02) {
        uint64_t legacy_total_orig_size = 0;
        read_exact(&legacy_total_orig_size, sizeof(legacy_total_orig_size));
        total_orig_size = legacy_total_orig_size;

        std::vector<JanusSegmentDescriptorV2> legacy_descriptors(header.segment_count);
        read_exact(legacy_descriptors.data(), sizeof(JanusSegmentDescriptorV2) * header.segment_count);
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
        read_exact(descriptors.data(), sizeof(JanusSegmentDescriptor) * header.segment_count);
        for (size_t i = 0; i < header.segment_count; ++i) {
            total_orig_size += descriptors[i].original_size;
        }
    }

    uint64_t declared_total = 0, payload_total = 0;
    for (const auto& desc : descriptors) {
        if (desc.original_size == 0 || desc.original_size > JANUS_MAX_SEGMENT ||
            desc.compressed_size == 0 || desc.compressed_size > JANUS_MAX_NOTATION + 1024 * 1024 ||
            desc.algo_id > ALGO_LZMA_ULTRA || desc.prefilter_id > PREFILTER_NOTATION ||
            (header.version == 2 && desc.prefilter_id == PREFILTER_NOTATION))
            throw std::runtime_error("Invalid segment descriptor");
        declared_total += desc.original_size;
        payload_total += desc.compressed_size;
        if (declared_total > JANUS_MAX_OUTPUT) throw std::runtime_error("Decoded output exceeds 8 GiB limit");
    }
    if (header.version == 2 && declared_total != total_orig_size)
        throw std::runtime_error("Legacy archive size mismatch");
    total_orig_size = declared_total;
    if (payload_total != reader.file_size() - metadata_size)
        throw std::runtime_error("Truncated payload or trailing archive data");

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
        read_exact(compressed_payload.data(), desc.compressed_size);

        std::vector<uint8_t> original_data;

        if (desc.prefilter_id == PREFILTER_NOTATION) {
            bool notation_ok = true;
            size_t position = 0;
            uint32_t metadata_length = 0;
            uint32_t notation_length = 0;
            try {
                position += janus::VarintCodec::decode_unsigned(compressed_payload.data() + position,
                                                                compressed_payload.size() - position, metadata_length);
                if (metadata_length > 1024 * 1024 || metadata_length >= compressed_payload.size() - position) {
                    notation_ok = false;
                }
                if (notation_ok) {
                    const uint8_t* metadata = compressed_payload.data() + position;
                    position += metadata_length;
                    position += janus::VarintCodec::decode_unsigned(compressed_payload.data() + position,
                                                                    compressed_payload.size() - position, notation_length);
                    if (notation_length == 0 || notation_length > JANUS_MAX_NOTATION || position >= compressed_payload.size()) {
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
                                                     metadata, metadata_length, original_data, desc.original_size)) {
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
            const bool rle = desc.prefilter_id == PREFILTER_RLE;
            const size_t decode_limit = rle ? size_t(desc.original_size) * 2 : desc.original_size;

            if (desc.algo_id == ALGO_LZMA_ULTRA) {
                prefiltered_data = decompress_lzma(compressed_payload, decode_limit, !rle);
            } else if (desc.algo_id == ALGO_BROTLI_L11) {
                prefiltered_data = decompress_brotli(compressed_payload.data(), compressed_payload.size(), decode_limit, !rle);
            } else if (desc.algo_id == ALGO_STORE) {
                prefiltered_data = compressed_payload;
            }

            if (prefiltered_data.empty()) {
                std::cout << "[Janus Engine] Error: Decompression failed at segment " << (seg_idx + 1) << "\n";
                return false;
            }

            if (desc.prefilter_id == PREFILTER_RLE) {
                original_data = JanusEngine::undo_rle(prefiltered_data.data(), prefiltered_data.size(), desc.original_size);
            } else if (desc.prefilter_id == PREFILTER_DELTA) {
                original_data = JanusEngine::undo_delta(prefiltered_data.data(), prefiltered_data.size());
            } else {
                original_data = std::move(prefiltered_data);
            }
        }

        if (original_data.size() != desc.original_size) throw std::runtime_error("Decoded segment size mismatch");
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

    writer.commit();

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

#ifndef JANUS_LIBRARY_ONLY
int janus_main(int argc, char* argv[]) {
    try {
    if (argc < 3) {
        std::cout << "Janus Engine v2.0\n";
        std::cout << "Usage:\n";
        std::cout << "  Compress:   janus -c <input_path> [output_path] [--log <log.json>]\n";
        std::cout << "  Decompress: janus -d <input_path> [output_path] [--log <log.json>]\n";
        std::cout << "  Options:    --threads N  (1..16 workers, default: up to 2)\n";
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
    size_t thread_count = 0;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--log" && i + 1 < argc) {
            log_path = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument("Invalid thread count");
            thread_count = std::stoul(value);
            if (thread_count == 0 || thread_count > 16) throw std::out_of_range("Threads must be 1..16");
        } else if (arg == "-d" && (mode == "--raw-brotli" || mode == "--raw-lzma")) {
            raw_decompress = true;
        } else if (arg.rfind("--", 0) == 0) {
            throw std::invalid_argument("Unknown option or missing option value: " + arg);
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.empty() || positional.size() > 2) {
        std::cout << "Missing input path.\n";
        return 1;
    }

    std::string input_path = positional[0];
    std::string output_path;
    bool success = false;

    if (mode == "-c") {
        output_path = (positional.size() > 1) ? positional[1] : input_path + ".jan";
        success = compress_file(input_path, output_path, log_path, thread_count);
    } else if (mode == "-d") {
        output_path = (positional.size() > 1) ? positional[1] : input_path + ".unjan";
        success = decompress_file(input_path, output_path, log_path);
    } else if (mode == "--raw-brotli") {
        if (positional.size() < 2) {
            std::cout << "Missing output path.\n";
            return 1;
        }
        if (raw_decompress) {
            success = raw_brotli_decompress_file(input_path, positional[1]);
        } else {
            success = raw_brotli_compress_file(input_path, positional[1], log_path);
        }
    } else if (mode == "--raw-lzma") {
        if (positional.size() < 2) {
            std::cout << "Missing output path.\n";
            return 1;
        }
        if (raw_decompress) {
            success = raw_lzma_decompress_file(input_path, positional[1]);
        } else {
            success = raw_lzma_compress_file(input_path, positional[1], log_path);
        }
    } else {
        std::cout << "Unknown mode. Use -c for compression or -d for decompression.\n";
    }

    return success ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Janus: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Janus: unknown error\n";
        return 1;
    }
}
#endif // JANUS_LIBRARY_ONLY
