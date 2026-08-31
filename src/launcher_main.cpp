#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <xinput.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

namespace {
std::wstring window_title = L"FateUnlimitedCodesRecomp Configuration";
constexpr UINT wm_check_update = WM_APP + 7;
constexpr int action_count = 9;
constexpr std::array<const wchar_t*, action_count> action_names{
    L"Up", L"Down", L"Left", L"Right", L"Light Attack", L"Medium Attack",
    L"Heavy Attack", L"Reflect Guard", L"Start"
};
constexpr std::array<int, action_count> default_keys{
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'A', 'S', 'D', 'Z', VK_RETURN
};
constexpr std::array<WORD, action_count> default_pad{
    XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
    XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
    XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_B,
    XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_START
};

enum : int {
    id_tab = 100, id_scale, id_fullscreen,
    id_vsync, id_open_data, id_prepare, id_reset, id_apply, id_play, id_exit,
    id_keyboard_base = 1000, id_gamepad_base = 1100
};

LRESULT CALLBACK input_page_proc(HWND hwnd, UINT message, WPARAM wparam,
                                 LPARAM lparam, UINT_PTR, DWORD_PTR) {
    // Button notifications are sent to the immediate parent. The input page is
    // a child panel, so forward real clicks to the launcher window explicitly.
    if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED) {
        SendMessageW(GetParent(hwnd), WM_COMMAND, wparam, lparam);
        return 0;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

fs::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    buffer.resize(size);
    return fs::path(buffer).parent_path();
}

std::wstring key_name(int key) {
    if (!key) return L"Unassigned";
    const UINT scan = MapVirtualKeyW(UINT(key), MAPVK_VK_TO_VSC) << 16;
    wchar_t name[128]{};
    if (GetKeyNameTextW(LONG(scan), name, 128)) return name;
    return L"Key " + std::to_wstring(key);
}

std::wstring pad_name(WORD value) {
    switch (value) {
    case XINPUT_GAMEPAD_DPAD_UP: return L"D-Pad Up";
    case XINPUT_GAMEPAD_DPAD_DOWN: return L"D-Pad Down";
    case XINPUT_GAMEPAD_DPAD_LEFT: return L"D-Pad Left";
    case XINPUT_GAMEPAD_DPAD_RIGHT: return L"D-Pad Right";
    case XINPUT_GAMEPAD_START: return L"Menu / Start";
    case XINPUT_GAMEPAD_BACK: return L"View / Back";
    case XINPUT_GAMEPAD_LEFT_THUMB: return L"Left Stick Click";
    case XINPUT_GAMEPAD_RIGHT_THUMB: return L"Right Stick Click";
    case XINPUT_GAMEPAD_LEFT_SHOULDER: return L"Left Shoulder";
    case XINPUT_GAMEPAD_RIGHT_SHOULDER: return L"Right Shoulder";
    case XINPUT_GAMEPAD_A: return L"A";
    case XINPUT_GAMEPAD_B: return L"B";
    case XINPUT_GAMEPAD_X: return L"X";
    case XINPUT_GAMEPAD_Y: return L"Y";
    default: return value ? L"Button " + std::to_wstring(value) : L"Unassigned";
    }
}

std::wstring quote(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }

bool is_arrow_key(const WPARAM key) {
    return key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN;
}

bool run_process(std::wstring command, const fs::path& working_directory,
                 bool wait, DWORD* exit_code = nullptr, DWORD creation_flags = 0) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, creation_flags, nullptr,
                        working_directory.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (exit_code) GetExitCodeProcess(pi.hProcess, exit_code);
    }
    CloseHandle(pi.hProcess);
    return true;
}

std::wstring read_utf8_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    if (bytes.empty()) return {};
    size_t offset = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
                    static_cast<unsigned char>(bytes[1]) == 0xBB &&
                    static_cast<unsigned char>(bytes[2]) == 0xBF ? 3 : 0;
    const int size = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + offset,
                                         int(bytes.size() - offset), nullptr, 0);
    std::wstring result(size, L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, 0, bytes.data() + offset,
                                  int(bytes.size() - offset), result.data(), size);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}

