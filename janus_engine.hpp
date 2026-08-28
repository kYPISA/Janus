#ifndef JANUS_ENGINE_HPP
#define JANUS_ENGINE_HPP

#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <algorithm>
#include <unordered_map>

constexpr uint32_t JANUS_MAGIC = 0x53554E41;
constexpr uint8_t  JANUS_VERSION = 0x02;

enum JanusAlgoID : uint8_t {
    ALGO_STORE        = 0,
    ALGO_BROTLI_L11   = 1,
    ALGO_LZMA_ULTRA   = 2
};

enum JanusPrefilterID : uint8_t {
    PREFILTER_NONE  = 0,
    PREFILTER_RLE   = 1,
    PREFILTER_DELTA = 2
};

#pragma pack(push, 1)
struct JanusHeader {
    uint32_t magic;
    uint8_t  version;
    uint8_t  container_mode;
    uint16_t segment_count;
    uint64_t total_orig_size;
};

struct JanusSegmentDescriptor {
    uint64_t offset;
    uint8_t  algo_id;
    uint8_t  prefilter_id;
    uint64_t compressed_size;
    uint64_t original_size;
    uint32_t segment_crc32;
};
#pragma pack(pop)

struct SegmentBoundary {
    size_t offset;
    size_t size;
};

enum FileFormat {
    FORMAT_GENERIC,
    FORMAT_SAFETENSORS,
    FORMAT_GGUF
};

class JanusEngine {
public:
    static double calculate_entropy(const uint8_t* data, size_t size) {
        if (size == 0) return 0.0;
        size_t count[256] = {0};
        for (size_t i = 0; i < size; ++i) {
            count[data[i]]++;
        }
        double entropy = 0.0;
        for (int i = 0; i < 256; ++i) {
            if (count[i] > 0) {
                double p = static_cast<double>(count[i]) / size;
                entropy -= p * (std::log2(p));
            }
        }
        return entropy;
    }

