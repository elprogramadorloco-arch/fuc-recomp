// P6: reach and execute the recompiled PSP entry point.
//
// The smallest host that can answer "does the generated code actually run from
// module_start?". It loads the ELF into guest memory, registers the generated
// units and import wrappers, gives the guest a stack, and dispatches from the
// entry.
//
// Deliberately no display, no GPU, no HLE implementations. Without those the
// run is expected to stop at the first system call the game makes, and that is
// the useful result: it tells us how far initialisation gets and which import
// is the next thing to implement. Filling those in is P7, not this.

#include "fate_psp_profile.hpp"

#include "fuc/port/fate_psp_battle_map.hpp"
#include "fuc/port/player_input.hpp"
#include "fuc/port/psp_pad.hpp"
#include "fuc/platform/windows_psp_audio.hpp"
#include "fuc/platform/windows_psp_window.hpp"
#include "fuc/renderer/psp_ge_d3d11_surface.hpp"
#include "fuc/renderer/psp_ge_surface.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <thread>

namespace psprecomp {
void register_generated_functions(Runtime &runtime);
}

namespace {

std::string hex32(std::uint32_t value) {
    char buffer[11];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}


// One scripted button hold, in guest microseconds.
struct ScriptStep {
    fuc::port::Button button{};
    std::uint64_t from_us{};
    std::uint64_t to_us{};
};
std::vector<ScriptStep> g_input_script;

// The scripted buttons held at a given guest time. Shared by the exploration
// heartbeat and the live 60 Hz loop: the live loop used to publish an empty
// state, so nothing ever moved and idle could not be told apart from an action.
fuc::port::PhysicalInputState scripted_input_at(const std::uint64_t now_us) {
    fuc::port::PhysicalInputState state{};
    for (const auto &step : g_input_script) {
        if (now_us >= step.from_us && now_us < step.to_us) {
            state.held = static_cast<std::uint16_t>(
                state.held | fuc::port::button_mask(step.button));
        }
    }
    return state;
}

// Guest-RAM snapshot, taken at a chosen dispatch count and compared against the
// final state at the end of the run.
//
// This is the "unknown initial value" search the project kept doing by hand:
// stop the world twice and keep the words that moved. It lives in the host
// rather than in a separate tool because the guest's RAM only exists while the
// run does -- writing 32 MB to disk twice to diff it offline would be slower and
// would put game-derived bytes on the filesystem for no reason.
std::uint64_t g_snapshot_at = 0ull;
bool g_snapshot_taken = false;
std::vector<std::uint8_t> g_snapshot;
psprecomp::Runtime *g_snapshot_runtime = nullptr;
std::uint64_t g_trace_pc_at_us = 0ull;
bool g_trace_pc_at_dumped = false;
bool g_forward_trace_pc = false;

// Parses "start@1000-1500,cross@2000-2200": button, then the guest-time window in
// milliseconds. Anything unparseable is reported and skipped rather than
// silently ignored, because a typo in a script would otherwise look like the
// game failing to respond.
bool parse_input_script(const std::string &text) {
    static const std::map<std::string, fuc::port::Button> names{
        {"up", fuc::port::Button::up},         {"down", fuc::port::Button::down},
        {"left", fuc::port::Button::left},     {"right", fuc::port::Button::right},
        {"cross", fuc::port::Button::guard},   {"circle", fuc::port::Button::heavy},
        {"square", fuc::port::Button::light},  {"triangle", fuc::port::Button::medium},
        {"start", fuc::port::Button::start},   {"select", fuc::port::Button::select},
    };
    bool ok = true;
    std::size_t begin = 0u;
    while (begin <= text.size()) {
        const auto comma = text.find(',', begin);
        const std::string item = text.substr(begin, comma == std::string::npos
                                                        ? std::string::npos : comma - begin);
        if (!item.empty()) {
            const auto at = item.find('@');
            const auto dash = item.find('-', at == std::string::npos ? 0u : at);
            const auto found = at == std::string::npos ? names.end() : names.find(item.substr(0, at));
            if (found == names.end() || dash == std::string::npos) {
                std::fprintf(stderr, "script: no se entiende \"%s\"\n", item.c_str());
                ok = false;
            } else {
                ScriptStep step{};
                step.button = found->second;
                step.from_us = std::strtoull(item.substr(at + 1u, dash - at - 1u).c_str(), nullptr, 10) * 1000ull;
                step.to_us = std::strtoull(item.substr(dash + 1u).c_str(), nullptr, 10) * 1000ull;
                if (step.to_us <= step.from_us) {
                    std::fprintf(stderr, "script: ventana vacia en \"%s\"\n", item.c_str());
                    ok = false;
                } else {
                    g_input_script.push_back(step);
                }
            }
        }
        if (comma == std::string::npos) break;
        begin = comma + 1u;
    }
    return ok;
}

const char *confidence_name(const fuc::port::Confidence confidence) {
    switch (confidence) {
    case fuc::port::Confidence::verified: return "verified";
    case fuc::port::Confidence::provisional: return "provisional";
    default: return "unresolved";
    }
}

struct FighterDiagnostics final {
    std::uint32_t object{};
    std::uint32_t maximum_health{};
    std::uint32_t damage_taken{};
    float animation_seconds{};
    std::uint32_t animation_frames{};
};

fuc::port::BattleSnapshot capture_battle_snapshot(psprecomp::Runtime &runtime,
                                                   const std::uint64_t host_tick) {
    fuc::port::GuestMemoryReader reader{};
    reader.read_u32 = [&runtime](const std::uint32_t address, std::uint32_t &out) {
        if (!runtime.memory().contains(address, 4u)) return false;
        out = runtime.memory().load32(address);
        return true;
    };
    reader.read_f32 = [&runtime](const std::uint32_t address, float &out) {
        if (!runtime.memory().contains(address, 4u)) return false;
        const std::uint32_t bits = runtime.memory().load32(address);
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    };
    fuc::port::BattleSnapshot snapshot = fuc::port::PspStateBridge{
        std::move(reader), fuc::port::fate_psp_battle_map()}.capture(host_tick);
    constexpr std::uint64_t tick_us = 1'000'000ull / 60ull;
    snapshot.guest_tick.value = fate::virtual_time_us() / tick_us;
    snapshot.guest_tick.confidence = fuc::port::Confidence::verified;
    return snapshot;
}

void write_battle_snapshot(const std::filesystem::path &path,
                           const fuc::port::BattleSnapshot &snapshot,
                           const std::array<FighterDiagnostics, fuc::port::player_count> &diagnostics,
                           const std::uint64_t guest_time_us) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw psprecomp::Error("no se pudo escribir la instantanea de combate");