std::wstring configured_window_title() {
    const auto version = read_utf8_text(executable_directory() / L"version.txt");
    if (version.empty()) return L"FateUnlimitedCodesRecomp Configuration";
    return L"FateUnlimitedCodesRecomp - v" + version + L" Configuration";
}

std::optional<fs::path> find_file(const fs::path& root,
                                  const std::vector<std::wstring>& extensions,
                                  const std::wstring& contains = {}) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return std::nullopt;
    for (const auto& entry : fs::recursive_directory_iterator(root,
             fs::directory_options::skip_permission_denied, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        auto ext = entry.path().extension().wstring();
        auto filename = entry.path().filename().wstring();
        CharLowerBuffW(ext.data(), DWORD(ext.size()));
        CharLowerBuffW(filename.data(), DWORD(filename.size()));
        bool extension_ok = extensions.empty();
        for (const auto& expected : extensions) if (ext == expected) extension_ok = true;
        if (extension_ok && (contains.empty() || filename.find(contains) != std::wstring::npos))
            return entry.path();
    }
    return std::nullopt;
}

struct App {
    HINSTANCE instance{};
    HWND window{}, tab{}, graphics_page{}, inputs_page{}, status{};
    HWND scale{}, fullscreen{};
    std::array<HWND, action_count> keyboard_buttons{}, gamepad_buttons{};
    std::array<std::array<int, action_count>, 2> keys{};
    std::array<std::array<WORD, action_count>, 2> pads{};
    int player = 0;
    int capture_action = -1;
    bool capture_gamepad = false;
    WORD previous_pad = 0;
    std::array<bool, 256> previous_keys{};
    fs::path root, data, save;

    void defaults() {
        for (int p = 0; p < 2; ++p) {
            keys[p] = default_keys;
            pads[p] = default_pad;
        }
    }

    void load_config() {
        std::ifstream input(save / L"input.toml");
        std::string line;
        int current_player = -1;
        while (std::getline(input, line)) {
            if (line == "[player1]") { current_player = 0; continue; }
            if (line == "[player2]") { current_player = 1; continue; }
            if (current_player < 0) continue;
            int action = -1, value = 0;
            if (sscanf_s(line.c_str(), "action%d_keyboard = %d", &action, &value) == 2 &&
                action >= 0 && action < action_count) keys[current_player][action] = value;
            else if (sscanf_s(line.c_str(), "action%d_gamepad = %d", &action, &value) == 2 &&
                     action >= 0 && action < action_count) pads[current_player][action] = WORD(value);
        }
        std::ifstream graphics(save / L"graphics.toml");
        while (std::getline(graphics, line)) {
            if (line == "fullscreen = true")
                SendMessageW(fullscreen, BM_SETCHECK, BST_CHECKED, 0);
            else if (line == "fullscreen = false")
                SendMessageW(fullscreen, BM_SETCHECK, BST_UNCHECKED, 0);
        }
    }

