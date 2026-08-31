#pragma once

#include <filesystem>
#include <string>

namespace fuc_public {

bool extract_iso9660(const std::filesystem::path& iso,
                     const std::filesystem::path& destination,
                     std::wstring& error);

}
