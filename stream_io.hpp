#ifndef STREAM_IO_HPP
#define STREAM_IO_HPP

#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <iostream>

class StreamReader {
public:
    explicit StreamReader(const std::string& filepath, size_t buffer_size = 16777216)
        : buffer_size_(buffer_size), file_size_(0), current_pos_(0) {
        file_.open(filepath, std::ios::binary | std::ios::ate);
        if (file_.is_open()) {
            file_size_ = static_cast<size_t>(file_.tellg());
            file_.seekg(0, std::ios::beg);
        }
        buffer_.resize(buffer_size_);
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

        file_.read(reinterpret_cast<char*>(dest), count);
        size_t bytes_read = static_cast<size_t>(file_.gcount());
        current_pos_ += bytes_read;
        return bytes_read;
    }

    bool seek(size_t offset) {
        if (!file_.is_open()) return false;
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
    size_t buffer_size_;
    size_t file_size_;
    size_t current_pos_;
    std::vector<uint8_t> buffer_;
};

class StreamWriter {
public:
    explicit StreamWriter(const std::string& filepath, size_t buffer_size = 16777216)
        : buffer_size_(buffer_size), buffer_pos_(0) {
        file_.open(filepath, std::ios::binary);
        buffer_.resize(buffer_size_);
    }

    ~StreamWriter() {
        flush();
        if (file_.is_open()) {
            file_.close();
        }
    }

    bool is_open() const {
        return file_.is_open();
    }

    void write(const uint8_t* src, size_t count) {
        if (!file_.is_open()) return;

        size_t remaining = count;
        size_t src_offset = 0;

        while (remaining > 0) {
            size_t available = buffer_size_ - buffer_pos_;
            size_t to_copy = std::min(remaining, available);

            std::memcpy(buffer_.data() + buffer_pos_, src + src_offset, to_copy);
            buffer_pos_ += to_copy;
            src_offset += to_copy;
            remaining -= to_copy;

            if (buffer_pos_ >= buffer_size_) {
                flush();
            }
        }
    }

    void flush() {
        if (!file_.is_open() || buffer_pos_ == 0) return;
        file_.write(reinterpret_cast<char*>(buffer_.data()), buffer_pos_);
        buffer_pos_ = 0;
    }

    void seekp(size_t offset) {
        flush();
        file_.seekp(offset, std::ios::beg);
    }

    size_t tellp() {
        return static_cast<size_t>(file_.tellp()) + buffer_pos_;
    }

private:
    std::ofstream file_;
    size_t buffer_size_;
    size_t buffer_pos_;
    std::vector<uint8_t> buffer_;
};

#endif