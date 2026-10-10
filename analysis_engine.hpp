#ifndef ANALYSIS_ENGINE_HPP
#define ANALYSIS_ENGINE_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <vector>

// Abstract analysis backend. The CPU implementation is used today; the GPU
// stub provides a compile-time extension point for future CUDA / Vulkan Compute
// histogram / dictionary analysis kernels.
class IAnalysisEngine {
public:
    virtual ~IAnalysisEngine() = default;

    // Order-0 Shannon entropy in bits per byte.
    virtual double compute_entropy(const uint8_t* data, size_t size) = 0;

    // Primary notation-dictionary analysis. Returns true if the engine produced
    // a vocabulary + transformed body itself. Returning false tells the caller
    // to fall back to the CPU notation prefilter path.
    virtual bool analyze_notation_dictionary(const uint8_t* data, size_t size,
                                             std::vector<uint8_t>& out_vocab,
                                             std::vector<uint8_t>& out_body) = 0;
};

class CPUAnalysisEngine : public IAnalysisEngine {
public:
    double compute_entropy(const uint8_t* data, size_t size) override {
        if (size == 0) return 0.0;

        size_t count[256] = {0};
        for (size_t i = 0; i < size; ++i) {
            count[data[i]]++;
        }

        double entropy = 0.0;
        const double inv_size = 1.0 / static_cast<double>(size);
        for (int i = 0; i < 256; ++i) {
            if (count[i] > 0) {
                double p = static_cast<double>(count[i]) * inv_size;
                entropy -= p * std::log2(p);
            }
        }
        return entropy;
    }

    bool analyze_notation_dictionary(const uint8_t* /*data*/, size_t /*size*/,
                                     std::vector<uint8_t>& /*out_vocab*/,
                                     std::vector<uint8_t>& /*out_body*/) override {
        // The CPU pipeline performs dictionary analysis inside the notation
        // prefilter encoder (notation_prefilter.hpp). Returning false here
        // keeps the existing CPU path untouched.
        return false;
    }
};

class GPUAnalysisEngine : public IAnalysisEngine {
public:
    double compute_entropy(const uint8_t* /*data*/, size_t /*size*/) override {
        std::cerr << "[Janus Analysis] GPU entropy engine is a stub; "
                     "falling back to CPU entropy estimation.\n";
        return 0.0;
    }

    bool analyze_notation_dictionary(const uint8_t* /*data*/, size_t /*size*/,
                                     std::vector<uint8_t>& /*out_vocab*/,
                                     std::vector<uint8_t>& /*out_body*/) override {
        std::cerr << "[Janus Analysis] GPU notation dictionary engine is a stub; "
                     "falling back to CPU prefilter path.\n";
        return false;
    }
};

#endif // ANALYSIS_ENGINE_HPP
