#ifndef STREAM_IO_HPP
#define STREAM_IO_HPP

#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <filesystem>
#include <limits>
#include <random>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

inline bool janus_same_file(const std::string& a, const std::string& b) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::equivalent(fs::path(a), fs::path(b), ec) && !ec) return true;
    return fs::weakly_canonical(fs::path(a)) == fs::weakly_canonical(fs::path(b));
}

inline void janus_validate_paths(const std::string& input, const std::string& output,
                                 const std::string& log = "") {
    if (input.empty() || output.empty() || janus_same_file(input, output) ||
        (!log.empty() && (janus_same_file(input, log) || janus_same_file(output, log)))) {
        throw std::runtime_error("Input, output and log must be distinct files");
    }
}

class StreamReader {
public:
    explicit StreamReader(const std::string& filepath, size_t buffer_size = 16777216)
        : file_size_(0), current_pos_(0) {
        (void)buffer_size;
        file_.open(filepath, std::ios::binary | std::ios::ate);
        if (file_.is_open()) {
            const auto size = file_.tellg();
            if (size < 0 || static_cast<uint64_t>(size) > std::numeric_limits<size_t>::max()) {
                throw std::runtime_error("Cannot determine input file size");
            }
            file_size_ = static_cast<size_t>(size);
            file_.seekg(0, std::ios::beg);
        }
    }

    ~StreamReader() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    bool is_open() const {
        return file_.is_open();
    }

    size_t read(uint8_t* dest, size_t count) {
        if (!file_.is_open()) return 0;
        if (count > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
            throw std::runtime_error("Read is too large");
        }

        file_.read(reinterpret_cast<char*>(dest), count);
        size_t bytes_read = static_cast<size_t>(file_.gcount());
        current_pos_ += bytes_read;
        return bytes_read;
    }

    bool seek(size_t offset) {
        if (!file_.is_open() || offset > file_size_) return false;
        file_.clear();
        file_.seekg(offset, std::ios::beg);
        current_pos_ = offset;
        return !file_.fail();
    }

    size_t file_size() const {
        return file_size_;
    }

    size_t tell() const {
        return current_pos_;
    }

private:
    std::ifstream file_;
    size_t file_size_;
    size_t current_pos_;
};

class StreamWriter {
public:
    explicit StreamWriter(const std::string& filepath, size_t buffer_size = 16777216)
        : destination_(std::filesystem::absolute(filepath)) {
        (void)buffer_size;
        namespace fs = std::filesystem;
        const auto status = fs::symlink_status(destination_);
        if (fs::is_symlink(status) || (fs::exists(status) && !fs::is_regular_file(status))) {
            throw std::runtime_error("Output must be a regular file, not a symlink");
        }
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto candidate = destination_.parent_path() / (".janus-tmp-" + std::to_string(random()) + "-" + std::to_string(random()));
            std::error_code ec;
            if (!fs::create_directory(candidate, ec)) continue;
            temporary_dir_ = candidate;
            fs::permissions(candidate, fs::perms::owner_all, fs::perm_options::replace, ec);
            if (ec) {
                fs::remove(candidate, ec);
                throw std::runtime_error("Cannot protect temporary output directory");
            }
            temporary_file_ = candidate / "output";
            file_.open(temporary_file_, std::ios::binary | std::ios::trunc);
            return;
        }
        throw std::runtime_error("Cannot create temporary output beside destination");
    }

    ~StreamWriter() {
        file_.close();
        std::error_code ec;
        if (!temporary_file_.empty()) std::filesystem::remove(temporary_file_, ec);
        if (!temporary_dir_.empty()) std::filesystem::remove(temporary_dir_, ec);
    }

    bool is_open() const {
        return file_.is_open();
    }

    void write(const uint8_t* src, size_t count) {
        if (count > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
            throw std::runtime_error("Write is too large");
        if (count) file_.write(reinterpret_cast<const char*>(src), static_cast<std::streamsize>(count));
        if (!file_) throw std::runtime_error("Output write failed");
    }

    void flush() {
        file_.flush();
        if (!file_) throw std::runtime_error("Output flush failed");
    }

    void commit() {
        flush();
        file_.close();
        if (file_.fail()) throw std::runtime_error("Output close failed");
#ifdef _WIN32
        if (!MoveFileExW(temporary_file_.c_str(), destination_.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            throw std::runtime_error("Cannot replace output file");
        }
#else
        std::filesystem::rename(temporary_file_, destination_);
#endif
        temporary_file_.clear();
    }

    void seekp(size_t offset) {
        flush();
        file_.seekp(offset, std::ios::beg);
        if (!file_) throw std::runtime_error("Output seek failed");
    }

    size_t tellp() {
        const auto position = file_.tellp();
        if (position < 0) throw std::runtime_error("Cannot determine output position");
        return static_cast<size_t>(position);
    }

private:
    std::ofstream file_;
    std::filesystem::path destination_, temporary_dir_, temporary_file_;
};

#endif