#include "iso9660.hpp"

#include <filesystem>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 4 || std::wstring(argv[1]) != L"extract-iso") {
        std::wcerr << L"Usage: FateUnlimitedCodesDataTool extract-iso <game.iso> <destination>\n";
        return 2;
    }
    std::wstring error;
    if (!fuc_public::extract_iso9660(std::filesystem::path(argv[2]),
                                    std::filesystem::path(argv[3]), error)) {
        std::wcerr << error << L'\n';
        return 1;
    }
    std::wcout << L"ISO extracted successfully.\n";
    return 0;
}