    output.precision(9);
    output << "{\n"
           << "  \"schema\": \"fuc.psp-battle-snapshot.v1\",\n"
           << "  \"contractVersion\": " << snapshot.contract_version << ",\n"
           << "  \"executableSha256\": \"" << fuc::port::fate_psp_executable_sha256 << "\",\n"
           << "  \"guestTimeUs\": " << guest_time_us << ",\n"
           << "  \"hostTick\": " << snapshot.host_tick << ",\n"
           << "  \"fighterPositionsAvailable\": "
           << (snapshot.has_fighter_positions() ? "true" : "false") << ",\n"
           << "  \"fighters\": [\n";
    for (std::size_t index = 0; index < snapshot.fighters.size(); ++index) {
        const auto &fighter = snapshot.fighters[index];
        output << "    {\n"
               << "      \"position\": {\"confidence\": \""
               << confidence_name(fighter.position.confidence) << "\", \"value\": ["
               << fighter.position.value.x << ", " << fighter.position.value.y << ", "
               << fighter.position.value.z << "]},\n"
               << "      \"actionId\": {\"confidence\": \""
               << confidence_name(fighter.action_id.confidence) << "\", \"value\": "
               << fighter.action_id.value << "},\n"
               << "      \"health\": {\"confidence\": \""
               << confidence_name(fighter.health.confidence) << "\", \"value\": "
               << fighter.health.value << "},\n"
               << "      \"diagnostics\": {\"object\": \"" << hex32(diagnostics[index].object)
               << "\", \"maximumHealth\": " << diagnostics[index].maximum_health
               << ", \"damageTaken\": " << diagnostics[index].damage_taken
               << ", \"animationSeconds\": " << diagnostics[index].animation_seconds
               << ", \"animationFrames\": " << diagnostics[index].animation_frames << "}\n"
               << "    }" << (index + 1u == snapshot.fighters.size() ? "\n" : ",\n");
    }
    output << "  ],\n"
           << "  \"camera\": {\n"
           << "    \"position\": {\"confidence\": \""
           << confidence_name(snapshot.camera.position.confidence) << "\", \"value\": ["
           << snapshot.camera.position.value.x << ", " << snapshot.camera.position.value.y
           << ", " << snapshot.camera.position.value.z
           << "], \"note\": \"raw PSP guest values; no presentation conversion\"}\n"
           << "  }\n"
           << "}\n";
    if (!output) throw psprecomp::Error("fallo al cerrar la instantanea de combate");
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc < 2)
            throw psprecomp::Error(
                "start this private core through FateUnlimitedCodesRecomp.exe");
        const std::filesystem::path executable = argv[1];
        std::uint64_t max_dispatches = 2'000'000ull;
        bool stub_imports = false;
        bool use_profile = false;
        bool trace_scheduler = false;
        bool trace_hle = false;
        bool trace_pc = false;
        bool show_window = false;
        std::filesystem::path surface_dump;
        std::filesystem::path savedata_root;
        std::filesystem::path game_data_root;
        std::filesystem::path font_root;
        std::filesystem::path umd_image;
        std::filesystem::path input_config;
        std::vector<float> wanted_floats;
        bool auto_start = false;
        unsigned long diff_max = 40ul;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> peeks;
        struct PointerPeek {
            std::uint32_t pointer_address;
            std::uint32_t offset;
            std::uint32_t count;
        };
        std::vector<PointerPeek> pointer_peeks;
        std::filesystem::path battle_snapshot_output;
        std::uint64_t live_frames = 0u;
        std::string live_trace_path;
        std::uint32_t live_watch_offset = 0u;
        std::uint32_t live_watch_count = 0u;
        for (int index = 2; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--stub-imports") stub_imports = true;
            else if (argument == "--profile") use_profile = true;
            else if (argument == "--trace-sched") trace_scheduler = true;
            else if (argument == "--trace-hle") trace_hle = true;
            else if (argument == "--trace-pc") trace_pc = true;
            else if (argument == "--trace-pc-at" && index + 1 < argc) {
                trace_pc = true;
                g_trace_pc_at_us = std::strtoull(argv[++index], nullptr, 0) * 1000ull;
            }
            else if (argument == "--window") show_window = true;
            else if (argument == "--surface-dump" && index + 1 < argc)
                surface_dump = argv[++index];
            else if (argument == "--savedata-root" && index + 1 < argc)
                savedata_root = argv[++index];
            else if (argument == "--game-data-root" && index + 1 < argc)
                game_data_root = argv[++index];
            else if (argument == "--font-root" && index + 1 < argc)
                font_root = argv[++index];
            else if (argument == "--umd-image" && index + 1 < argc)
                umd_image = argv[++index];
            else if (argument == "--input-config" && index + 1 < argc)
                input_config = argv[++index];
            else if (argument == "--auto-start") auto_start = true;
            else if (argument == "--script" && index + 1 < argc) {
                auto_start = true;
                if (!parse_input_script(argv[++index])) return 2;
            }
            else if (argument == "--script-file" && index + 1 < argc) {
                auto_start = true;
                const std::filesystem::path path = argv[++index];
                std::ifstream input(path, std::ios::binary);
                if (!input) {
                    std::fprintf(stderr, "script: no se puede abrir %s\n",
                                 path.string().c_str());
                    return 2;
                }
                const std::string text((std::istreambuf_iterator<char>(input)),
                                       std::istreambuf_iterator<char>());
                if (!parse_input_script(text)) return 2;
            }
            else if (argument == "--find-float" && index + 1 < argc)
                wanted_floats.push_back(std::strtof(argv[++index], nullptr));
            else if (argument == "--snapshot-at" && index + 1 < argc)
                g_snapshot_at = std::strtoull(argv[++index], nullptr, 0);
            else if (argument == "--diff-max" && index + 1 < argc)
                diff_max = std::strtoul(argv[++index], nullptr, 0);
            else if (argument == "--peek" && index + 1 < argc) {
                const std::string value = argv[++index];
                const std::size_t separator = value.find(':');
                const std::string address_text = value.substr(0u, separator);
                const std::string count_text = separator == std::string::npos
                                                   ? std::string{}
                                                   : value.substr(separator + 1u);
                char *address_end = nullptr;
                const unsigned long address = std::strtoul(address_text.c_str(), &address_end, 0);
                char *count_end = nullptr;
                const unsigned long count = count_text.empty()
                                                ? 1ul
                                                : std::strtoul(count_text.c_str(), &count_end, 0);
                if (address_end == address_text.c_str() || *address_end != '\0' ||
                    (!count_text.empty() &&
                     (count_end == count_text.c_str() || *count_end != '\0')) ||
                    address > 0xFFFFFFFFul || count == 0ul || count > 256ul) {
                    std::fprintf(stderr,
                                 "--peek espera ADDR[:COUNT], con COUNT entre 1 y 256\n");
                    return 2;
                }
                peeks.emplace_back(static_cast<std::uint32_t>(address),
                                   static_cast<std::uint32_t>(count));
            }
            else if (argument == "--peek-ptr" && index + 1 < argc) {
                const std::string value = argv[++index];
                const std::size_t first = value.find(':');
                const std::size_t second = first == std::string::npos
                                               ? std::string::npos
                                               : value.find(':', first + 1u);
                if (first == std::string::npos) {
                    std::fprintf(stderr,
                                 "--peek-ptr espera POINTER_ADDR:OFFSET[:COUNT]\n");
                    return 2;
                }
                const std::string pointer_text = value.substr(0u, first);
                const std::string offset_text = value.substr(
                    first + 1u, second == std::string::npos ? second : second - first - 1u);
                const std::string count_text = second == std::string::npos
                                                   ? std::string{}
                                                   : value.substr(second + 1u);
                char *pointer_end = nullptr;
                char *offset_end = nullptr;
                char *count_end = nullptr;
                const unsigned long pointer_address =
                    std::strtoul(pointer_text.c_str(), &pointer_end, 0);
                const unsigned long offset = std::strtoul(offset_text.c_str(), &offset_end, 0);
                const unsigned long count = count_text.empty()
                                                ? 1ul
                                                : std::strtoul(count_text.c_str(), &count_end, 0);
                if (pointer_end == pointer_text.c_str() || *pointer_end != '\0' ||
                    offset_end == offset_text.c_str() || *offset_end != '\0' ||
                    (!count_text.empty() &&
                     (count_end == count_text.c_str() || *count_end != '\0')) ||
                    pointer_address > 0xFFFFFFFFul || offset > 0xFFFFFFFFul ||
                    count == 0ul || count > 256ul) {
                    std::fprintf(stderr,
                                 "--peek-ptr espera POINTER_ADDR:OFFSET[:COUNT], COUNT 1..256\n");
                    return 2;
                }
                pointer_peeks.push_back({static_cast<std::uint32_t>(pointer_address),
                                         static_cast<std::uint32_t>(offset),
                                         static_cast<std::uint32_t>(count)});
            }
            else if (argument == "--battle-snapshot" && index + 1 < argc)
                battle_snapshot_output = argv[++index];
            else if (argument == "--live-frames" && index + 1 < argc)
                live_frames = std::strtoull(argv[++index], nullptr, 0);
            else if (argument == "--live-trace" && index + 1 < argc)
                live_trace_path = argv[++index];
            else if (argument == "--live-watch" && index + 1 < argc) {
                // OFFSET:COUNT, words of fighter one's object appended to every
                // trace row. This is the differential instrument: a field is
                // identified by WHICH words move in step with something already
                // known, and that needs the whole block sampled per frame, not
                // one word peeked at the end of a run.
                const std::string value = argv[++index];
                const auto colon = value.find(':');
                if (colon == std::string::npos) {
                    std::fprintf(stderr, "--live-watch espera OFFSET:COUNT\n");
                    return 2;
                }
                live_watch_offset = static_cast<std::uint32_t>(
                    std::strtoul(value.substr(0u, colon).c_str(), nullptr, 0));
                live_watch_count = static_cast<std::uint32_t>(
                    std::strtoul(value.substr(colon + 1u).c_str(), nullptr, 0));
                if (live_watch_count == 0u || live_watch_count > 512u) {
                    std::fprintf(stderr, "--live-watch: COUNT entre 1 y 512\n");
                    return 2;
                }
            }
            else max_dispatches = std::strtoull(argument.c_str(), nullptr, 0);
        }
        const auto elf = psprecomp::Elf32Image::from_file(executable);
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        // Where PSP device paths resolve to. A path like `disc0:/PSP_GAME/...`
        // becomes a path under this root, and `Runtime::translate_path` rejects
        // any `..`, so nothing outside it is reachable. The public launcher
        // always passes the locally prepared extracted-data directory.
        const std::filesystem::path game_data = game_data_root;
        runtime.set_game_root(std::filesystem::is_directory(game_data)
                                  ? game_data : executable.parent_path());

        const auto relocations = elf.load_and_relocate(runtime.memory(),
                                                       psprecomp::kDefaultPspUserLoadBase);
        std::printf("imagen    : %s\n", executable.filename().string().c_str());
        std::printf("relocations: %u (invalidas %u, no soportadas %u)\n",
                    relocations.total, relocations.invalid, relocations.unsupported);

        // Where the loaded image ends. The guest stack has to live above it, or
        // it would overwrite the program.
        std::uint64_t image_end = 0u;
        for (std::size_t index = 0; index < elf.segments().size(); ++index) {
            const auto &segment = elf.segments()[index];
            if (segment.type != 1u) continue; // PT_LOAD
            const std::uint64_t start =
                elf.segment_runtime_address(index, psprecomp::kDefaultPspUserLoadBase);
            image_end = std::max(image_end, start + segment.memory_size);
        }
        if (image_end == 0u || image_end > 0x0A000000ull) {
            throw psprecomp::Error("extension de carga del ELF PSP invalida");
        }

        const auto module = elf.find_module_info(runtime.memory(),
                                                 psprecomp::kDefaultPspUserLoadBase);
        if (!module) throw psprecomp::Error("no se encontro el module info tras la carga");
        const auto imports = elf.scan_imports(runtime.memory(), *module);

        psprecomp::register_generated_functions(runtime);
        std::printf("funciones registradas: %zu\n", runtime.function_count());

        // `--stub-imports` binds every import the module declares to a no-op that
        // returns 0, and counts the calls. Nothing is emulated, so anything the
        // guest does with the results is meaningless -- the point is purely to
        // measure how far initialisation walks and which system calls it leans
        // on, which is the input to P7. Off by default so the honest "stops at
        // the first missing import" result stays the default.
        static std::map<std::string, std::uint64_t> stub_calls;
        if (stub_imports) {
            std::size_t bound = 0u;
            for (const auto &import : imports) {
                const std::string key = import.library + "::" + hex32(import.nid);
                runtime.register_hle(import.library, import.nid,
                                     [key](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                                         ++stub_calls[key];
                                         ctx.set_gpr(2, 0u); // v0 = 0
                                     });
                ++bound;
            }
            std::printf("imports enlazados a stub: %zu\n", bound);
        }

        std::printf("modulo    : %s, gp %s, %zu imports\n", module->name.c_str(),
                    hex32(module->gp).c_str(), imports.size());

        // A modest guest stack just above the image, 256 KB, 16-byte aligned and
        // growing down. The real loader thread stack is a P7 concern; this only
        // has to be somewhere the entry can push to.
        const std::uint32_t arena = static_cast<std::uint32_t>((image_end + 0xFFFu) & ~0xFFFull);
        const std::uint32_t stack_size = 256u * 1024u;
        const std::uint32_t stack_top = (arena + stack_size) & ~0xFu;
        if (!runtime.memory().contains(arena, stack_size)) {
            throw psprecomp::Error("la pila del invitado no cabe en la memoria emulada");
        }
        runtime.memory().zero(arena, stack_size);

        // Installed after the stubs on purpose: a real implementation must win
        // over a no-op if both flags are given.
        if (use_profile) {
            if (!savedata_root.empty()) fate::set_savedata_root(savedata_root);
            if (!game_data_root.empty()) fate::set_game_data_root(game_data_root);
            if (!font_root.empty()) fate::set_font_root(font_root);
            const std::uint32_t profile_arena = stack_top + 0x1000u;
            // The PSP user partition runs to 0x0A000000, and GuestMemory's 32 MB
            // starting at 0x08000000 covers exactly that. Stopping at 0x09000000
            // left the allocator about 3.6 MB once the thread stacks were carved
            // out; the game asked for more, got "out of memory", and retried in a
            // tight loop -- measured at 555550 calls to
            // `sceKernelAllocPartitionMemory` inside a critical section.
            const std::uint32_t profile_arena_end = 0x0A000000u;
            // The raw UMD device resolves to the user's own disc image.
            const std::filesystem::path umd = umd_image.empty()
                ? std::filesystem::path("private_original/psp.iso") : umd_image;
            if (std::filesystem::is_regular_file(umd)) fate::set_umd_image(umd);
            fate::set_trace_pc(trace_pc);
            fate::install_profile(runtime, profile_arena, profile_arena_end, trace_scheduler, trace_hle);
            std::printf("perfil Fate instalado, arena de pilas %s .. %s\n",
                        hex32(profile_arena).c_str(), hex32(profile_arena_end).c_str());
        }

        std::map<std::uint32_t,
                 std::unique_ptr<fuc::renderer::psp::GeSurface>> ge_surfaces;
        fuc::renderer::psp::GeSurface* ge_present_surface = nullptr;
        std::unique_ptr<fuc::platform::WindowsPspWindow> ge_window;
        std::unique_ptr<fuc::platform::WindowsPspAudio> psp_audio;
        std::uint64_t audio_buffers_submitted = 0u;
        std::uint64_t audio_nonzero_samples = 0u;
        fate::GeDrawHook ge_window_draw_hook;
        std::uint64_t ge_draws_presented = 0u;
        std::uint64_t ge_draws_unsupported = 0u;
        std::chrono::nanoseconds ge_backend_elapsed{};
        std::map<std::uint64_t, std::uint64_t> ge_unsupported_kinds;
        const bool ge_draw_diag = std::getenv("PSPRECOMP_GE_DRAW_DIAG") != nullptr;
        const bool ge_stage_diag = std::getenv("PSPRECOMP_GE_STAGE_DIAG") != nullptr;
        const bool ge_inventory = std::getenv("PSPRECOMP_GE_INVENTORY") != nullptr;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ge_pixel_diag;
        if (const char* text = std::getenv("PSPRECOMP_GE_PIXEL_DIAG")) {
            std::string values(text);
            std::size_t begin = 0u;
            while (begin < values.size()) {
                const std::size_t comma = values.find(',', begin);
                const std::string item = values.substr(
                    begin, comma == std::string::npos ? comma : comma - begin);
                const std::size_t colon = item.find(':');
                if (colon != std::string::npos) {
                    const auto x = std::strtoul(item.substr(0u, colon).c_str(), nullptr, 0);
                    const auto y = std::strtoul(item.substr(colon + 1u).c_str(), nullptr, 0);
                    if (x < 480u && y < 272u)
                        ge_pixel_diag.emplace_back(static_cast<std::uint32_t>(x),
                                                   static_cast<std::uint32_t>(y));
                }
                if (comma == std::string::npos) break;
                begin = comma + 1u;
            }
        }
        const bool ge_prefer_gpu = std::getenv("PSPRECOMP_GE_SOFTWARE") == nullptr;
        std::uint64_t ge_draw_sequence = 0u;
        std::uint32_t ge_model_diag_count = 0u;
        std::set<std::uint32_t> ge_model_diag_lists;
        std::set<std::uint32_t> ge_stage_diag_commands;
        std::map<std::uint64_t, std::array<std::uint64_t, 6>> projected_draw_stats;
        std::map<std::uint32_t, std::array<std::uint64_t, 6>> projected_list_stats;
        const bool render_surface = show_window || !surface_dump.empty();
        if (render_surface) {
            if (!use_profile) throw psprecomp::Error("render GE requiere --profile");
            if (show_window) {
                if (!input_config.empty())
                    fuc::platform::set_windows_psp_input_config(input_config);
                ge_window = std::make_unique<fuc::platform::WindowsPspWindow>();
                if (!ge_window->open()) throw psprecomp::Error("no se pudo crear la ventana Win32");
                psp_audio = std::make_unique<fuc::platform::WindowsPspAudio>();
            }
            ge_window_draw_hook =
                [&ge_surfaces, &ge_draws_presented, &ge_draws_unsupported,
                 &ge_backend_elapsed,
                 &ge_unsupported_kinds, ge_draw_diag, ge_stage_diag, ge_inventory,
                 &ge_draw_sequence,
                 &ge_model_diag_count, &ge_model_diag_lists,
                 &ge_stage_diag_commands,
                 &projected_draw_stats,
                 &projected_list_stats, ge_pixel_diag, ge_prefer_gpu](
                    psprecomp::Runtime &rt,
                    const fuc::renderer::psp::PrimitiveDraw &draw) {
                    auto& surface_entry = ge_surfaces[draw.framebuffer_address];
                    if (!surface_entry)
                        surface_entry = fuc::renderer::psp::make_ge_surface(
                            ge_prefer_gpu);
                    auto* const ge_surface = surface_entry.get();
                    const auto type = fuc::renderer::psp::decode_vertex_type(
                        fuc::renderer::psp::decode_command(0x12000000u | draw.vertex_type));
                    const auto layout = fuc::renderer::psp::vertex_layout(type);
                    if (ge_inventory) {
                        auto& list_stats = projected_list_stats[draw.list_address];
                        ++list_stats[0];
                        list_stats[1] = std::max<std::uint64_t>(
                            list_stats[1], draw.vertex_count);
                        list_stats[2] = draw.vertex_type;
                        list_stats[3] = draw.vertex_address;
                        list_stats[4] = draw.texture_address;
                        list_stats[5] = static_cast<std::uint64_t>(draw.type);
                    }
                    if (ge_inventory && !type.through && draw.vertex_count >= 20u) {
                        const std::uint64_t key =
                            (static_cast<std::uint64_t>(draw.vertex_type) << 32u) |
                            draw.vertex_address;
                        auto& stats = projected_draw_stats[key];
                        ++stats[0];
                        stats[1] = std::max<std::uint64_t>(stats[1], draw.vertex_count);
                        stats[2] = draw.texture_address;
                        stats[3] = draw.texture_width;
                        stats[4] = draw.texture_height;
                        stats[5] = static_cast<std::uint64_t>(draw.type);
                    }
                    const std::size_t vertex_size =
                        static_cast<std::size_t>(layout.stride) * draw.vertex_count;
                    std::vector<std::uint8_t> expanded_vertices;
                    std::span<const std::uint8_t> vertices;
                    bool valid_vertices = layout.stride != 0u;
                    if (valid_vertices && type.index_format == 0u) {
                        const auto *vertex_pointer = rt.memory().raw_pointer(
                            draw.vertex_address, vertex_size);
                        valid_vertices = vertex_pointer != nullptr;
                        if (valid_vertices) vertices = {vertex_pointer, vertex_size};
                    } else if (valid_vertices) {
                        const std::uint32_t index_size = type.index_format == 1u ? 1u
                            : (type.index_format == 2u ? 2u : 4u);
                        valid_vertices = rt.memory().contains(
                            draw.index_address,
                            static_cast<std::size_t>(index_size) * draw.vertex_count);
                        expanded_vertices.reserve(vertex_size);
                        for (std::uint32_t i = 0u; valid_vertices && i < draw.vertex_count; ++i) {
                            const std::uint32_t index_address = draw.index_address + i * index_size;
                            const std::uint32_t vertex_index = type.index_format == 1u
                                ? rt.memory().load8(index_address)
                                : (type.index_format == 2u
                                    ? rt.memory().load16(index_address)
                                    : rt.memory().load32(index_address));
                            const auto *source = rt.memory().raw_pointer(
                                draw.vertex_address + vertex_index * layout.stride,
                                layout.stride);
                            if (source == nullptr) { valid_vertices = false; break; }
                            expanded_vertices.insert(expanded_vertices.end(), source,
                                                     source + layout.stride);
                        }
                        if (valid_vertices) vertices = expanded_vertices;
                    }
                    if (!valid_vertices) {
                        ++ge_draws_unsupported;
                        const std::uint64_t key = (0xFFull << 56u) |
                            (static_cast<std::uint64_t>(draw.type) << 48u) |
                            (static_cast<std::uint64_t>(draw.texture_format) << 40u) |
                            draw.vertex_type;
                        ++ge_unsupported_kinds[key];
                        return;
                    }
                    std::span<const std::uint8_t> texels;
                    std::span<const std::uint8_t> palette;
                    if (draw.texture_enabled &&
                        (draw.texture_format <= 3u ||
                         draw.texture_format == 4u ||
                         draw.texture_format == 5u)) {
                        const std::size_t row_bytes = draw.texture_format == 3u
                            ? draw.texture_buffer_width * 4u
                            : (draw.texture_format <= 2u
                                ? draw.texture_buffer_width * 2u
                                : (draw.texture_format == 4u
                                ? (draw.texture_buffer_width + 1u) / 2u
                                : draw.texture_buffer_width));
                        const std::size_t texture_size = draw.texture_swizzled
                            ? ((row_bytes + 15u) / 16u) *
                              ((draw.texture_height + 7u) / 8u) * 128u
                            : row_bytes * draw.texture_height;
                        const auto *texture_pointer = rt.memory().raw_pointer(
                            draw.texture_address, texture_size);
                        const std::size_t palette_size = draw.texture_format >= 4u
                            ? (draw.clut_format == 3u ? 1024u : 512u) : 0u;
                        const auto *palette_pointer = palette_size == 0u ? nullptr
                            : rt.memory().raw_pointer(draw.clut_address, palette_size);
                        if (texture_pointer != nullptr) texels = {texture_pointer, texture_size};
                        if (palette_pointer != nullptr) palette = {palette_pointer, palette_size};
                    }
                    ++ge_draw_sequence;
                    bool diagnose = false;
                    if (ge_draw_diag && !type.through) {
                        if (ge_stage_diag && draw.vertex_type == 0x000011FFu) {
                            diagnose = ge_stage_diag_commands.insert(
                                draw.command_address).second;
                        } else if (draw.vertex_count >= 20u &&
                                   ge_model_diag_count < 32u) {
                            diagnose = ge_model_diag_lists.insert(draw.list_address).second;
                            if (diagnose) ++ge_model_diag_count;
                        }
                    }
                    std::vector<std::uint32_t> pixel_before;
                    if (!ge_pixel_diag.empty()) {
                        const auto pixels = ge_surface->pixels();
                        pixel_before.reserve(ge_pixel_diag.size());
                        for (const auto [x, y] : ge_pixel_diag)
                            pixel_before.push_back(pixels[
                                static_cast<std::size_t>(y) * ge_surface->width() + x]);
                    }
                    std::vector<std::uint32_t> before;
                    if (diagnose) {
                        const auto pixels = ge_surface->pixels();
                        before.assign(pixels.begin(), pixels.end());
                        if (draw.vertex_type == 0x000010A1u ||
                            draw.vertex_type == 0x000011FFu ||
                            draw.vertex_type == 0x000017E3u ||
                            draw.vertex_type == 0x0001D7E3u) {
                            for (std::uint32_t vertex_index = 0u;
                                 vertex_index < std::min<std::uint32_t>(draw.vertex_count, 3u);
                                 ++vertex_index) {
                                fuc::renderer::psp::ThroughVertex decoded{};
                                const auto vertex_span = vertices.subspan(
                                    static_cast<std::size_t>(vertex_index) * layout.stride,
                                    layout.stride);
                                if (!fuc::renderer::psp::decode_projected_vertex(
                                        type, draw, vertex_span, decoded)) continue;
                                const float inv_w = std::fabs(decoded.clip_w) > 1.0e-12f
                                    ? 1.0f / decoded.clip_w : 0.0f;
                                const float screen_x = decoded.x * inv_w * draw.viewport_scale_x +
                                    draw.viewport_center_x - draw.viewport_offset_x;
                                const float screen_y = decoded.y * inv_w * draw.viewport_scale_y +
                                    draw.viewport_center_y - draw.viewport_offset_y;
                                std::printf(
                                    "GEVERT seq=%llu i=%u clip=%.6g,%.6g,%.6g,%.6g "
                                    "screen=%.6g,%.6g uv=%.6g,%.6g color=%s\n",
                                    static_cast<unsigned long long>(ge_draw_sequence),
                                    vertex_index, decoded.x, decoded.y, decoded.z,
                                    decoded.clip_w, screen_x, screen_y, decoded.u,
                                    decoded.v, hex32(decoded.color).c_str());
                            }
                        }
                    }
                    const auto backend_started = std::chrono::steady_clock::now();
                    const auto status = ge_surface->draw(
                        draw, vertices, texels, palette);
                    ge_backend_elapsed += std::chrono::steady_clock::now() -
                                          backend_started;
                    if (!ge_pixel_diag.empty()) {
                        const auto pixels = ge_surface->pixels();
                        for (std::size_t index = 0u; index < ge_pixel_diag.size(); ++index) {
                            const auto [x, y] = ge_pixel_diag[index];
                            const std::uint32_t after = pixels[
                                static_cast<std::size_t>(y) * ge_surface->width() + x];
                            if (after == pixel_before[index]) continue;
                            std::printf(
                                "GEPIXEL seq=%llu xy=%u,%u %s->%s list=%s cmd=%s "
                                "prim=%u vtype=%s vaddr=%s verts=%u texen=%u "
                                "tex=%s %ux%u tbw=%u fmt=%u swz=%u clut=%s/%u/%u/%u/%u "
                                "material=%s texfunc=%u/%u/%u cull=%u/%u depth=%u/%u/%u "
                                "blend=%u/%u,%u,%u\n",
                                static_cast<unsigned long long>(ge_draw_sequence), x, y,
                                hex32(pixel_before[index]).c_str(), hex32(after).c_str(),
                                hex32(draw.list_address).c_str(),
                                hex32(draw.command_address).c_str(),
                                static_cast<unsigned>(draw.type),
                                hex32(draw.vertex_type).c_str(),
                                hex32(draw.vertex_address).c_str(), draw.vertex_count,
                                draw.texture_enabled ? 1u : 0u,
                                hex32(draw.texture_address).c_str(), draw.texture_width,
                                draw.texture_height, draw.texture_buffer_width,
                                draw.texture_format, draw.texture_swizzled ? 1u : 0u,
                                hex32(draw.clut_address).c_str(), draw.clut_format,
                                draw.clut_shift, draw.clut_mask, draw.clut_start,
                                hex32(draw.material_color).c_str(), draw.texture_function,
                                draw.texture_use_alpha ? 1u : 0u,
                                draw.texture_double_color ? 1u : 0u,
                                draw.cull_enabled ? 1u : 0u,
                                draw.accept_counter_clockwise ? 1u : 0u,
                                draw.depth_test_enabled ? 1u : 0u,
                                draw.depth_write_enabled ? 1u : 0u, draw.depth_function,
                                draw.blend_enabled ? 1u : 0u, draw.blend_equation,
                                draw.blend_source_factor, draw.blend_destination_factor);
                        }
                    }
                    if (status == fuc::renderer::psp::SurfaceDrawStatus::success)
                        ++ge_draws_presented;
                    else
                    {
                        ++ge_draws_unsupported;
                        const std::uint64_t key =
                            (static_cast<std::uint64_t>(status) << 56u) |
                            (static_cast<std::uint64_t>(draw.type) << 48u) |
                            (static_cast<std::uint64_t>(draw.texture_format) << 40u) |
                            draw.vertex_type;
                        ++ge_unsupported_kinds[key];
                        if (ge_draw_diag && ge_draws_unsupported <= 32u) {
                            std::printf(
                                "GEUNSUPPORTED status=%u prim=%u vtype=%s count=%u "
                                "texen=%u texfmt=%u tex=%s tw=%u th=%u tbw=%u "
                                "swz=%u texbytes=%zu clutbytes=%zu\n",
                                static_cast<unsigned>(status),
                                static_cast<unsigned>(draw.type),
                                hex32(draw.vertex_type).c_str(), draw.vertex_count,
                                draw.texture_enabled ? 1u : 0u, draw.texture_format,
                                hex32(draw.texture_address).c_str(),
                                draw.texture_width, draw.texture_height,
                                draw.texture_buffer_width,
                                draw.texture_swizzled ? 1u : 0u,
                                texels.size(), palette.size());
                        }
                    }
                    if (diagnose) {
                        const auto after = ge_surface->pixels();
                        std::uint32_t min_x = ge_surface->width();
                        std::uint32_t min_y = ge_surface->height();
                        std::uint32_t max_x = 0u, max_y = 0u;
                        std::uint64_t changed = 0u;
                        for (std::uint32_t y = 0u; y < ge_surface->height(); ++y) {
                            for (std::uint32_t x = 0u; x < ge_surface->width(); ++x) {
                                const std::size_t at = static_cast<std::size_t>(y) *
                                    ge_surface->width() + x;
                                if (before[at] == after[at]) continue;
                                ++changed;
                                min_x = std::min(min_x, x); min_y = std::min(min_y, y);
                                max_x = std::max(max_x, x); max_y = std::max(max_y, y);
                            }
                        }
                        std::printf(
                            "GEDRAW seq=%llu list=%s cmd=%s prim=%u vtype=%s "
                            "vaddr=%s verts=%u "
                            "tex=%s %ux%u tbw=%u fmt=%u swz=%u clut=%u "
                            "material=%s texfunc=%u/%u "
                            "blend=%u/%u,%u,%u depth=%u/%u/%u "
                            "worldT=%.6g,%.6g,%.6g bone0T=%.6g,%.6g,%.6g "
                            "bone7T=%.6g,%.6g,%.6g viewT=%.6g,%.6g,%.6g "
                            "viewport=%.6g,%.6g,%.6g,%.6g,%.6g,%.6g "
                            "proj=%.6g,%.6g,%.6g,%.6g,%.6g,%.6g "
                            "changed=%llu bounds=%u,%u..%u,%u\n",
                            static_cast<unsigned long long>(ge_draw_sequence),
                            hex32(draw.list_address).c_str(),
                            hex32(draw.command_address).c_str(),
                            static_cast<unsigned>(draw.type), hex32(draw.vertex_type).c_str(),
                            hex32(draw.vertex_address).c_str(), draw.vertex_count,
                            hex32(draw.texture_address).c_str(), draw.texture_width,
                            draw.texture_height, draw.texture_buffer_width,
                            draw.texture_format, draw.texture_swizzled ? 1u : 0u,
                            draw.clut_format, hex32(draw.material_color).c_str(),
                            draw.texture_function, draw.texture_use_alpha ? 1u : 0u,
                            draw.blend_enabled ? 1u : 0u,
                            draw.blend_equation, draw.blend_source_factor,
                            draw.blend_destination_factor,
                            draw.depth_test_enabled ? 1u : 0u, draw.depth_function,
                            draw.depth_write_enabled ? 1u : 0u,
                            draw.transform.world[9], draw.transform.world[10],
                            draw.transform.world[11],
                            draw.transform.bones[9], draw.transform.bones[10],
                            draw.transform.bones[11],
                            draw.transform.bones[93], draw.transform.bones[94],
                            draw.transform.bones[95],
                            draw.transform.view[9], draw.transform.view[10],
                            draw.transform.view[11],
                            draw.viewport_scale_x, draw.viewport_scale_y,
                            draw.viewport_center_x, draw.viewport_center_y,
                            draw.viewport_offset_x, draw.viewport_offset_y,
                            draw.transform.projection[0], draw.transform.projection[5],
                            draw.transform.projection[10], draw.transform.projection[11],
                            draw.transform.projection[14], draw.transform.projection[15],
                            static_cast<unsigned long long>(changed),
                            changed == 0u ? 0u : min_x, changed == 0u ? 0u : min_y,
                            changed == 0u ? 0u : max_x, changed == 0u ? 0u : max_y);
                    }
                };
            if (live_frames == 0u) {
                live_frames = show_window
                    ? std::numeric_limits<std::uint64_t>::max()
                    : 600u;
            }
        }

        auto &cpu = runtime.cpu();
        cpu.set_gpr(29, stack_top); // sp
        cpu.set_gpr(28, module->gp); // gp
        cpu.set_gpr(31, 0u);         // ra: returning to 0 ends the run
        cpu.set_gpr(4, 0u);          // a0
        cpu.set_gpr(5, 0u);          // a1

        const std::uint32_t entry = elf.runtime_entry(psprecomp::kDefaultPspUserLoadBase);
        std::printf("pila      : %s .. %s\n", hex32(arena).c_str(), hex32(stack_top).c_str());
        std::printf("entry     : %s\n", hex32(entry).c_str());
        std::printf("tope de dispatches: %llu\n\n",
                    static_cast<unsigned long long>(max_dispatches));

        // Input goes through the port's own contract: an `InputSampler` freezes
        // both players before the step and `to_psp_buttons` translates. Nothing
        // here hand-rolls PSP button bits.
        static fuc::port::InputSampler sampler;
        static std::uint64_t published_tick = 0u;
        g_snapshot_runtime = &runtime;

        if (auto_start && g_input_script.empty()) {
            // Default when only `--auto-start` is given: START held for half of
            // each of the first ten seconds, which is what walks past title and
            // attract screens.
            for (std::uint64_t second = 0; second < 10u; ++second) {
                g_input_script.push_back({fuc::port::Button::start,
                                          second * 1'000'000ull,
                                          second * 1'000'000ull + 500'000ull});
            }
        }
        if (auto_start) {
            g_forward_trace_pc = trace_pc;
            // Taps START on the guest's own clock, not the host's: half a second
            // held, half released, forever. Menus advance on START, so this is
            // enough to walk past title and attract screens and see how far the
            // game gets -- an exploration tool, not the real input path.
            //
            // The real path publishes one frozen InputSnapshot per 60 Hz tick;
            // that needs a host loop that interleaves with the guest, which this
            // one-shot `run` does not do yet.
            psprecomp::set_runtime_heartbeat_hook(
                [](std::uint64_t dispatch, std::uint32_t pc) {
                    if (g_forward_trace_pc) fate::trace_pc_heartbeat(pc);
                    if (g_trace_pc_at_us != 0ull && !g_trace_pc_at_dumped &&
                        fate::virtual_time_us() >= g_trace_pc_at_us) {
                        g_trace_pc_at_dumped = true;
                        fate::dump_trace_pc_ring("la sonda temporal solicitada");
                    }
                    // Snapshot first: it must be a picture of the guest BEFORE
                    // this tick publishes new input, or the very act of
                    // observing would be part of what changed.
                    if (g_snapshot_at != 0ull && !g_snapshot_taken &&
                        dispatch >= g_snapshot_at && g_snapshot_runtime != nullptr) {
                        g_snapshot_taken = true;
                        auto &memory = g_snapshot_runtime->memory();
                        g_snapshot.resize(memory.size());
                        memory.copy_out(psprecomp::GuestMemory::kPhysicalBase, g_snapshot);
                    }
                    // A fixed 60 Hz tick on the GUEST's clock. Pacing on the
                    // host's would make the game's timing depend on how fast this
                    // machine runs the translation.
                    constexpr std::uint64_t kTickUs = 1'000'000ull / 60ull;
                    const std::uint64_t tick = fate::virtual_time_us() / kTickUs;
                    if (tick == published_tick) return; // still inside this tick
                    published_tick = tick;

                    // Scripted source: each entry holds one button for a window of
                    // guest time. Menus need directions and a confirm, not just
                    // START, and recompiling to change a sequence would make
                    // exploring them painfully slow -- hence `--script`.
                    const fuc::port::PhysicalInputState state =
                        scripted_input_at(fate::virtual_time_us());
                    // Both slots are sampled and frozen together even though only
                    // one carries input, so the two-player path is the one being
                    // exercised rather than a single-player shortcut that would
                    // have to be undone later.
                    sampler.set_player_state(fuc::port::PlayerId::one, 0u, state);
                    sampler.set_player_state(fuc::port::PlayerId::two, 1u, {});
                    const auto snapshot = sampler.freeze(tick);

                    const auto &first = snapshot.player(fuc::port::PlayerId::one).input;
                    fate::set_pad_state(fuc::port::to_psp_buttons(first),
                                        fuc::port::to_psp_analog(first.analog_x),
                                        fuc::port::to_psp_analog(first.analog_y));
                },
                trace_pc ? 1u : 1024u);
            std::printf("entrada: contrato del port a 60 Hz sobre el reloj del invitado\n");
        }

        // The GE walker follows the PSP CALL/RET sublists when the active
        // command buffer invokes them. No boot-era pixels need to be retained;
        // rasterizing hidden warm-up frames only slows the Windows host.
        constexpr std::uint64_t kGraphicsWarmupDispatches = 0u;
        const std::uint64_t warmup_dispatches = render_surface
            ? std::min(max_dispatches, kGraphicsWarmupDispatches) : 0u;
        if (ge_window) {
            ge_window->show_loading();
            fate::set_vblank_hook(
                [&ge_window](psprecomp::Runtime& rt, std::uint64_t,
                             std::uint32_t) {
                    ge_window->pump_events();
                    if (!ge_window->open()) rt.stop("ventana cerrada durante carga");
                });
        }
        // Attach the host device before the first guest dispatch.  Joining the
        // PSP audio timeline after a fast-forward drops the beginning of each
        // active channel and is not equivalent to a normal hardware boot.
        if (psp_audio && psp_audio->available()) {
            fate::set_audio_pcm_hook(
                [&psp_audio, &audio_buffers_submitted,
                 &audio_nonzero_samples](const std::span<const std::int16_t> samples,
                              const std::uint32_t channels,
                              const std::uint32_t left,
                              const std::uint32_t right,
                              const std::uint32_t psp_channel,
                              const std::uint64_t guest_start_us) {
                    ++audio_buffers_submitted;
                    audio_nonzero_samples += static_cast<std::uint64_t>(
                        std::count_if(samples.begin(), samples.end(),
                                      [](const std::int16_t sample) {
                                          return sample != 0;
                                      }));
                    psp_audio->submit(samples, channels, left, right,
                                      psp_channel, guest_start_us);
                });
        }
        const auto fast_forward_started = std::chrono::steady_clock::now();
        runtime.run(entry, max_dispatches - warmup_dispatches);
        const auto fast_forward_elapsed = std::chrono::steady_clock::now() -
                                          fast_forward_started;
        std::printf("tiempo host fast-forward AOT: %.3f s\n",
                    std::chrono::duration<double>(fast_forward_elapsed).count());
        if (ge_window) fate::set_vblank_hook({});
        if (render_surface) {
            fate::set_ge_draw_hook(ge_window_draw_hook);
            if (warmup_dispatches != 0u) {
                std::printf("warm-up GE previo: %llu dispatches\n",
                            static_cast<unsigned long long>(warmup_dispatches));
                runtime.run(runtime.cpu().pc, warmup_dispatches);
            }
        }

        if (live_frames != 0u) {
            if (!use_profile) throw psprecomp::Error("el ciclo vivo requiere --profile");
            fate::reset_ge_flow_stats();
            std::ofstream trace;
            if (!live_trace_path.empty()) {
                trace.open(live_trace_path, std::ios::trunc);
                if (!trace) throw psprecomp::Error("no se pudo abrir --live-trace");
                trace << "frame,guest_us,cam_x,cam_y,cam_z,p1_action,p1_anim,p1_anim_frame,p1_x,p1_z,"
                         "p2_action,p2_anim,p2_anim_frame,p2_x,p2_z";
                for (std::uint32_t w = 0; w < live_watch_count; ++w)
                    trace << ",w" << (live_watch_offset + 4u * w);
                trace << '\n';
            }
            bool reached_vblank = false;
            fate::set_vblank_hook([&reached_vblank, &ge_surfaces,
                                   &ge_present_surface, &ge_window](
                                                    psprecomp::Runtime &rt,
                                                    std::uint64_t, std::uint32_t) {
                const std::uint32_t displayed = 0x04000000u |
                    (fate::display_framebuffer() & 0x001FFFF0u);
                const auto found = ge_surfaces.find(displayed);
                if (found != ge_surfaces.end())
                    ge_present_surface = found->second.get();
                else if (!ge_surfaces.empty())
                    ge_present_surface = ge_surfaces.begin()->second.get();
                if (ge_present_surface != nullptr)
                    ge_present_surface->finish_frame();
                if (ge_present_surface != nullptr && ge_window)
                    ge_window->present(ge_present_surface->pixels(),
                                       ge_present_surface->width(),
                                       ge_present_surface->height());
                reached_vblank = true;
                rt.stop("frontera VBlank viva");
            });
            const auto frame_period = std::chrono::microseconds{16'667};
            const auto live_started = std::chrono::steady_clock::now();
            std::chrono::nanoseconds live_runtime_elapsed{};
            auto deadline = std::chrono::steady_clock::now();
            std::uint64_t captured = 0u;
            for (; captured < live_frames; ++captured) {
                if (ge_window && !ge_window->open()) break;
                // Input for both players is frozen before the guest resumes.
                auto player_one = scripted_input_at(fate::virtual_time_us());
                fuc::port::PhysicalInputState player_two{};
                if (ge_window) {
                    const auto physical_one = ge_window->poll_input(0u);
                    player_one.held = static_cast<std::uint16_t>(
                        player_one.held | physical_one.held);
                    if (physical_one.analog_x != 0.0f || physical_one.analog_y != 0.0f) {
                        player_one.analog_x = physical_one.analog_x;
                        player_one.analog_y = physical_one.analog_y;
                    }
                    player_two = ge_window->poll_input(1u);
                }
                sampler.set_player_state(fuc::port::PlayerId::one, 0u, player_one);
                sampler.set_player_state(fuc::port::PlayerId::two, 1u, player_two);
                const auto input = sampler.freeze(published_tick + captured + 1u);
                const auto &first = input.player(fuc::port::PlayerId::one).input;
                if (first.held != 0u && std::getenv("FUC_INPUT_DIAG") != nullptr)
                    std::printf("[input] player=1 held=0x%04X\n", first.held);
                fate::set_pad_state(fuc::port::to_psp_buttons(first),
                                    fuc::port::to_psp_analog(first.analog_x),
                                    fuc::port::to_psp_analog(first.analog_y));

                reached_vblank = false;
                const std::uint32_t resume_pc = runtime.cpu().pc;
                const auto runtime_started = std::chrono::steady_clock::now();
                runtime.run(resume_pc, 500'000u);
                live_runtime_elapsed += std::chrono::steady_clock::now() -
                                        runtime_started;
                if (!reached_vblank)
                    throw psprecomp::Error("AOT no alcanzo el siguiente VBlank vivo: " +
                                           runtime.stop_reason());
                const auto snapshot = capture_battle_snapshot(runtime, captured);
                if (trace.is_open()) {
                    // One row per live frame. Action and animation ids are the
                    // guest's own opaque numbers -- written down, never
                    // interpreted here -- so that mapping them to clips is done
                    // from a measured series rather than from a single sample.
                    trace << (captured + 1u) << ',' << fate::virtual_time_us()
                          << ',' << snapshot.camera.position.value.x
                          << ',' << snapshot.camera.position.value.y
                          << ',' << snapshot.camera.position.value.z;
                    for (const auto &fighter : snapshot.fighters) {
                        trace << ',' << (fighter.action_id.usable()
                                             ? std::to_string(fighter.action_id.value) : "")
                              << ',' << (fighter.animation_id.usable()
                                             ? std::to_string(fighter.animation_id.value) : "")
                              << ',' << (fighter.animation_frame.usable()
                                             ? std::to_string(fighter.animation_frame.value) : "")
                              << ',' << (fighter.position.usable()
                                             ? std::to_string(fighter.position.value.x) : "")
                              << ',' << (fighter.position.usable()
                                             ? std::to_string(fighter.position.value.z) : "");
                    }
                    if (live_watch_count != 0u) {
                        std::uint32_t base = 0u;
                        if (runtime.memory().contains(fuc::port::fate_psp_fighter_pointer_p1, 4u))
                            base = runtime.memory().load32(fuc::port::fate_psp_fighter_pointer_p1);
                        for (std::uint32_t w = 0; w < live_watch_count; ++w) {
                            const std::uint32_t at = base + live_watch_offset + 4u * w;
                            trace << ',';
                            if (base != 0u && runtime.memory().contains(at, 4u))
                                trace << runtime.memory().load32(at);
                        }
                    }
                    trace << '\n';
                }
                deadline += frame_period;
                std::this_thread::sleep_until(deadline);
            }
            fate::set_vblank_hook({});
            fate::set_audio_pcm_hook({});
            const auto live_elapsed = std::chrono::steady_clock::now() - live_started;
            std::printf("frames AOT vivos capturados sin filtrado: %llu\n",
                        static_cast<unsigned long long>(captured));
            std::printf("tiempo host ciclo vivo: %.3f s (%.3f ms/frame)\n",
                        std::chrono::duration<double>(live_elapsed).count(),
                        captured == 0u ? 0.0 :
                            std::chrono::duration<double, std::milli>(live_elapsed).count() /
                                static_cast<double>(captured));
            std::printf("tiempo runtime vivo: %.3f ms/frame; backend GE: %.3f ms/frame\n",
                        captured == 0u ? 0.0 :
                            std::chrono::duration<double, std::milli>(
                                live_runtime_elapsed).count() / static_cast<double>(captured),
                        captured == 0u ? 0.0 :
                            std::chrono::duration<double, std::milli>(
                                ge_backend_elapsed).count() / static_cast<double>(captured));
            if (psp_audio) {
                const auto audio_stats = psp_audio->stats();
                std::printf("audio PSP: buffers=%llu muestras-no-cero=%llu salida=%s\n",
                            static_cast<unsigned long long>(audio_buffers_submitted),
                            static_cast<unsigned long long>(audio_nonzero_samples),
                            psp_audio->available() ? "WinMM" : "no disponible");
                std::printf(
                    "cola WinMM: accepted=%llu completed=%llu dropped=%llu "
                    "queued=%zu max-queued=%zu\n",
                    static_cast<unsigned long long>(audio_stats.accepted),
                    static_cast<unsigned long long>(audio_stats.completed),
                    static_cast<unsigned long long>(audio_stats.dropped),
                    audio_stats.queued, audio_stats.maximum_queued);
            }
            fate::report_audio_flow();
            fate::report_mpeg_flow();
        }
        if (render_surface) {
            std::printf("draws GE rasterizados: %llu, no soportados: %llu\n",
                        static_cast<unsigned long long>(ge_draws_presented),
                        static_cast<unsigned long long>(ge_draws_unsupported));
            for (const auto& [key, stats] : projected_draw_stats) {
                std::printf(
                    "  malla PSP vtype=%s vaddr=%s draws=%llu maxverts=%llu "
                    "prim=%llu tex=%s %llux%llu\n",
                    hex32(static_cast<std::uint32_t>(key >> 32u)).c_str(),
                    hex32(static_cast<std::uint32_t>(key)).c_str(),
                    static_cast<unsigned long long>(stats[0]),
                    static_cast<unsigned long long>(stats[1]),
                    static_cast<unsigned long long>(stats[5]),
                    hex32(static_cast<std::uint32_t>(stats[2])).c_str(),
                    static_cast<unsigned long long>(stats[3]),
                    static_cast<unsigned long long>(stats[4]));
            }
            for (const auto& [list, stats] : projected_list_stats) {
                std::printf(
                    "  sublista PSP %s draws=%llu maxverts=%llu vtype=%s "
                    "vaddr=%s tex=%s prim=%llu\n",
                    hex32(list).c_str(),
                    static_cast<unsigned long long>(stats[0]),
                    static_cast<unsigned long long>(stats[1]),
                    hex32(static_cast<std::uint32_t>(stats[2])).c_str(),
                    hex32(static_cast<std::uint32_t>(stats[3])).c_str(),
                    hex32(static_cast<std::uint32_t>(stats[4])).c_str(),
                    static_cast<unsigned long long>(stats[5]));
            }
            for (const auto &[key, count] : ge_unsupported_kinds) {
                std::printf("  GE pendiente status=%u prim=%u texfmt=%u vtype=%s count=%llu\n",
                            static_cast<unsigned>((key >> 56u) & 0xFFu),
                            static_cast<unsigned>((key >> 48u) & 0xFFu),
                            static_cast<unsigned>((key >> 40u) & 0xFFu),
                            hex32(static_cast<std::uint32_t>(key)).c_str(),
                            static_cast<unsigned long long>(count));
            }
            fate::set_ge_draw_hook({});
            std::printf("framebuffer display activo: %s\n",
                        hex32(0x04000000u |
                            (fate::display_framebuffer() & 0x001FFFF0u)).c_str());
            for (const auto& [address, surface] : ge_surfaces) {
                std::uint64_t nonblack = 0u;
                for (const std::uint32_t color : surface->pixels())
                    if ((color & 0x00FFFFFFu) != 0u) ++nonblack;
                std::printf("  framebuffer GE %s nonblack=%llu\n",
                            hex32(address).c_str(),
                            static_cast<unsigned long long>(nonblack));
            }
            if (!surface_dump.empty()) {
                if (ge_present_surface == nullptr && !ge_surfaces.empty())
                    ge_present_surface = ge_surfaces.begin()->second.get();
                if (ge_present_surface == nullptr)
                    throw psprecomp::Error("no hay framebuffer GE para --surface-dump");
                std::ofstream image(surface_dump, std::ios::binary | std::ios::trunc);
                if (!image) throw psprecomp::Error("no se pudo abrir --surface-dump");
                image << "P6\n" << ge_present_surface->width() << ' '
                      << ge_present_surface->height()
                      << "\n255\n";
                for (const std::uint32_t color : ge_present_surface->pixels()) {
                    const char rgb[3]{static_cast<char>(color & 0xFFu),
                                      static_cast<char>((color >> 8u) & 0xFFu),
                                      static_cast<char>((color >> 16u) & 0xFFu)};
                    image.write(rgb, sizeof(rgb));
                }
                std::printf("superficie PSP guardada: %s\n",
                            surface_dump.string().c_str());
            }
        }

        if (use_profile && trace_pc) fate::dump_trace_pc_ring("que terminase la ejecucion");

        // `--find-float V` scans guest RAM for a 32-bit float and reports where
        // it sits. A targeted search for a known value beats a blind
        // differential sweep, and a hit hands over the address of a structure
        // rather than a lone number.
        //
        // Exact bit comparison, and that is a real limitation: it only works for
        // a value measured ON THIS PLATFORM. Feeding it a constant measured on
        // Values measured on a different platform may differ in their low bits.
        // Search only for observations made on this PSP guest image.
        for (const float wanted : wanted_floats) {
            std::uint32_t pattern{};
            std::memcpy(&pattern, &wanted, sizeof(pattern));
            std::vector<std::uint32_t> hits;
            for (std::uint32_t address = 0x08800000u; address + 4u <= 0x0A000000u; address += 4u) {
                if (!runtime.memory().contains(address, 4u)) continue;
                if (runtime.memory().load32(address) != pattern) continue;
                hits.push_back(address);
                if (hits.size() >= 64u) break;
            }
            std::printf("\nbusqueda de %.9g (bits %s): %zu coincidencias%s\n",
                        static_cast<double>(wanted), hex32(pattern).c_str(), hits.size(),
                        hits.size() >= 64u ? " (cortado en 64)" : "");
            for (std::size_t index = 0; index < hits.size() && index < 24u; ++index) {
                std::printf("   %s\n", hex32(hits[index]).c_str());
            }
        }
        for (const auto &[address, count] : peeks) {
            const std::uint64_t byte_count = static_cast<std::uint64_t>(count) * 4ull;
            std::printf("\nmemoria en %s (%u palabras):\n", hex32(address).c_str(), count);
            if (byte_count > 0xFFFFFFFFull ||
                !runtime.memory().contains(address, static_cast<std::uint32_t>(byte_count))) {
                std::printf("   rango fuera de la RAM del invitado\n");
                continue;
            }
            for (std::uint32_t word = 0u; word < count; ++word) {
                const std::uint32_t current = address + word * 4u;
                const std::uint32_t bits = runtime.memory().load32(current);
                float as_float = 0.0f;
                std::memcpy(&as_float, &bits, sizeof(as_float));
                std::printf("   %s  %s  u32 %10u  f32 %.9g\n",
                            hex32(current).c_str(), hex32(bits).c_str(), bits,
                            static_cast<double>(as_float));
            }
        }
        for (const auto &peek : pointer_peeks) {
            std::printf("\npuntero en %s + %s (%u palabras):\n",
                        hex32(peek.pointer_address).c_str(), hex32(peek.offset).c_str(),
                        peek.count);
            if (!runtime.memory().contains(peek.pointer_address, 4u)) {
                std::printf("   direccion del puntero fuera de la RAM del invitado\n");
                continue;
            }
            const std::uint32_t base = runtime.memory().load32(peek.pointer_address);
            const std::uint64_t address64 = static_cast<std::uint64_t>(base) + peek.offset;
            const std::uint64_t byte_count = static_cast<std::uint64_t>(peek.count) * 4ull;
            std::printf("   base %s, destino %s\n", hex32(base).c_str(),
                        address64 <= 0xFFFFFFFFull
                            ? hex32(static_cast<std::uint32_t>(address64)).c_str()
                            : "fuera-de-rango");
            if (address64 > 0xFFFFFFFFull || byte_count > 0xFFFFFFFFull ||
                !runtime.memory().contains(static_cast<std::uint32_t>(address64),
                                           static_cast<std::uint32_t>(byte_count))) {
                std::printf("   rango resuelto fuera de la RAM del invitado\n");
                continue;
            }
            for (std::uint32_t word = 0u; word < peek.count; ++word) {
                const std::uint32_t current = static_cast<std::uint32_t>(address64) + word * 4u;
                const std::uint32_t bits = runtime.memory().load32(current);
                float as_float = 0.0f;
                std::memcpy(&as_float, &bits, sizeof(as_float));
                std::printf("   %s  %s  u32 %10u  f32 %.9g\n",
                            hex32(current).c_str(), hex32(bits).c_str(), bits,
                            static_cast<double>(as_float));
            }
        }
        if (!battle_snapshot_output.empty()) {
            constexpr std::uint64_t kTickUs = 1'000'000ull / 60ull;
            fuc::port::BattleSnapshot snapshot = capture_battle_snapshot(
                runtime, fate::virtual_time_us() / kTickUs);
            std::array<FighterDiagnostics, fuc::port::player_count> diagnostics{};
            const std::array<std::uint32_t, fuc::port::player_count> fighter_pointers = {
                fuc::port::fate_psp_fighter_pointer_p1,
                fuc::port::fate_psp_fighter_pointer_p2,
            };
            for (std::size_t index = 0; index < diagnostics.size(); ++index) {
                const std::uint32_t base = runtime.memory().load32(fighter_pointers[index]);
                diagnostics[index].object = base;
                if (base == 0u) continue;
                diagnostics[index].maximum_health = runtime.memory().load32(
                    base + fuc::port::fate_psp_fighter_max_health_offset);
                diagnostics[index].damage_taken = runtime.memory().load32(
                    base + fuc::port::fate_psp_fighter_damage_taken_offset);
                const std::uint32_t animation_bits = runtime.memory().load32(
                    base + fuc::port::fate_psp_fighter_animation_seconds_offset);
                std::memcpy(&diagnostics[index].animation_seconds, &animation_bits,
                            sizeof(animation_bits));
                diagnostics[index].animation_frames = runtime.memory().load32(
                    base + fuc::port::fate_psp_fighter_animation_frames_offset);
            }
            write_battle_snapshot(battle_snapshot_output, snapshot, diagnostics,
                                  fate::virtual_time_us());
            std::printf("instantanea de combate: %s\n",
                        battle_snapshot_output.string().c_str());
        }
        std::printf("\nse detuvo: %s\n", runtime.stop_reason().c_str());
        std::printf("pc final : %s\n", hex32(runtime.cpu().pc).c_str());
        // The runtime already counts executions of the addresses named in
        // PSPRECOMP_COUNT_PC, but nothing printed them, so the instrument
        // existed and could not be read. It answers "is this routine ever
        // reached?" for up to eight addresses at almost no cost -- the cheapest
        // question to ask when a state machine refuses to advance.
        psprecomp::report_counted_pcs();

        // Snapshot diff: the words that moved between the snapshot and the end.
        //
        // Reported as GUEST addresses with both interpretations, because a
        // 32-bit word is only meaningful once you decide what it is, and at this
        // stage nobody knows yet.
        if (g_snapshot_taken) {
            auto &memory = runtime.memory();
            std::vector<std::uint8_t> final_state(memory.size());
            memory.copy_out(psprecomp::GuestMemory::kPhysicalBase, final_state);
            std::uint64_t changed = 0u;
            std::uint64_t shown = 0u;
            std::printf("\ndiferencia con la instantanea en el despacho %llu:\n",
                        static_cast<unsigned long long>(g_snapshot_at));
            for (std::size_t offset = 0; offset + 4u <= final_state.size(); offset += 4u) {
                if (std::memcmp(&g_snapshot[offset], &final_state[offset], 4u) == 0) continue;
                ++changed;
                if (shown >= diff_max) continue;
                ++shown;
                std::uint32_t before = 0u;
                std::uint32_t after = 0u;
                std::memcpy(&before, &g_snapshot[offset], 4u);
                std::memcpy(&after, &final_state[offset], 4u);
                float before_f = 0.0f;
                float after_f = 0.0f;
                std::memcpy(&before_f, &before, 4u);
                std::memcpy(&after_f, &after, 4u);
                std::printf("   %s  u32 %10u -> %-10u  f32 %12.6f -> %.6f\n",
                            hex32(static_cast<std::uint32_t>(
                                psprecomp::GuestMemory::kPhysicalBase + offset)).c_str(),
                            before, after, before_f, after_f);
            }
            std::printf("   palabras cambiadas: %llu (mostradas %llu, limite --diff-max)\n",
                        static_cast<unsigned long long>(changed),
                        static_cast<unsigned long long>(shown));
        }
        // The guest clock, reported always. `--script` aims presses at guest
        // time, so a run that advanced 3 s and one that advanced 200 s need
        // completely different scripts -- and without this the difference is
        // invisible, which made an out-of-range script look like a game that
        // ignored input.
        std::printf("tiempo de invitado final: %llu.%03llu s\n",
                    static_cast<unsigned long long>(fate::virtual_time_us() / 1000000ull),
                    static_cast<unsigned long long>((fate::virtual_time_us() / 1000ull) % 1000ull));
        if (use_profile) {
            fate::report_ge_flow();
            fate::report_hle_calls();
        }
        if (!stub_calls.empty()) {
            std::vector<std::pair<std::string, std::uint64_t>> ranked(stub_calls.begin(),
                                                                     stub_calls.end());
            std::sort(ranked.begin(), ranked.end(),
                      [](const auto &a, const auto &b) { return a.second > b.second; });
            std::uint64_t total = 0u;
            for (const auto &entry : ranked) total += entry.second;
            std::printf("\nimports distintos llamados: %zu, llamadas totales: %llu\n",
                        ranked.size(), static_cast<unsigned long long>(total));
            for (std::size_t index = 0; index < ranked.size() && index < 15u; ++index) {
                std::printf("   %-44s %llu\n", ranked[index].first.c_str(),
                            static_cast<unsigned long long>(ranked[index].second));
            }
        }
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 3;
    }
}
