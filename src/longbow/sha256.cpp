#include "longbow/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace flecha::longbow {
namespace fs = std::filesystem;
namespace {
using namespace std::string_view_literals;

class Hasher final {
   public:
    void Update(std::string_view input) {
        for (const unsigned char byte : input) {
            _buffer[_bufferSize++] = byte;
            if (_bufferSize == _buffer.size()) Transform();
        }
        _bytes += input.size();
    }

    std::string Finish() {
        const std::uint64_t bits = _bytes * 8;
        _buffer[_bufferSize++] = 0x80;
        if (_bufferSize > 56) {
            while (_bufferSize < 64) _buffer[_bufferSize++] = 0;
            Transform();
        }
        while (_bufferSize < 56) _buffer[_bufferSize++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8)
            _buffer[_bufferSize++] = static_cast<std::uint8_t>(bits >> shift);
        Transform();
        std::ostringstream result;
        result << std::hex << std::setfill('0');
        for (const std::uint32_t word : _state) result << std::setw(8) << word;
        return result.str();
    }

   private:
    static std::uint32_t Rotate(std::uint32_t value, unsigned count) {
        return (value >> count) | (value << (32 - count));
    }
    void Transform() {
        static constexpr std::array<std::uint32_t, 64> constants = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
            0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
            0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
            0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::array<std::uint32_t, 64> words{};
        for (unsigned index = 0; index < 16; ++index)
            words[index] =
                (static_cast<std::uint32_t>(_buffer[index * 4]) << 24) |
                (static_cast<std::uint32_t>(_buffer[index * 4 + 1]) << 16) |
                (static_cast<std::uint32_t>(_buffer[index * 4 + 2]) << 8) |
                _buffer[index * 4 + 3];
        for (unsigned index = 16; index < 64; ++index) {
            const std::uint32_t first = Rotate(words[index - 15], 7) ^
                                        Rotate(words[index - 15], 18) ^
                                        (words[index - 15] >> 3);
            const std::uint32_t second = Rotate(words[index - 2], 17) ^
                                         Rotate(words[index - 2], 19) ^
                                         (words[index - 2] >> 10);
            words[index] =
                words[index - 16] + first + words[index - 7] + second;
        }
        auto [a, b, c, d, e, f, g, h] = _state;
        for (unsigned index = 0; index < 64; ++index) {
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t first =
                h + (Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25)) + choice +
                constants[index] + words[index];
            const std::uint32_t second =
                (Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22)) + majority;
            h = g;
            g = f;
            f = e;
            e = d + first;
            d = c;
            c = b;
            b = a;
            a = first + second;
        }
        _state[0] += a;
        _state[1] += b;
        _state[2] += c;
        _state[3] += d;
        _state[4] += e;
        _state[5] += f;
        _state[6] += g;
        _state[7] += h;
        _bufferSize = 0;
    }

    std::array<std::uint32_t, 8> _state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                           0xa54ff53a, 0x510e527f, 0x9b05688c,
                                           0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> _buffer{};
    std::size_t _bufferSize = 0;
    std::uint64_t _bytes = 0;
};

void UpdateFile(Hasher& hasher, const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read " + file.string());
    std::array<char, 64 * 1024> buffer{};
    while (stream) {
        stream.read(buffer.data(), buffer.size());
        hasher.Update(std::string_view(buffer.data(), stream.gcount()));
    }
    if (!stream.eof()) throw std::runtime_error("cannot read " + file.string());
}

}  // namespace

std::string Sha256(std::string_view bytes) {
    Hasher hasher;
    hasher.Update(bytes);
    return hasher.Finish();
}

std::string Sha256File(const fs::path& file) {
    Hasher hasher;
    UpdateFile(hasher, file);
    return hasher.Finish();
}

std::string Sha256Tree(const fs::path& directory) {
    if (!fs::is_directory(directory))
        throw std::runtime_error("cannot hash non-directory " +
                                 directory.string());
    std::vector<fs::path> files;
    for (const auto& item : fs::recursive_directory_iterator(directory)) {
        const fs::file_status status = item.symlink_status();
        if (fs::is_symlink(status))
            throw std::runtime_error("source tree contains symbolic link " +
                                     item.path().string());
        if (fs::is_regular_file(status)) files.push_back(item.path());
    }
    std::sort(files.begin(), files.end(),
              [&](const fs::path& left, const fs::path& right) {
                  return left.lexically_relative(directory).generic_string() <
                         right.lexically_relative(directory).generic_string();
              });
    Hasher hasher;
    for (const fs::path& file : files) {
        const std::string relative =
            file.lexically_relative(directory).generic_string();
        hasher.Update("path\0"sv);
        hasher.Update(relative);
        hasher.Update("\0content\0"sv);
        UpdateFile(hasher, file);
        hasher.Update("\0end\0"sv);
    }
    return hasher.Finish();
}

}  // namespace flecha::longbow