    HWND add(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
             int x, int y, int w, int h, int id = 0) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
            x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance, nullptr);
    }

    void create_ui() {
        root = executable_directory(); data = root / L"data"; save = root / L"save";
        fs::create_directories(data); fs::create_directories(save / L"game");
        tab = add(window, WC_TABCONTROLW, L"", WS_TABSTOP, 14, 14, 956, 535, id_tab);
        TCITEMW item{TCIF_TEXT};
        item.pszText = const_cast<wchar_t*>(L"Graphics Settings"); TabCtrl_InsertItem(tab, 0, &item);
        item.pszText = const_cast<wchar_t*>(L"Inputs"); TabCtrl_InsertItem(tab, 1, &item);
        graphics_page = add(window, L"STATIC", L"", 0, 28, 54, 926, 480);
        inputs_page = add(window, L"STATIC", L"", 0, 28, 54, 926, 480);
        SetWindowSubclass(inputs_page, input_page_proc, 1, 0);

        add(graphics_page, L"STATIC", L"Display", SS_LEFT, 24, 24, 200, 24);
        add(graphics_page, L"STATIC", L"Window scale:", SS_LEFT, 40, 70, 130, 24);
        scale = add(graphics_page, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                    175, 66, 180, 120, id_scale);
        SendMessageW(scale, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1x (480 x 272)"));
        SendMessageW(scale, CB_SETCURSEL, 0, 0);
        EnableWindow(scale, FALSE);
        fullscreen = add(graphics_page, L"BUTTON", L"Start in fullscreen",
                         BS_AUTOCHECKBOX | WS_TABSTOP, 40, 112, 220, 26, id_fullscreen);
        add(graphics_page, L"STATIC",
            L"The game keeps the original PSP logical resolution. These settings only control native presentation.",
            SS_LEFT, 40, 165, 760, 48);

        add(inputs_page, L"STATIC", L"Player 1 (PSP)", SS_LEFT,
            22, 10, 180, 25);
        add(inputs_page, L"STATIC", L"Game action", SS_LEFT, 24, 48, 190, 24);
        add(inputs_page, L"STATIC", L"Keyboard", SS_LEFT, 245, 48, 265, 24);
        add(inputs_page, L"STATIC", L"Gamepad", SS_LEFT, 555, 48, 265, 24);
        for (int i = 0; i < action_count; ++i) {
            const int y = 76 + i * 40;
            add(inputs_page, L"STATIC", action_names[i], SS_LEFT, 24, y + 5, 190, 25);
            keyboard_buttons[i] = add(inputs_page, L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP,
                                      235, y, 270, 30, id_keyboard_base + i);
            gamepad_buttons[i] = add(inputs_page, L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP,
                                     545, y, 270, 30, id_gamepad_base + i);
        }
        add(inputs_page, L"STATIC",
            L"Click a field, then press the new keyboard key or gamepad button. Esc cancels; Delete clears.",
            SS_LEFT, 24, 440, 820, 28);

        add(window, L"BUTTON", L"Open Data Folder", BS_PUSHBUTTON, 14, 565, 145, 32, id_open_data);
        add(window, L"BUTTON", L"Recheck Data", BS_PUSHBUTTON, 168, 565, 150, 32, id_prepare);
        add(window, L"BUTTON", L"Reset Defaults", BS_PUSHBUTTON, 327, 565, 135, 32, id_reset);
        add(window, L"BUTTON", L"Apply", BS_PUSHBUTTON, 665, 565, 90, 32, id_apply);
        add(window, L"BUTTON", L"Play", BS_DEFPUSHBUTTON, 764, 565, 90, 32, id_play);
        add(window, L"BUTTON", L"Exit", BS_PUSHBUTTON, 863, 565, 90, 32, id_exit);
        status = add(window, L"STATIC", L"", SS_LEFT, 16, 610, 935, 35);
        load_config();
        ShowWindow(inputs_page, SW_HIDE);
        refresh_bindings(); recheck(false);
        SetTimer(window, 1, 30, nullptr);
        PostMessageW(window, wm_check_update, 0, 0);
    }

    void refresh_bindings() {
        for (int i = 0; i < action_count; ++i) {
            SetWindowTextW(keyboard_buttons[i], key_name(keys[player][i]).c_str());
            SetWindowTextW(gamepad_buttons[i], pad_name(pads[player][i]).c_str());
        }
    }

    bool ready(std::wstring* missing = nullptr) const {
        std::vector<std::wstring> items;
        if (!fs::exists(data / L"game" / L"PSP_GAME")) items.push_back(L"extracted ISO content");
        if (!find_file(data / L"fonts", {L".pgf"}, L"ltn0")) items.push_back(L"ltn0.pgf from font.zip");
        if (!fs::exists(data / L"eboot" / L"NPEH00007_EBOOT_decrypted.BIN")) items.push_back(L"decrypted EBOOT dump");
        if (missing) {
            std::wostringstream text;
            for (size_t i = 0; i < items.size(); ++i) { if (i) text << L", "; text << items[i]; }
            *missing = text.str();
        }
        return items.empty();
    }

    void recheck(bool dialog) {
        // Adopt an EBOOT produced by PPSSPP in its isolated memstick.
        if (auto dump = find_file(data / L"ppsspp_memstick", {L".bin"}, L"eboot")) {
            fs::create_directories(data / L"eboot");
            std::error_code ec;
            fs::copy_file(*dump, data / L"eboot" / L"NPEH00007_EBOOT_decrypted.BIN",
                          fs::copy_options::overwrite_existing, ec);
        }
        std::wstring missing;
        if (ready(&missing)) SetWindowTextW(status, L"Data is prepared. The portable save is stored in save\\.");
        else SetWindowTextW(status, (L"Output is incomplete. Place game.iso, font.zip and ppsspp.zip in data using these exact names, then run run.bat. Missing: " + missing).c_str());
        if (dialog && !missing.empty()) MessageBoxW(window,
            (L"Data preparation is not complete.\n\nMissing: " + missing).c_str(), window_title.c_str(), MB_OK | MB_ICONINFORMATION);
    }

    void save_config() {
        fs::create_directories(save);
        std::ofstream input(save / L"input.toml", std::ios::trunc);
        input << "# Generated by FateUnlimitedCodesRecomp\n";
        for (int p = 0; p < 1; ++p) {
            input << "\n[player" << (p + 1) << "]\n";
            for (int i = 0; i < action_count; ++i)
                input << "action" << i << "_keyboard = " << keys[p][i]
                      << "\naction" << i << "_gamepad = " << pads[p][i] << "\n";
        }
        std::ofstream graphics(save / L"graphics.toml", std::ios::trunc);
        graphics << "# Generated by FateUnlimitedCodesRecomp\nscale = "
                 << (SendMessageW(scale, CB_GETCURSEL, 0, 0) + 1)
                 << "\nfullscreen = " << (SendMessageW(fullscreen, BM_GETCHECK, 0, 0) == BST_CHECKED ? "true" : "false") << "\n";
        SetWindowTextW(status, L"Settings saved in save\\.");
    }

    void play() {
        save_config();
        std::wstring missing;
        if (!ready(&missing)) { recheck(true); return; }
        const auto core = data / L"runtime" / L"FateUnlimitedCodesRecompCore.exe";
        if (!fs::exists(core)) {
            MessageBoxW(window,
                L"The locally built recompiled core is not staged in data\\runtime.\n\nFor a legal public package it is deliberately not included. Build it locally from your own decrypted EBOOT, then use tools\\stage_local_core.ps1.",
                window_title.c_str(), MB_OK | MB_ICONINFORMATION); return;
        }
        auto font = find_file(data / L"fonts", {L".pgf"}, L"ltn0");
        const auto iso = fs::exists(data / L"game.iso")
            ? std::optional<fs::path>(data / L"game.iso") : find_file(data, {L".iso"});
        if (!font || !iso) { recheck(true); return; }
        std::wstring command = quote(core) + L" " + quote(data / L"eboot" / L"NPEH00007_EBOOT_decrypted.BIN") +
            L" 1 --profile --window --savedata-root " + quote(save / L"game") +
            L" --game-data-root " + quote(data / L"game") +
            L" --font-root " + quote(font->parent_path()) +
            L" --umd-image " + quote(*iso) +
            L" --input-config " + quote(save / L"input.toml");
        const auto window_scale = std::to_wstring(SendMessageW(scale, CB_GETCURSEL, 0, 0) + 1);
        const bool start_fullscreen = SendMessageW(fullscreen, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SetEnvironmentVariableW(L"FUC_WINDOW_SCALE", window_scale.c_str());
        SetEnvironmentVariableW(L"FUC_FULLSCREEN", start_fullscreen ? L"1" : L"0");
        const bool started = run_process(command, root, false);
        SetEnvironmentVariableW(L"FUC_WINDOW_SCALE", nullptr);
        SetEnvironmentVariableW(L"FUC_FULLSCREEN", nullptr);
        if (!started)
            MessageBoxW(window, L"The local recompiled core could not be started.", window_title.c_str(), MB_OK | MB_ICONERROR);
    }

    void check_for_update() {
        const auto checker = root / L"tools" / L"check_update.ps1";
        if (!fs::exists(checker)) return;
        std::wstring command = L"powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File " +
            quote(checker) + L" -InstallRoot " + quote(root);
        DWORD code = 0;
        if (!run_process(command, root, true, &code, CREATE_NO_WINDOW) || code != 10) return;

        const auto update_state = save / L"update";
        const auto version = read_utf8_text(update_state / L"latest.version");
        const auto notes = read_utf8_text(update_state / L"latest.notes");
        if (version.empty()) return;

        const std::wstring heading = L"FateUnlimitedCodesRecomp " + version + L" is available";
        const std::wstring content = notes.empty() ? L"A new version is ready to download." : notes;
        TASKDIALOG_BUTTON buttons[] = {
            {1001, L"Update"},
            {1002, L"Later"}
        };
        TASKDIALOGCONFIG dialog{sizeof(dialog)};
        dialog.hwndParent = window;
        dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        dialog.dwCommonButtons = 0;
        dialog.pszWindowTitle = L"What's New";
        dialog.pszMainIcon = TD_INFORMATION_ICON;
        dialog.pszMainInstruction = heading.c_str();
        dialog.pszContent = content.c_str();
        dialog.cButtons = ARRAYSIZE(buttons);
        dialog.pButtons = buttons;
        dialog.nDefaultButton = 1001;
        int pressed = 1002;
        HRESULT dialog_result = E_NOTIMPL;
        if (const HMODULE controls = LoadLibraryW(L"comctl32.dll")) {
            using TaskDialogIndirectFn = HRESULT (WINAPI *)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
            const auto task_dialog = reinterpret_cast<TaskDialogIndirectFn>(
                GetProcAddress(controls, "TaskDialogIndirect"));
            if (task_dialog) dialog_result = task_dialog(&dialog, &pressed, nullptr, nullptr);
            FreeLibrary(controls);
        }
        if (FAILED(dialog_result)) {
            pressed = MessageBoxW(window, (heading + L"\n\n" + content + L"\n\nUpdate now?").c_str(),
                                  L"What's New", MB_YESNO | MB_ICONINFORMATION) == IDYES ? 1001 : 1002;
        }
        if (pressed != 1001) return;

        const auto downloader = root / L"tools" / L"download_update.ps1";
        const auto manifest = update_state / L"latest-manifest.json";
        command = L"powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File " +
            quote(downloader) + L" -InstallRoot " + quote(root) + L" -Manifest " + quote(manifest);
        SetWindowTextW(status, L"Downloading and verifying the update package...");
        code = 1;
        if (!run_process(command, root, true, &code, CREATE_NO_WINDOW) || code != 0) {
            MessageBoxW(window, L"The update could not be downloaded or verified. The installed game was not changed.",
                        L"Update failed", MB_OK | MB_ICONERROR);
            recheck(false); return;
        }
        const auto updater_path = read_utf8_text(root / L"update" / L"updater_path.txt");
        if (updater_path.empty() || !fs::exists(updater_path)) {
            MessageBoxW(window, L"The verified package does not contain update.bat.",
                        L"Update failed", MB_OK | MB_ICONERROR); return;
        }
        const fs::path updater(updater_path);
        const std::wstring parameters = L"-InstallRoot " + quote(root) +
            L" -Manifest " + quote(updater.parent_path() / L"update-manifest.json");
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", updater.c_str(),
            parameters.c_str(), updater.parent_path().c_str(), SW_SHOWNORMAL));
        if (result <= 32) {
            MessageBoxW(window, L"update.bat could not be started.", L"Update failed", MB_OK | MB_ICONERROR);
            return;
        }
        DestroyWindow(window);
    }

    void begin_capture(int action, bool gamepad) {
        capture_action = action; capture_gamepad = gamepad; previous_pad = 0;
        for (int key = 0; key < 256; ++key)
            previous_keys[static_cast<std::size_t>(key)] =
                (GetAsyncKeyState(key) & 0x8000) != 0;
        SetWindowTextW(gamepad ? gamepad_buttons[action] : keyboard_buttons[action],
                       gamepad ? L"Press a button..." : L"Press a key...");
        SetFocus(window);
    }

    void keyboard_capture(WPARAM key) {
        if (capture_action < 0 || capture_gamepad) return;
        if (key == VK_ESCAPE) { capture_action = -1; refresh_bindings(); return; }
        keys[player][capture_action] = (key == VK_DELETE || key == VK_BACK) ? 0 : int(key);
        capture_action = -1; refresh_bindings();
    }

    void poll_keyboard() {
        if (capture_action < 0 || capture_gamepad) return;
        for (int key = 1; key < 256; ++key) {
            const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
            const bool pressed = down && !previous_keys[static_cast<std::size_t>(key)];
            previous_keys[static_cast<std::size_t>(key)] = down;
            if (pressed && (key < VK_LBUTTON || key > VK_XBUTTON2)) {
                keyboard_capture(static_cast<WPARAM>(key));
                return;
            }
        }
    }

    void poll_gamepad() {
        if (capture_action < 0 || !capture_gamepad) return;
        XINPUT_STATE state{};
        if (XInputGetState(DWORD(player), &state) != ERROR_SUCCESS) return;
        const WORD now = state.Gamepad.wButtons;
        const WORD pressed = WORD(now & ~previous_pad);
        previous_pad = now;
        if (!pressed) return;
        WORD selected = 1;
        while (!(pressed & selected) && selected) selected = WORD(selected << 1);
        pads[player][capture_action] = selected;
        capture_action = -1; refresh_bindings();
    }

    void command(int id) {
        if (id >= id_keyboard_base && id < id_keyboard_base + action_count) { begin_capture(id - id_keyboard_base, false); return; }
        if (id >= id_gamepad_base && id < id_gamepad_base + action_count) { begin_capture(id - id_gamepad_base, true); return; }
        switch (id) {
        case id_open_data: fs::create_directories(data); ShellExecuteW(window, L"open", data.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case id_prepare: recheck(true); break;
        case id_reset: defaults(); refresh_bindings(); break;
        case id_apply: save_config(); break;
        case id_play: play(); break;
        case id_exit: DestroyWindow(window); break;
        }
    }
};

