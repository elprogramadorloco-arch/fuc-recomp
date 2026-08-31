#include "iso9660.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr std::uint64_t sector_size = 2048;

std::uint32_t le32(const unsigned char* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::wstring clean_name(const unsigned char* p, std::size_t size) {
    std::string name(reinterpret_cast<const char*>(p), size);
    if (const auto semicolon = name.find(';'); semicolon != std::string::npos)
        name.resize(semicolon);
    while (!name.empty() && name.back() == '.') name.pop_back();
    if (name.empty() || name == "." || name == "..") return {};
    for (char& c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || u < 32) c = '_';
    }
    return std::wstring(name.begin(), name.end());
}

bool read_at(std::ifstream& input, std::uint64_t offset, void* data,
             std::size_t size) {
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset));
    input.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return input.good() || input.gcount() == static_cast<std::streamsize>(size);
}

bool extract_directory(std::ifstream& input, std::uint32_t extent,
                       std::uint32_t length, const fs::path& destination,
                       std::wstring& error, int depth) {
    if (depth > 32 || length > 64u * 1024u * 1024u) {
        error = L"La estructura del ISO no es valida.";
        return false;
    }
    std::vector<unsigned char> data(length);
    if (!read_at(input, std::uint64_t(extent) * sector_size, data.data(), data.size())) {
        error = L"No se pudo leer un directorio del ISO.";
        return false;
    }
    fs::create_directories(destination);
    std::size_t pos = 0;
    while (pos < data.size()) {
        const std::uint8_t record_length = data[pos];
        if (record_length == 0) {
            pos = ((pos / sector_size) + 1) * sector_size;
            continue;
        }
        if (pos + record_length > data.size() || record_length < 34) {
            error = L"El directorio ISO9660 esta truncado.";
            return false;
        }
        const auto* record = data.data() + pos;
        const std::uint8_t name_length = record[32];
        if (33u + name_length <= record_length &&
            !(name_length == 1 && (record[33] == 0 || record[33] == 1))) {
            const auto name = clean_name(record + 33, name_length);
            if (!name.empty()) {
                const fs::path output = destination / name;
                const auto child_extent = le32(record + 2);
                const auto child_length = le32(record + 10);
                const bool directory = (record[25] & 2) != 0;
                if (directory) {
                    if (!extract_directory(input, child_extent, child_length, output,
                                           error, depth + 1)) return false;
                } else {
                    fs::create_directories(output.parent_path());
                    std::ofstream out(output, std::ios::binary | std::ios::trunc);
                    std::array<char, 64 * 1024> buffer{};
                    std::uint64_t remaining = child_length;
                    std::uint64_t offset = std::uint64_t(child_extent) * sector_size;
                    while (remaining) {
                        const auto chunk = static_cast<std::size_t>(
                            std::min<std::uint64_t>(remaining, buffer.size()));
                        if (!read_at(input, offset, buffer.data(), chunk)) {
                            error = L"No se pudo leer un archivo del ISO.";
                            return false;
                        }
                        out.write(buffer.data(), static_cast<std::streamsize>(chunk));
                        remaining -= chunk;
                        offset += chunk;
                    }
                    if (!out) {
                        error = L"No se pudo escribir el contenido extraido.";
                        return false;
                    }
                }
            }
        }
        pos += record_length;
    }
    return true;
}
}

namespace fuc_public {
bool extract_iso9660(const fs::path& iso, const fs::path& destination,
                     std::wstring& error) {
    std::ifstream input(iso, std::ios::binary);
    if (!input) { error = L"No se pudo abrir el ISO."; return false; }
    std::array<unsigned char, sector_size> pvd{};
    if (!read_at(input, 16 * sector_size, pvd.data(), pvd.size()) ||
        pvd[0] != 1 || std::string(reinterpret_cast<char*>(pvd.data() + 1), 5) != "CD001") {
        error = L"El archivo no contiene un volumen ISO9660 PSP reconocible.";
        return false;
    }
    const auto* root = pvd.data() + 156;
    return extract_directory(input, le32(root + 2), le32(root + 10),
                             destination, error, 0);
}
}
