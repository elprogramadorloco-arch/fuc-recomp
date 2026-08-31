#include "fuc/platform/windows_psp_window.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fuc::platform {
namespace {

constexpr std::size_t binding_count = 9;
struct InputBindings {
    std::array<std::array<int, binding_count>, 2> keys{{
        {{VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'A', 'S', 'X', 'Z', VK_RETURN}},
        {{VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'A', 'S', 'X', 'Z', VK_RETURN}}
    }};
    std::array<std::array<WORD, binding_count>, 2> pads{{
        {{XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
          XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
          XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_B,
          XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_START}},
        {{XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
          XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
          XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_B,
          XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_START}}
    }};
};

std::filesystem::path g_input_config_path;
std::optional<InputBindings> g_input_bindings;

const InputBindings& input_bindings() {
    if (!g_input_bindings) {
        InputBindings result;
        std::filesystem::path config_path = g_input_config_path;
        if (config_path.empty()) {
            if (const wchar_t* legacy = _wgetenv(L"FUC_INPUT_CONFIG"); legacy && *legacy)
                config_path = legacy;
        }
        std::ifstream input{config_path};
        std::string line;
        int player = -1;
        while (std::getline(input, line)) {
            if (line == "[player1]") { player = 0; continue; }
            if (line == "[player2]") { player = 1; continue; }
            if (player < 0) continue;
            int action = -1, value = 0;
            if (sscanf_s(line.c_str(), "action%d_keyboard = %d", &action, &value) == 2 &&
                action >= 0 && action < static_cast<int>(binding_count))
                result.keys[player][action] = value;
            else if (sscanf_s(line.c_str(), "action%d_gamepad = %d", &action, &value) == 2 &&
                     action >= 0 && action < static_cast<int>(binding_count))
                result.pads[player][action] = static_cast<WORD>(value);
        }
        g_input_bindings = std::move(result);
    }
    return *g_input_bindings;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

void set_windows_psp_input_config(const std::filesystem::path& path) {
    g_input_config_path = path.empty() ? std::filesystem::path{}
                                       : std::filesystem::absolute(path).lexically_normal();
    g_input_bindings.reset();
}

WindowsPspWindow::WindowsPspWindow() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW klass{};
    klass.lpfnWndProc = window_proc;
    klass.hInstance = instance;
    klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    klass.lpszClassName = L"FucPspNativeWindow";
    RegisterClassW(&klass);
    int scale = 2;
    if (const wchar_t* configured = _wgetenv(L"FUC_WINDOW_SCALE"))
        scale = std::clamp(_wtoi(configured), 1, 3);
    const bool fullscreen = [] {
        const wchar_t* configured = _wgetenv(L"FUC_FULLSCREEN");
        return configured && std::wstring(configured) == L"1";
    }();
    const DWORD style = fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT rect{0, 0, 480 * scale, 272 * scale};
    AdjustWindowRect(&rect, style, FALSE);
    handle_ = CreateWindowExW(0, klass.lpszClassName,
                              L"FateUnlimitedCodesRecomp",
                              style | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top,
                              nullptr, nullptr, instance, nullptr);
}

WindowsPspWindow::~WindowsPspWindow() {
    if (handle_ != nullptr && IsWindow(static_cast<HWND>(handle_)))
        DestroyWindow(static_cast<HWND>(handle_));
}

bool WindowsPspWindow::open() const noexcept {
    return handle_ != nullptr && IsWindow(static_cast<HWND>(handle_));
}

void WindowsPspWindow::pump_events() const noexcept {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void WindowsPspWindow::show_loading() const noexcept {
    pump_events();
    if (!open()) return;
    RECT client{};
    GetClientRect(static_cast<HWND>(handle_), &client);
    HDC dc = GetDC(static_cast<HWND>(handle_));
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, L"Cargando datos PSP...", -1, &client,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ReleaseDC(static_cast<HWND>(handle_), dc);
}

port::PhysicalInputState WindowsPspWindow::poll_input(
    const std::uint32_t player) const noexcept {
    port::PhysicalInputState result{};
    if (!open()) return result;
    const auto press = [&result](const bool held, const port::Button button) {
        if (held) result.held = static_cast<std::uint16_t>(
            result.held | port::button_mask(button));
    };

    XINPUT_STATE controller{};
    if (player < XUSER_MAX_COUNT &&
        XInputGetState(player, &controller) == ERROR_SUCCESS) {
        const WORD buttons = controller.Gamepad.wButtons;
        const auto& configured = input_bindings().pads[player];
        constexpr std::array<port::Button, binding_count> actions{
            port::Button::up, port::Button::down, port::Button::left,
            port::Button::right, port::Button::light, port::Button::medium,
            port::Button::heavy, port::Button::guard, port::Button::start
        };
        for (std::size_t i = 0; i < binding_count; ++i)
            press(configured[i] != 0 && (buttons & configured[i]) != 0u, actions[i]);
        press((buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0u, port::Button::shoulder_left);
        press((buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0u, port::Button::shoulder_right);
        press((buttons & XINPUT_GAMEPAD_START) != 0u, port::Button::start);
        press((buttons & XINPUT_GAMEPAD_BACK) != 0u, port::Button::select);
        const auto axis = [](const SHORT value, const SHORT deadzone) {
            if (std::abs(static_cast<int>(value)) <= deadzone) return 0.0f;
            const float normalized = value < 0
                ? static_cast<float>(value + deadzone) /
                      static_cast<float>(32768 - deadzone)
                : static_cast<float>(value - deadzone) /
                      static_cast<float>(32767 - deadzone);
            return std::clamp(normalized, -1.0f, 1.0f);
        };
        result.analog_x = axis(controller.Gamepad.sThumbLX,
                               XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        result.analog_y = -axis(controller.Gamepad.sThumbLY,
                                XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    }

    // Player one also has a direct keyboard layout. Input is accepted only
    // while this PSP window owns the foreground, so running the port does not
    // capture keys typed into another application.
    if (player < 2u && GetForegroundWindow() == static_cast<HWND>(handle_)) {
        const auto key = [](const int code) {
            return (GetAsyncKeyState(code) & 0x8000) != 0;
        };
        const auto& configured = input_bindings().keys[player];
        constexpr std::array<port::Button, binding_count> actions{
            port::Button::up, port::Button::down, port::Button::left,
            port::Button::right, port::Button::light, port::Button::medium,
            port::Button::heavy, port::Button::guard, port::Button::start
        };
        for (std::size_t i = 0; i < binding_count; ++i)
            press(configured[i] != 0 && key(configured[i]), actions[i]);
        press(key('Q'), port::Button::shoulder_left);
        press(key('E'), port::Button::shoulder_right);
        press(key(VK_RSHIFT), port::Button::select);
    }
    return result;
}

void WindowsPspWindow::present(const std::span<const std::uint32_t> psp_rgba,
                               const std::uint32_t width,
                               const std::uint32_t height) {
    pump_events();
    if (!open() || psp_rgba.size() < static_cast<std::size_t>(width) * height) return;

    std::vector<std::uint32_t> bgra(psp_rgba.size());
    for (std::size_t i = 0; i < psp_rgba.size(); ++i) {
        const std::uint32_t color = psp_rgba[i];
        bgra[i] = (color & 0xFF00FF00u) | ((color & 0xFFu) << 16u) |
                  ((color >> 16u) & 0xFFu);
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    RECT client{};
    GetClientRect(static_cast<HWND>(handle_), &client);
    HDC dc = GetDC(static_cast<HWND>(handle_));
    StretchDIBits(dc, 0, 0, client.right, client.bottom,
                  0, 0, static_cast<int>(width), static_cast<int>(height),
                  bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(static_cast<HWND>(handle_), dc);
}

} // namespace fuc::platform
