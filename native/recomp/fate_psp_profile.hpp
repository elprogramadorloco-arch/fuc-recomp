#pragma once

// Minimal Fate profile for the recompiled PSP module: just enough HLE to carry
// control from `module_start` into the game's own main thread. See the .cpp for
// what is deliberately absent.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>

namespace psprecomp {
class Runtime;
}

namespace fuc::renderer::psp {
struct PrimitiveDraw;
}

namespace fate {

// `arena_start`/`arena_end` bound the guest memory this profile may hand out as
// thread stacks. The caller owns the decision, because only it knows where the
// loaded image ends.
// Graba los ultimos PC del invitado y los vuelca cuando `Main` termina.
void set_trace_pc(bool enabled);

// Vuelca el anillo de PC. Util con cualquier motivo de parada, no solo la salida limpia.
void dump_trace_pc_ring(const char *why);

// Compone la sonda de PC con el heartbeat de input del host. El runtime admite
// un solo heartbeat, por lo que el host debe reenviar cada PC cuando ambos
// instrumentos están activos.
void trace_pc_heartbeat(std::uint32_t pc);

// Imagen de disco a la que resuelve el dispositivo UMD en crudo (`umd0:`).
// El juego lee sus datos por sector, no fichero a fichero.
void set_umd_image(const std::filesystem::path &path);

// Root privado de Savedata. El host normal conserva la ruta por defecto; los
// arneses pueden aislar una partida de prueba sin reemplazar el progreso real.
void set_savedata_root(const std::filesystem::path &path);

// Raiz del contenido PSP extraido aportado localmente por el usuario. Los
// resolutores de medios (ATRAC/PMF) deben usar la misma raiz que sceIo.
void set_game_data_root(const std::filesystem::path &path);

// Directorio de fuentes PGF aportado localmente por el usuario. Vacio conserva
// la ruta privada de desarrollo por compatibilidad con los arneses existentes.
void set_font_root(const std::filesystem::path &path);

// Estado del pad que leera el invitado por `sceCtrlReadBufferPositive`.
// `buttons` son ya los bits del PSP; los ejes van centrados en 128. El host lo
// publica una vez por tick, antes de dejar correr al invitado.
// Tiempo virtual del invitado en microsegundos. El host lo necesita para
// cronometrar la entrada en el reloj del juego y no en el suyo.
std::uint64_t virtual_time_us();
[[nodiscard]] std::uint32_t display_framebuffer();

void set_pad_state(std::uint32_t buttons, std::uint8_t analog_x, std::uint8_t analog_y);

// Called at the guest's own VBlank wait, after the frame counter advances and
// before the waiting thread yields. A live host can stop the runtime here,
// capture one atomic BattleSnapshot and resume from the preserved CPU state.
using VblankHook = std::function<void(psprecomp::Runtime&, std::uint64_t, std::uint32_t)>;
void set_vblank_hook(VblankHook hook);

// Draw packets decoded from the original GE command stream. The callback sees
// guest memory at the PRIM boundary and may copy vertices/textures, but it must
// not retain pointers into GuestMemory.
using GeDrawHook = std::function<void(
    psprecomp::Runtime&, const fuc::renderer::psp::PrimitiveDraw&)>;
void set_ge_draw_hook(GeDrawHook hook);

// Interleaved signed 16-bit PCM produced by the PSP audio path.  The callback
// receives only data owned by the game; it must copy samples before returning.
using AudioPcmHook = std::function<void(
    std::span<const std::int16_t>, std::uint32_t, std::uint32_t,
    std::uint32_t, std::uint32_t, std::uint64_t)>;
void set_audio_pcm_hook(AudioPcmHook hook);

void install_profile(psprecomp::Runtime &runtime, std::uint32_t arena_start,
                     std::uint32_t arena_end, bool trace_scheduler = false,
                     bool trace_hle = false);

// Cuantas veces se llamo a cada import implementado, para saber por donde va la
// inicializacion sin tener que trazarlo todo.
void report_hle_calls();
void report_ge_flow();
void report_audio_flow();
void report_mpeg_flow();
void reset_ge_flow_stats();

[[nodiscard]] std::size_t thread_count();
[[nodiscard]] std::uint32_t arena_used();

} // namespace fate
