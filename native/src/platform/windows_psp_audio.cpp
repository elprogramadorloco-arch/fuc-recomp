#include "fuc/platform/windows_psp_audio.hpp"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <map>
#include <vector>

namespace fuc::platform {

struct WindowsPspAudio::Impl {
    struct Block {
        std::vector<std::int16_t> samples;
        WAVEHDR header{};
    };

    HWAVEOUT output{};
    std::deque<std::unique_ptr<Block>> queued;
    WindowsPspAudioStats counters{};
    struct MixFrame {
        std::int64_t left{};
        std::int64_t right{};
    };
    std::map<std::uint64_t, MixFrame> mix_frames;
    std::uint64_t next_mix_frame{};
    std::uint64_t latest_mix_end{};
    std::uint64_t mix_origin{};
    bool mix_started{};
    bool mix_primed{};

    Impl() {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2u;
        format.nSamplesPerSec = 44100u;
        format.wBitsPerSample = 16u;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8u;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        if (waveOutOpen(&output, WAVE_MAPPER, &format, 0u, 0u,
                        CALLBACK_NULL) != MMSYSERR_NOERROR)
            output = nullptr;
    }

    ~Impl() {
        if (output == nullptr) return;
        waveOutReset(output);
        for (auto& block : queued)
            waveOutUnprepareHeader(output, &block->header, sizeof(WAVEHDR));
        waveOutClose(output);
    }

    void reap() {
        while (!queued.empty() &&
               (queued.front()->header.dwFlags & WHDR_DONE) != 0u) {
            waveOutUnprepareHeader(output, &queued.front()->header,
                                   sizeof(WAVEHDR));
            queued.pop_front();
            ++counters.completed;
        }
    }

    void queue_stereo(std::vector<std::int16_t> samples) {
        if (output == nullptr || samples.empty()) return;
        reap();
        // Bound host latency while allowing the short burst produced when the
        // live presenter takes ownership after deterministic fast-forward.
        if (queued.size() >= 24u) {
            ++counters.dropped;
            return;
        }
        auto block = std::make_unique<Block>();
        block->samples = std::move(samples);
        const std::size_t frames = block->samples.size() / 2u;
        (void)frames;
        block->header.lpData = reinterpret_cast<LPSTR>(block->samples.data());
        block->header.dwBufferLength = static_cast<DWORD>(
            block->samples.size() * sizeof(std::int16_t));
        if (waveOutPrepareHeader(output, &block->header, sizeof(WAVEHDR)) !=
            MMSYSERR_NOERROR)
            return;
        if (waveOutWrite(output, &block->header, sizeof(WAVEHDR)) !=
            MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(output, &block->header, sizeof(WAVEHDR));
            return;
        }
        queued.push_back(std::move(block));
        ++counters.accepted;
        if (queued.size() > counters.maximum_queued)
            counters.maximum_queued = queued.size();
    }

    void submit(const std::span<const std::int16_t> input,
                const std::uint32_t channels, const std::uint32_t left_volume,
                const std::uint32_t right_volume, const std::uint32_t,
                const std::uint64_t guest_start_us) {
        if (output == nullptr || (channels != 1u && channels != 2u) ||
            input.size() < channels)
            return;
        constexpr std::uint64_t sample_rate = 44100u;
        // The deterministic fast-forward hands over channels at different
        // points inside their current blocking buffer. Keep one bounded PSP
        // scheduling window so those concurrent streams meet before output.
        constexpr std::uint64_t mix_headroom = 4096u;
        // 512 frames is the native block size used by the game's long-lived
        // channels. It avoids presenting three tiny WinMM writes in one 60 Hz
        // host tick while preserving the same 44.1 kHz mixed timeline.
        constexpr std::size_t output_frames = 512u;
        const std::size_t frames = input.size() / channels;
        const std::uint64_t start =
            (guest_start_us * sample_rate + 500000u) / 1000000u;
        if (!mix_started) {
            next_mix_frame = start;
            latest_mix_end = start;
            mix_origin = start;
            mix_started = true;
        }
        const auto scale = [](const std::int16_t sample,
                              const std::uint32_t volume) {
            const std::int64_t value = static_cast<std::int64_t>(sample) *
                                       std::min<std::uint32_t>(volume, 0xFFFFu) /
                                       0x8000;
            return value;
        };
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            const std::uint64_t at = start + frame;
            if (at < next_mix_frame) continue;
            const std::int16_t left = input[frame * channels];
            const std::int16_t right = channels == 1u ? left
                                                       : input[frame * channels + 1u];
            auto& mixed = mix_frames[at];
            mixed.left += scale(left, left_volume);
            mixed.right += scale(right, right_volume);
        }
        if (start + frames > latest_mix_end) latest_mix_end = start + frames;
        constexpr std::uint64_t startup_observation = 8192u;
        if (!mix_primed) {
            if (latest_mix_end < mix_origin + startup_observation) return;
            next_mix_frame = latest_mix_end - mix_headroom;
            for (auto iterator = mix_frames.begin(); iterator != mix_frames.end() &&
                 iterator->first < next_mix_frame;)
                iterator = mix_frames.erase(iterator);
            mix_primed = true;
        }
        while (latest_mix_end >= next_mix_frame + mix_headroom + output_frames) {
            std::vector<std::int16_t> mixed(output_frames * 2u);
            for (std::size_t frame = 0u; frame < output_frames; ++frame) {
                const auto found = mix_frames.find(next_mix_frame + frame);
                if (found == mix_frames.end()) continue;
                mixed[frame * 2u] = static_cast<std::int16_t>(
                    std::clamp<std::int64_t>(found->second.left, -32768, 32767));
                mixed[frame * 2u + 1u] = static_cast<std::int16_t>(
                    std::clamp<std::int64_t>(found->second.right, -32768, 32767));
                mix_frames.erase(found);
            }
            next_mix_frame += output_frames;
            queue_stereo(std::move(mixed));
        }
    }

    WindowsPspAudioStats stats() {
        if (output != nullptr) reap();
        counters.queued = queued.size();
        return counters;
    }
};

WindowsPspAudio::WindowsPspAudio() : impl_(std::make_unique<Impl>()) {}
WindowsPspAudio::~WindowsPspAudio() = default;
bool WindowsPspAudio::available() const noexcept {
    return impl_ && impl_->output != nullptr;
}
WindowsPspAudioStats WindowsPspAudio::stats() { return impl_->stats(); }
void WindowsPspAudio::submit(const std::span<const std::int16_t> samples,
                             const std::uint32_t channels,
                             const std::uint32_t left_volume,
                             const std::uint32_t right_volume,
                             const std::uint32_t psp_channel,
                             const std::uint64_t guest_start_us) {
    impl_->submit(samples, channels, left_volume, right_volume, psp_channel,
                  guest_start_us);
}

} // namespace fuc::platform

#else

namespace fuc::platform {
struct WindowsPspAudio::Impl {};
WindowsPspAudio::WindowsPspAudio() : impl_(std::make_unique<Impl>()) {}
WindowsPspAudio::~WindowsPspAudio() = default;
bool WindowsPspAudio::available() const noexcept { return false; }
WindowsPspAudioStats WindowsPspAudio::stats() { return {}; }
void WindowsPspAudio::submit(std::span<const std::int16_t>, std::uint32_t,
                             std::uint32_t, std::uint32_t, std::uint32_t,
                             std::uint64_t) {}
} // namespace fuc::platform

#endif
