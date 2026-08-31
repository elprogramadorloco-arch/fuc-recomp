#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace fuc::platform {

struct WindowsPspAudioStats {
    std::uint64_t accepted{};
    std::uint64_t completed{};
    std::uint64_t dropped{};
    std::size_t queued{};
    std::size_t maximum_queued{};
};

class WindowsPspAudio {
public:
    WindowsPspAudio();
    ~WindowsPspAudio();
    WindowsPspAudio(const WindowsPspAudio&) = delete;
    WindowsPspAudio& operator=(const WindowsPspAudio&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] WindowsPspAudioStats stats();
    void submit(std::span<const std::int16_t> samples,
                std::uint32_t channels, std::uint32_t left_volume,
                std::uint32_t right_volume, std::uint32_t psp_channel,
                std::uint64_t guest_start_us);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fuc::platform