App* app = nullptr;

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: app->window = hwnd; app->create_ui(); return 0;
    case WM_COMMAND: app->command(LOWORD(wparam)); return 0;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lparam)->idFrom == id_tab &&
            reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE) {
            const int selected = TabCtrl_GetCurSel(app->tab);
            ShowWindow(app->graphics_page, selected == 0 ? SW_SHOW : SW_HIDE);
            ShowWindow(app->inputs_page, selected == 1 ? SW_SHOW : SW_HIDE);
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (app->capture_action >= 0 && !app->capture_gamepad) {
            app->keyboard_capture(wparam);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_TIMER: app->poll_keyboard(); app->poll_gamepad(); return 0;
    case wm_check_update: app->check_for_update(); return 0;
    case WM_DESTROY: KillTimer(hwnd, 1); PostQuitMessage(0); return 0;
    default: return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);
    App storage; app = &storage; app->instance = instance; app->defaults();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = window_proc; wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"FateUnlimitedCodesRecompLauncher";
    RegisterClassExW(&wc);
    window_title = configured_window_title();
    const HWND window = CreateWindowExW(0, wc.lpszClassName, window_title.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show); UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        const bool key_down = message.message == WM_KEYDOWN ||
                              message.message == WM_SYSKEYDOWN;
        if (app->capture_action >= 0 && !app->capture_gamepad && key_down) {
            app->keyboard_capture(message.wParam); continue;
        }
        // IsDialogMessage uses the arrows for dialog navigation. The binding
        // screen must behave like a controller configurator: arrows are input
        // candidates while capturing and otherwise do not move UI focus.
        if (key_down && is_arrow_key(message.wParam)) continue;
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    return int(message.wParam);
}
