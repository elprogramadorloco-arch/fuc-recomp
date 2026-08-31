#pragma once

#include "fuc/port/player_input.hpp"

#include <cstdint>
#include <filesystem>
#include <span>

namespace fuc::platform {

// Sets the launcher-generated binding file before the native PSP window is
// created. An explicit CLI path is preferred over process environment state.
void set_windows_psp_input_config(const std::filesystem::path& path);

class WindowsPspWindow {
public:
    WindowsPspWindow();
    ~WindowsPspWindow();
    WindowsPspWindow(const WindowsPspWindow&) = delete;
    WindowsPspWindow& operator=(const WindowsPspWindow&) = delete;

    [[nodiscard]] bool open() const noexcept;
    void pump_events() const noexcept;
    void show_loading() const noexcept;
    [[nodiscard]] port::PhysicalInputState poll_input(
        std::uint32_t player = 0u) const noexcept;
    void present(std::span<const std::uint32_t> psp_rgba,
                 std::uint32_t width, std::uint32_t height);

private:
    void* handle_{};
};

} // namespace fuc::platform
