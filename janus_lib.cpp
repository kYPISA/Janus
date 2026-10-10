#include "janus_lib.h"

#include <string>
#include <exception>
#include <mutex>

// Defined in main.cpp, linked through janus_core by both the CLI and library.
bool compress_file(const std::string& input_file, const std::string& output_file,
                   const std::string& log_path, size_t thread_count);
bool decompress_file(const std::string& input_file, const std::string& output_file,
                     const std::string& log_path);

namespace {
std::mutex api_mutex;
thread_local char last_error[512] = {};
void set_error(const char* message) noexcept {
    size_t i = 0;
    while (message[i] && i + 1 < sizeof(last_error)) {
        last_error[i] = message[i];
        ++i;
    }
    last_error[i] = '\0';
}
}
extern "C" {

const char* janus_last_error(void) { return last_error; }

const char* janus_version_string(void) {
    return "Janus Engine 0.2.0 (container v3)";
}

int janus_compress_file(const char* input_path, const char* output_path,
                        const char* log_path, int thread_count) {
    last_error[0] = '\0';
    if (!input_path || !output_path || !*input_path || !*output_path || thread_count < 0) {
        set_error("Invalid paths or thread count");
        return 0;
    }
    try {
        std::lock_guard<std::mutex> lock(api_mutex);
        size_t threads = 0;
        if (thread_count > 0) {
            threads = static_cast<size_t>(thread_count);
        }
        const bool ok = compress_file(std::string(input_path), std::string(output_path),
                             log_path ? std::string(log_path) : std::string(),
                             threads);
        if (!ok) set_error("Compression failed");
        return ok ? 1 : 0;
    } catch (const std::exception& e) {
        set_error(e.what());
        return 0;
    } catch (...) {
        set_error("Unknown compression error");
        return 0;
    }
}

int janus_decompress_file(const char* input_path, const char* output_path,
                          const char* log_path) {
    last_error[0] = '\0';
    if (!input_path || !output_path || !*input_path || !*output_path) {
        set_error("Invalid paths");
        return 0;
    }
    try {
        std::lock_guard<std::mutex> lock(api_mutex);
        const bool ok = decompress_file(std::string(input_path), std::string(output_path),
                                       log_path ? std::string(log_path) : std::string());
        if (!ok) set_error("Decompression failed: invalid archive or file error");
        return ok ? 1 : 0;
    } catch (const std::exception& e) {
        set_error(e.what());
        return 0;
    } catch (...) {
        set_error("Unknown decompression error");
        return 0;
    }
}

} // extern "C"