    static std::vector<uint8_t> apply_rle(const uint8_t* src, size_t size) {
        std::vector<uint8_t> dest;
        dest.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            uint8_t count = 1;
            while (i + 1 < size && src[i] == src[i + 1] && count < 255) {
                count++;
                i++;
            }
            dest.push_back(count);
            dest.push_back(src[i]);
            if (dest.size() >= size) {
                return std::vector<uint8_t>(src, src + size);
            }
        }
        if (dest.size() >= size) {
            return std::vector<uint8_t>(src, src + size);
        }
        return dest;
    }

    static std::vector<uint8_t> undo_rle(const uint8_t* src, size_t size) {
        std::vector<uint8_t> dest;
        for (size_t i = 0; i < size; i += 2) {
            uint8_t count = src[i];
            uint8_t val = src[i + 1];
            dest.insert(dest.end(), count, val);
        }
        return dest;
    }

    static std::vector<uint8_t> apply_delta(const uint8_t* src, size_t size) {
        std::vector<uint8_t> dest(size);
        if (size == 0) return dest;
        dest[0] = src[0];
        for (size_t i = 1; i < size; ++i) {
            dest[i] = static_cast<uint8_t>(src[i] - src[i - 1]);
        }
        return dest;
    }

    static std::vector<uint8_t> undo_delta(const uint8_t* src, size_t size) {
        std::vector<uint8_t> dest(size);
        if (size == 0) return dest;
        dest[0] = src[0];
        for (size_t i = 1; i < size; ++i) {
            dest[i] = static_cast<uint8_t>(dest[i - 1] + src[i]);
        }
        return dest;
    }

    static JanusPrefilterID evaluate_and_apply_prefilter(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
        size_t sample_size = std::min(input.size(), static_cast<size_t>(262144));
        double orig_entropy = calculate_entropy(input.data(), sample_size);

        auto rle_sample = apply_rle(input.data(), sample_size);
        double rle_entropy = calculate_entropy(rle_sample.data(), rle_sample.size());

        auto delta_sample = apply_delta(input.data(), sample_size);
        double delta_entropy = calculate_entropy(delta_sample.data(), delta_sample.size());

        if (rle_sample.size() < sample_size * 0.85) {
            output = apply_rle(input.data(), input.size());
            if (output.size() < input.size()) {
                return PREFILTER_RLE;
            }
        }

        if (delta_entropy < orig_entropy - 0.25) {
            output = apply_delta(input.data(), input.size());
            return PREFILTER_DELTA;
        }

        output = input;
        return PREFILTER_NONE;
    }

    static JanusAlgoID select_compression_algorithm(const std::string& filepath, const uint8_t* data_sample, size_t sample_size) {
        std::string ext = "";
        size_t dot_pos = filepath.find_last_of('.');
        if (dot_pos != std::string::npos) {
            ext = filepath.substr(dot_pos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        }

        if (ext == ".txt" || ext == ".json" || ext == ".lua" ||
            ext == ".xml" || ext == ".html" || ext == ".css" ||
            ext == ".js" || ext == ".md" || ext == ".yaml" || ext == ".yml") {
            return ALGO_BROTLI_L11;
        }

        if (ext == ".bin" || ext == ".safetensors" || ext == ".gguf" ||
            ext == ".pth" || ext == ".pt" || ext == ".ckpt") {
            return ALGO_LZMA_ULTRA;
        }

        if (sample_size > 0 && data_sample != nullptr) {
            double entropy = calculate_entropy(data_sample, sample_size);
            if (entropy > 7.5) {
                return ALGO_LZMA_ULTRA;
            } else {
                return ALGO_BROTLI_L11;
            }
        }

        return ALGO_LZMA_ULTRA;
    }

    static std::vector<SegmentBoundary> calculate_segments(size_t file_size, size_t target_chunk_size = 134217728) {
        std::vector<SegmentBoundary> segments;

        if (file_size == 0) return segments;

        if (file_size <= target_chunk_size) {
            segments.push_back({0, file_size});
            return segments;
        }

        size_t num_segments = (file_size + target_chunk_size - 1) / target_chunk_size;

        if (file_size % target_chunk_size < target_chunk_size / 4 && num_segments > 1) {
            num_segments--;
        }

        size_t base_size = file_size / num_segments;
        size_t remainder = file_size % num_segments;
        size_t current_offset = 0;

        for (size_t i = 0; i < num_segments; ++i) {
            size_t segment_size = base_size + (i < remainder ? 1 : 0);
            segments.push_back({current_offset, segment_size});
            current_offset += segment_size;
        }

        return segments;
    }

    static FileFormat detect_format(const std::string& filepath, const uint8_t* header, size_t header_size) {
        std::string ext = "";
        size_t dot_pos = filepath.find_last_of('.');
        if (dot_pos != std::string::npos) {
            ext = filepath.substr(dot_pos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        }

        if (ext == ".safetensors") return FORMAT_SAFETENSORS;
        if (ext == ".gguf") return FORMAT_GGUF;

        if (header_size >= 4) {
            if (header[0] == 'G' && header[1] == 'G' && header[2] == 'U' && header[3] == 'F') {
                return FORMAT_GGUF;
            }
        }

        return FORMAT_GENERIC;
    }

    static std::vector<SegmentBoundary> split_safetensors(const uint8_t* file_data, size_t file_size) {
        std::vector<SegmentBoundary> segments;

        if (file_size < 8) {
            return calculate_segments(file_size);
        }

        uint64_t json_length = 0;
        std::memcpy(&json_length, file_data, sizeof(uint64_t));

        if (json_length > file_size - 8 || json_length == 0) {
            return calculate_segments(file_size);
        }

        size_t header_end = 8 + static_cast<size_t>(json_length);
        segments.push_back({0, header_end});

        size_t remaining = file_size - header_end;
        if (remaining > 0) {
            auto tensor_segments = calculate_segments(remaining, 134217728);
            for (auto& seg : tensor_segments) {
                seg.offset += header_end;
                segments.push_back(seg);
            }
        }

        return segments;
    }

    static std::vector<SegmentBoundary> split_gguf(const uint8_t* file_data, size_t file_size) {
        std::vector<SegmentBoundary> segments;

        if (file_size < 16) {
            return calculate_segments(file_size);
        }

        uint32_t version = 0;
        std::memcpy(&version, file_data + 4, sizeof(uint32_t));

        if (version > 10) {
            return calculate_segments(file_size);
        }

        size_t estimated_header_size = 1024;
        if (estimated_header_size > file_size) {
            estimated_header_size = file_size;
        }

        segments.push_back({0, estimated_header_size});

        if (file_size > estimated_header_size) {
            size_t remaining = file_size - estimated_header_size;
            auto tensor_segments = calculate_segments(remaining, 134217728);
            for (auto& seg : tensor_segments) {
                seg.offset += estimated_header_size;
                segments.push_back(seg);
            }
        }

        return segments;
    }
};

#endif