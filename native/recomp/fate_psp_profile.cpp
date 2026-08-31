// P7, first step: the minimum HLE that carries control from `module_start` into
// the game's own main thread.
//
// Measured before writing any of it: with every import stubbed, `module_start`
// runs to completion and calls exactly four of them -- set compiled SDK version,
// set compiler version, `sceKernelCreateThread`, `sceKernelStartThread` -- and
// then returns. So the whole of P6's remaining distance is a thread manager, and
// this file implements just enough of one.
//
// What it is honestly NOT: a PSP kernel. There is no delay/wake, no semaphores,
// no event flags, no callbacks, no preemption on anything but thread start, and
// virtual time does not advance. `external/PSPRecomp/profiles/vcs` is the
// reference for all of that and is ten thousand lines; this is the small slice
// that the measurement said was on the critical path. Anything the game needs
// beyond it will show up as a missing import, which is exactly how the next
// piece of work gets identified.

#include "fate_psp_profile.hpp"

#include "fuc/port/psp_atrac.hpp"
#include "fuc/port/psp_mpeg.hpp"
#include "fuc/port/psp_pgf.hpp"
#include "fuc/port/psp_vag.hpp"
#include "fuc/renderer/psp_ge_command.hpp"
#include "vcs_media_decoder.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <functional>
#include <iterator>
#include <utility>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fate {

void sas_mix_grain(psprecomp::Runtime& runtime, std::uint32_t output,
                   bool mix_existing, std::uint32_t mix_left = 0x1000u,
                   std::uint32_t mix_right = 0x1000u);

namespace {

// PSP kernel error codes, only the ones this slice can actually return.
constexpr std::uint32_t kErrorIllegalThreadId = 0x80020197u;
constexpr std::uint32_t kErrorUnknownThreadId = 0x80020198u;
constexpr std::uint32_t kErrorIllegalPriority = 0x80020193u;
constexpr std::uint32_t kErrorIllegalStackSize = 0x80020194u;
constexpr std::uint32_t kErrorNoMemory = 0x80020190u;
constexpr std::uint32_t kErrorIllegalMemSize = 0x800201A1u;
constexpr std::uint32_t kErrorDormant = 0x800201A2u;
constexpr std::uint32_t kErrorNotDormant = 0x800201A4u;
constexpr std::uint32_t kErrorIllegalSize = 0x800201BCu;
constexpr std::uint32_t kErrorIllegalAddress = 0x800200D3u;
constexpr std::uint32_t kUtilityInvalidStatus = 0x80110001u;
constexpr std::uint32_t kUtilityInvalidParamSize = 0x80110004u;
constexpr std::uint32_t kSavedataLoadNoData = 0x80110307u;

enum class ThreadState { Created, Ready, Running, Ended };

struct ThreadRecord {
    std::string name;
    std::uint32_t entry{};
    std::uint32_t initial_priority{};
    std::uint32_t priority{};
    std::uint32_t stack_size{};
    std::uint32_t attributes{};
    std::uint32_t stack_bottom{};
    std::uint32_t stack_top{};
    ThreadState state{ThreadState::Created};
};

struct Continuation {
    std::int32_t uid{};
    psprecomp::AllegrexContext context;
    // Virtual microseconds before which this thread must not be picked. A delay
    // is not a yield: without this a high-priority thread that sleeps in a loop
    // is chosen again immediately and starves everything below it, which is
    // exactly what kept the game from ever leaving its callback thread.
    std::uint64_t wake_at_us{0};
};

struct ThreadTable {
    std::int32_t next_uid{1};
    std::int32_t current_uid{0};
    std::map<std::int32_t, ThreadRecord> threads;
    std::deque<Continuation> ready;
    std::uint32_t arena_next{};
    std::uint32_t arena_end{};
    bool trace{false};
};

ThreadTable g_table;

// Defined with the interrupt records below. The scheduler calls this after it
// has selected a runnable continuation so a hardware VBlank can pre-empt guest
// code even when no thread is inside a sceDisplay wait.
void inject_periodic_sub_interrupt(psprecomp::Runtime& runtime,
                                   Continuation& continuation);

// Hilos bloqueados esperando a que OTRO termine, indexados por el uid esperado.
//
// Antes esto se hacia reintentando: se reencolaba al que esperaba con el `pc`
// puesto en el stub del import para que volviera a llamar. Eso choca con el
// envoltorio de import que genera PSPRecomp, que hace
// `if (ctx.pc == import_pc) ctx.pc = return_address`: cuando el hilo que se
// reanuda esta parado en **ese mismo stub**, el envoltorio cree que el HLE no
// movio el `pc` y ejecuta el retorno, saltandose la espera y con la direccion de
// retorno de otra invocacion. Medido: el contexto del modulo se saltaba asi su
// espera y seguia derecho al apagado mientras los hilos de carga seguian vivos.
//
// Una lista de espera de verdad no tiene ese problema: el que espera se guarda
// con el `pc` de retorno normal y el resultado ya puesto, y solo vuelve a la cola
// cuando la condicion se cumple.
std::map<std::int32_t, std::vector<Continuation>> g_thread_waiters;

std::string hex32(std::uint32_t value) {
    char buffer[11];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

std::string read_fixed_string(psprecomp::Runtime &runtime, std::uint32_t address,
                              std::uint32_t capacity) {
    if (!runtime.memory().contains(address, capacity)) return {};
    std::string value;
    value.reserve(capacity);
    for (std::uint32_t index = 0u; index < capacity; ++index) {
        const char byte = static_cast<char>(runtime.memory().load8(address + index));
        if (byte == '\0') break;
        value.push_back(byte);
    }
    return value;
}

void trace(const std::string &message) {
    if (g_table.trace) std::printf("[sched] %s\n", message.c_str());
}

bool g_trace_hle = false;
bool g_interrupts_enabled = true;

// Estado del pad que el host publica cada tick. El perfil no sabe nada de SDL ni
// de dispositivos: recibe ya los bits del PSP y los ejes centrados en 128, que es
// lo que el invitado espera leer.
std::uint32_t g_pad_buttons = 0u;
std::uint8_t g_pad_analog_x = 128u;
std::uint8_t g_pad_analog_y = 128u;

// Ring of the most recent guest PCs.
//
// `Main` is a long straight-line init sequence with no early-return branch in
// sight, yet it returns instead of entering its main loop. Static reading would
// mean following hundreds of instructions through dozens of calls; the last few
// addresses actually executed before the thread ends say it directly. The
// runtime heartbeat fires on every dispatch when the interval is 1, so this
// records into a fixed ring and dumps it at thread end.
constexpr std::size_t kPcRingSize = 96u;
std::uint32_t g_pc_ring[kPcRingSize]{};
std::size_t g_pc_ring_next = 0u;
bool g_trace_pc = false;

void heartbeat_record_pc(std::uint64_t, std::uint32_t pc) {
    g_pc_ring[g_pc_ring_next] = pc;
    g_pc_ring_next = (g_pc_ring_next + 1u) % kPcRingSize;
}

void dump_pc_ring(const char *why) {
    if (!g_trace_pc) return;
    std::printf("[pc] ultimos PC antes de %s:\n", why);
    for (std::size_t index = 0; index < kPcRingSize; ++index) {
        const std::uint32_t pc = g_pc_ring[(g_pc_ring_next + index) % kPcRingSize];
        if (pc == 0u) continue;
        std::printf("      0x%08X\n", pc);
    }
}
std::uint64_t g_virtual_time_us = 0u;
// gp del modulo, que todo hilo del invitado comparte.
std::uint32_t g_gp = 0u;
// El juego declara su SDK al arrancar. ThreadMan usa ese dato para escoger la
// variante real de SceKernelThreadInfo (104 bytes hasta 2.60, 108 despues), en
// vez de imponer el layout del host.
std::uint32_t g_compiled_sdk_version = 0u;
std::map<std::string, std::uint64_t> g_hle_calls;
// Direccion del stub de import que se esta atendiendo, para poder reintentar la
// llamada entera cuando una espera tiene que bloquear.
std::uint32_t g_current_hle_pc = 0u;

// Binds one import and, when tracing is on, prints every call with its first
// four arguments and the value returned. Iterating on P7 means running,
// reading which call misbehaved, and fixing it; without this the only signal is
// the name of the *next missing* import, which says nothing about the ones that
// already ran and returned something wrong.
void bind(psprecomp::Runtime &runtime, const char *library, std::uint32_t nid,
          const char *name,
          std::function<void(psprecomp::Runtime &, psprecomp::AllegrexContext &)> handler) {
    const std::string label = std::string(library) + "::" + name;
    runtime.register_hle(library, nid,
        [label, handler = std::move(handler)](psprecomp::Runtime &rt,
                                              psprecomp::AllegrexContext &ctx) {
            const std::uint32_t a0 = ctx.gpr[4];
            const std::uint32_t a1 = ctx.gpr[5];
            const std::uint32_t a2 = ctx.gpr[6];
            const std::uint32_t a3 = ctx.gpr[7];
            g_current_hle_pc = ctx.pc;
            ++g_hle_calls[label];
            handler(rt, ctx);
            if (g_trace_hle) {
                // The call site is printed too. Without it the trace says what
                // the game asked for but not which of 78,000 functions asked,
                // and that is what every investigation ends up needing: a retry
                // loop is only diagnosable once you know who is looping.
                // `ra` holds the return address, so the `jal` is at `ra - 8`:
                // the call plus its delay slot.
                const std::uint32_t return_address = ctx.gpr[31];
                std::printf("[hle] %-46s (%s, %s, %s, %s) -> %s  from=%s\n", label.c_str(),
                            hex32(a0).c_str(), hex32(a1).c_str(), hex32(a2).c_str(),
                            hex32(a3).c_str(), hex32(ctx.gpr[2]).c_str(),
                            hex32(return_address >= 8u ? return_address - 8u : 0u).c_str());
            }
        });
}

// For the calls whose whole correct behaviour is "succeed and do nothing".
void bind_success(psprecomp::Runtime &runtime, const char *library, std::uint32_t nid,
                  const char *name, std::uint32_t value = 0u) {
    bind(runtime, library, nid, name,
         [value](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             ctx.set_gpr(2, value);
         });
}

// Bump allocator, upward, 256-byte aligned. Thread stacks are never freed here:
// this slice only has to get the game started, and a real allocator is part of
// the memory manager work that comes later.
bool allocate_stack(std::uint32_t size, std::uint32_t &bottom, std::uint32_t &top) {
    const std::uint32_t aligned = (size + 0xFFu) & ~0xFFu;
    if (g_table.arena_next + aligned > g_table.arena_end) return false;
    bottom = g_table.arena_next;
    top = bottom + aligned;
    g_table.arena_next = top;
    return true;
}

std::uint32_t priority_of(std::int32_t uid) {
    const auto found = g_table.threads.find(uid);
    // The initial module context is not a registered thread. Treat it as the
    // lowest priority so that starting any real thread preempts it, which is
    // what a PSP module_start observes.
    return found == g_table.threads.end() ? 0xFFu : found->second.priority;
}

// PSP priorities are inverted: a smaller number schedules sooner. Equal
// priorities keep FIFO order, so a stable pick of the minimum is correct.
bool activate_next(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                   const char *reason) {
    // Which thread is LEAVING, not just which arrives. Tracing only the incoming
    // one hid the question that mattered -- when and why `Main` stopped being
    // current -- because every switch looked the same from the outside.
    if (g_table.trace) {
        const auto leaving = g_table.threads.find(g_table.current_uid);
        std::printf("[sched] sale uid=%d (%s) por %s, pc=%s\n", g_table.current_uid,
                    leaving == g_table.threads.end() ? "module" : leaving->second.name.c_str(),
                    reason, hex32(ctx.pc).c_str());
    }
    if (g_table.ready.empty()) {
        runtime.stop("no hay hilos ejecutables");
        return false;
    }

    // Nothing runnable yet means every ready thread is sleeping. Virtual time is
    // ours to move, so jump straight to the earliest wakeup instead of spinning:
    // deterministic, and it cannot livelock.
    const bool any_awake = std::any_of(g_table.ready.begin(), g_table.ready.end(),
                                       [](const Continuation &c) {
                                           return c.wake_at_us <= g_virtual_time_us;
                                       });
    if (!any_awake) {
        std::uint64_t earliest = UINT64_MAX;
        for (const auto &c : g_table.ready) earliest = std::min(earliest, c.wake_at_us);
        g_virtual_time_us = std::max(g_virtual_time_us, earliest);
    }

    // Among the threads that are awake, PSP priority decides: a smaller number
    // schedules sooner. Equal priorities keep FIFO order, so a stable minimum is
    // the right pick.
    auto selected = g_table.ready.end();
    for (auto it = g_table.ready.begin(); it != g_table.ready.end(); ++it) {
        if (it->wake_at_us > g_virtual_time_us) continue;
        if (selected == g_table.ready.end() || priority_of(it->uid) < priority_of(selected->uid)) {
            selected = it;
        }
    }
    if (selected == g_table.ready.end()) {
        runtime.stop("no hay hilos ejecutables");
        return false;
    }
    Continuation continuation = *selected;
    g_table.ready.erase(selected);
    g_table.current_uid = continuation.uid;
    inject_periodic_sub_interrupt(runtime, continuation);
    if (auto thread = g_table.threads.find(continuation.uid); thread != g_table.threads.end()) {
        thread->second.state = ThreadState::Running;
        psprecomp::set_runtime_thread_identity(continuation.uid, thread->second.name);
        trace(std::string(reason) + ": uid=" + std::to_string(continuation.uid) +
              " name=" + thread->second.name + " prio=" + std::to_string(thread->second.priority) +
              " pc=" + hex32(continuation.context.pc) +
              " (listos " + std::to_string(g_table.ready.size()) + ")");
    } else {
        psprecomp::set_runtime_thread_identity(continuation.uid, "module");
        trace(std::string(reason) + ": uid=" + std::to_string(continuation.uid) +
              " (module) pc=" + hex32(continuation.context.pc));
    }
    ctx = continuation.context;
    return true;
}

void end_current_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                        const char *reason) {
    const std::int32_t uid = g_table.current_uid;
    if (auto thread = g_table.threads.find(uid); thread != g_table.threads.end()) {
        thread->second.state = ThreadState::Ended;
        if (thread->second.name == "Main") dump_pc_ring("que Main terminase");
        // Quien esperaba a este hilo ya puede seguir.
        if (auto waiting = g_thread_waiters.find(uid); waiting != g_thread_waiters.end()) {
            for (auto &blocked : waiting->second) {
                trace("despierta: uid=" + std::to_string(blocked.uid) + " porque termino uid=" +
                      std::to_string(uid));
                g_table.ready.push_back(blocked);
            }
            g_thread_waiters.erase(waiting);
        }
        trace(std::string(reason) + ": uid=" + std::to_string(uid) +
              " name=" + thread->second.name + " terminado");
    } else {
        trace(std::string(reason) + ": contexto inicial del modulo terminado");
    }
    (void)activate_next(runtime, ctx, "thread-end");
}

void terminate_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                      bool erase) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (uid == g_table.current_uid) {
        ctx.set_gpr(2, 0u);
        end_current_thread(runtime, ctx, "terminate-self");
        return;
    }
    const auto found = g_table.threads.find(uid);
    if (found == g_table.threads.end()) { ctx.set_gpr(2, kErrorUnknownThreadId); return; }
    found->second.state = ThreadState::Ended;
    // A terminated thread must not keep a queued continuation, or the scheduler
    // would resume something that no longer exists.
    g_table.ready.erase(std::remove_if(g_table.ready.begin(), g_table.ready.end(),
                                       [uid](const Continuation &c) { return c.uid == uid; }),
                        g_table.ready.end());
    if (erase) g_table.threads.erase(found);
    ctx.set_gpr(2, 0u);
}

// Returning to address 0 is how a PSP thread entry function finishes: the kernel
// puts a trampoline there. Registering a function at 0 turns what would
// otherwise be "no recompiled function at 0x00000000" into a clean thread exit.
void thread_return_trampoline(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    end_current_thread(runtime, ctx, "return-to-zero");
}

void install_thread_manager(psprecomp::Runtime &runtime) {
    // sceKernelCreateThread(name, entry, priority, stackSize, attr, option)
    runtime.register_hle("ThreadManForUser", 0x446D8DE6u,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::string name = ctx.gpr[4] != 0u
                ? rt.memory().read_c_string(ctx.gpr[4], 128u) : "unnamed";
            const std::uint32_t entry = ctx.gpr[5];
            const std::uint32_t priority = ctx.gpr[6];
            const std::uint32_t requested_stack = ctx.gpr[7];
            if (priority > 0xFFu) { ctx.set_gpr(2, kErrorIllegalPriority); return; }
            if (requested_stack < 0x200u) { ctx.set_gpr(2, kErrorIllegalStackSize); return; }

            std::uint32_t bottom = 0u;
            std::uint32_t top = 0u;
            if (!allocate_stack(requested_stack, bottom, top) ||
                !rt.memory().contains(bottom, top - bottom)) {
                ctx.set_gpr(2, kErrorNoMemory);
                return;
            }
            rt.memory().zero(bottom, top - bottom);

            ThreadRecord record;
            record.name = name;
            record.entry = entry;
            record.initial_priority = priority;
            record.priority = priority;
            record.stack_size = top - bottom;
            record.attributes = ctx.gpr[8];
            record.stack_bottom = bottom;
            record.stack_top = top;

            const std::int32_t uid = g_table.next_uid++;
            trace("create: uid=" + std::to_string(uid) + " name=" + name +
                  " entry=" + hex32(entry) + " prio=" + std::to_string(priority) +
                  " stack=" + hex32(bottom) + ".." + hex32(top));
            g_table.threads.emplace(uid, std::move(record));
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });

    // sceKernelStartThread(thid, argSize, argPointer)
    runtime.register_hle("ThreadManForUser", 0xF475845Du,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto found = g_table.threads.find(uid);
            if (found == g_table.threads.end()) { ctx.set_gpr(2, kErrorUnknownThreadId); return; }
            ThreadRecord &thread = found->second;
            // DORMANT means "created but not started" OR "ended and not
            // deleted". On PSP `sceKernelExitThread` stops a thread without
            // destroying it, and starting it again is legitimate -- it is how a
            // game reuses a worker instead of creating one per job.
            //
            // Accepting only `Created` here was a real bug with a measurable
            // cost: this game's sound manager runs one `bgm play thread` per
            // track and restarts the same thread for the next one. Refusing it
            // returned NOT_DORMANT 3537 times, no second track ever loaded, and
            // the game sat waiting without even reading input.
            if (thread.state != ThreadState::Created &&
                thread.state != ThreadState::Ended) {
                ctx.set_gpr(2, kErrorNotDormant);
                return;
            }

            const std::uint32_t arg_size = ctx.gpr[5];
            const std::uint32_t arg_pointer = ctx.gpr[6];
            std::uint32_t sp = thread.stack_top - 0x100u;

            psprecomp::AllegrexContext next{};
            if (arg_pointer != 0u && arg_size != 0u) {
                const std::uint32_t aligned = (arg_size + 0xFu) & ~0xFu;
                if (sp < thread.stack_bottom + aligned + 64u ||
                    !rt.memory().contains(arg_pointer, arg_size)) {
                    ctx.set_gpr(2, kErrorIllegalThreadId);
                    return;
                }
                sp -= aligned;
                std::vector<std::uint8_t> arguments(arg_size);
                rt.memory().copy_out(arg_pointer, arguments);
                rt.memory().copy_in(sp, arguments);
                next.set_gpr(4, arg_size);
                next.set_gpr(5, sp);
            }
            sp -= 64u;

            // k0 is not scratch on the PSP: the kernel points it at a per-thread
            // control block, and guest code uses it directly. Leaving it zero
            // means the first `sw ...,N(k0)` writes to low memory -- measured
            // here as a fault storing to 0x00000004 from `0x089ACAC8`, which
            // looked like a bad module id until the register dump showed
            // k0 = 0. The block sits below the stack top, and the fields written
            // below are the ones the kernel populates.
            const std::uint32_t kernel_context = thread.stack_top - 0x100u;
            rt.memory().zero(kernel_context, 0x100u);
            rt.memory().store32(kernel_context + 0xC0u, static_cast<std::uint32_t>(uid));
            rt.memory().store32(kernel_context + 0xC8u, thread.stack_bottom);
            rt.memory().store32(kernel_context + 0xF8u, 0xFFFFFFFFu);
            rt.memory().store32(kernel_context + 0xFCu, 0xFFFFFFFFu);
            rt.memory().store32(thread.stack_bottom, static_cast<std::uint32_t>(uid));

            next.set_gpr(26, kernel_context); // k0
            g_gp = ctx.gpr[28];
            next.set_gpr(28, ctx.gpr[28]);    // gp is shared
            next.set_gpr(29, sp);
            next.set_gpr(30, sp);
            next.set_gpr(31, 0u);             // return to the trampoline at 0
            next.pc = thread.entry;

            thread.state = ThreadState::Ready;
            g_table.ready.push_back({uid, next});
            trace("start: uid=" + std::to_string(uid) + " name=" + thread.name +
                  " sp=" + hex32(sp));

            // A start only takes the CPU when the new thread's priority is
            // strictly higher, which on PSP means numerically smaller.
            const std::int32_t caller_uid = g_table.current_uid;
            if (thread.priority < priority_of(caller_uid)) {
                psprecomp::AllegrexContext caller = ctx;
                caller.set_gpr(2, 0u);
                caller.pc = ctx.gpr[31];
                g_table.ready.push_back({caller_uid, caller});
                (void)activate_next(rt, ctx, "thread-start-preempt");
            } else {
                ctx.set_gpr(2, 0u);
            }
        });

    // sceKernelExitThread(status) and sceKernelExitDeleteThread(status)
    for (const std::uint32_t nid : {0xAA73C935u, 0x809CE29Bu}) {
        runtime.register_hle("ThreadManForUser", nid,
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                end_current_thread(rt, ctx, "exit-thread");
            });
    }

    bind(runtime, "ThreadManForUser", 0x293B45B8u, "sceKernelGetThreadId",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             ctx.set_gpr(2, static_cast<std::uint32_t>(g_table.current_uid));
         });

    // The names below come from the public PSP NID tables, not from inference.
    bind(runtime, "ThreadManForUser", 0x94AA61EEu, "sceKernelGetThreadCurrentPriority",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             ctx.set_gpr(2, priority_of(g_table.current_uid));
         });

    // sceKernelReferThreadStatus(thid, info). El layout y el contrato de tamano
    // proceden de PSPSDK y de la implementacion publica de PPSSPP. Se escribe
    // solo lo que el llamador pidio en info->size; UID 0 designa el hilo actual.
    bind(runtime, "ThreadManForUser", 0x17C1684Eu, "sceKernelReferThreadStatus",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             constexpr std::uint32_t kThreadInfoSize = 104u;
             constexpr std::uint32_t kThreadInfoSizeAfter260 = 108u;

             const std::int32_t requested_uid = static_cast<std::int32_t>(ctx.gpr[4]);
             const std::int32_t uid = requested_uid == 0 ? g_table.current_uid : requested_uid;
             const auto found = g_table.threads.find(uid);
             if (found == g_table.threads.end()) {
                 ctx.set_gpr(2, kErrorUnknownThreadId);
                 return;
             }

             const std::uint32_t info = ctx.gpr[5];
             if ((info & 3u) != 0u || !rt.memory().contains(info, 4u)) {
                 ctx.set_gpr(2, kErrorIllegalAddress);
                 return;
             }

             const std::uint32_t wanted = rt.memory().load32(info);
             const std::uint32_t native_size =
                 g_compiled_sdk_version > 0x02060010u
                     ? kThreadInfoSizeAfter260 : kThreadInfoSize;
             if (wanted > native_size) {
                 ctx.set_gpr(2, kErrorIllegalSize);
                 return;
             }
             if (wanted == 0u) {
                 ctx.set_gpr(2, 0u);
                 return;
             }
             if (!rt.memory().contains(info, wanted)) {
                 ctx.set_gpr(2, kErrorIllegalAddress);
                 return;
             }

             rt.memory().zero(info, wanted);
             const auto store32_if_present = [&](std::uint32_t offset, std::uint32_t value) {
                 if (offset + 4u <= wanted) rt.memory().store32(info + offset, value);
             };
             store32_if_present(0u, native_size);

             const ThreadRecord &thread = found->second;
             for (std::size_t index = 0u; index < std::min<std::size_t>(31u, thread.name.size());
                  ++index) {
                 const std::uint32_t offset = 4u + static_cast<std::uint32_t>(index);
                 if (offset >= wanted) break;
                 rt.memory().store8(info + offset,
                                    static_cast<std::uint8_t>(thread.name[index]));
             }

             std::uint32_t status = 2u; // PSP_THREAD_READY
             if (uid == g_table.current_uid) {
                 status = 1u; // PSP_THREAD_RUNNING
             } else if (thread.state == ThreadState::Created ||
                        thread.state == ThreadState::Ended) {
                 status = 16u; // PSP_THREAD_STOPPED / dormant
             } else {
                 const bool sleeping = std::any_of(
                     g_table.ready.begin(), g_table.ready.end(),
                     [uid](const Continuation &candidate) {
                         return candidate.uid == uid && candidate.wake_at_us > g_virtual_time_us;
                     });
                 bool waiting_for_thread = false;
                 for (const auto &[_, waiters] : g_thread_waiters) {
                     if (std::any_of(waiters.begin(), waiters.end(),
                                     [uid](const Continuation &candidate) {
                                         return candidate.uid == uid;
                                     })) {
                         waiting_for_thread = true;
                         break;
                     }
                 }
                 if (sleeping || waiting_for_thread) status = 4u; // PSP_THREAD_WAITING
             }

             store32_if_present(36u, thread.attributes | 0xFFu);
             store32_if_present(40u, status);
             store32_if_present(44u, thread.entry);
             store32_if_present(48u, thread.stack_bottom);
             store32_if_present(52u, thread.stack_size);
             store32_if_present(56u, g_gp);
             store32_if_present(60u, thread.initial_priority);
             store32_if_present(64u, thread.priority);
             // waitType, waitId y wakeupCount quedan en cero hasta que el
             // scheduler conserve esa contabilidad por hilo.
             const std::uint32_t exit_status =
                 thread.state == ThreadState::Created ? kErrorDormant
                 : thread.state == ThreadState::Ended ? 0u
                 : kErrorNotDormant;
             store32_if_present(80u, exit_status);
             // runClocks y los tres contadores de preempcion/release quedan en
             // cero: aun no existe contabilidad por hilo que permita inventarlos.
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "ThreadManForUser", 0x71BC9871u, "sceKernelChangeThreadPriority",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
             const std::int32_t target = uid == 0 ? g_table.current_uid : uid;
             const auto found = g_table.threads.find(target);
             if (found == g_table.threads.end()) { ctx.set_gpr(2, kErrorUnknownThreadId); return; }
             found->second.priority = ctx.gpr[5];
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "ThreadManForUser", 0x3B183E26u, "sceKernelGetThreadExitStatus",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const auto found = g_table.threads.find(static_cast<std::int32_t>(ctx.gpr[4]));
             ctx.set_gpr(2, found == g_table.threads.end() ? kErrorUnknownThreadId : 0u);
         });

    bind(runtime, "ThreadManForUser", 0x9FA03CD3u, "sceKernelDeleteThread",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
             if (uid == g_table.current_uid) { ctx.set_gpr(2, kErrorIllegalThreadId); return; }
             ctx.set_gpr(2, g_table.threads.erase(uid) != 0u ? 0u : kErrorUnknownThreadId);
         });

    bind(runtime, "ThreadManForUser", 0x616403BAu, "sceKernelTerminateThread",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             terminate_thread(rt, ctx, false);
         });
    bind(runtime, "ThreadManForUser", 0x383F7BCCu, "sceKernelTerminateDeleteThread",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             terminate_thread(rt, ctx, true);
         });

    // No callback is ever queued in this profile, so none is ever pending.
    bind_success(runtime, "ThreadManForUser", 0x349D6D6Cu, "sceKernelCheckCallback", 0u);

    // Virtual time. There is no real clock behind the guest: the counter simply
    // advances, both on its own reads and on an explicit delay. That is enough
    // for a timing loop to make progress and it keeps runs deterministic.
    //
    // It is NOT a scheduler: a delay does not hand the CPU to anyone. If the game
    // ever depends on another thread running during a delay, that shows up as a
    // hang and this is the thing to fix.
    bind(runtime, "ThreadManForUser", 0x369ED59Du, "sceKernelGetSystemTimeLow",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             g_virtual_time_us += 100u;
             ctx.set_gpr(2, static_cast<std::uint32_t>(g_virtual_time_us));
         });
    // A delay gives up the CPU. It parks the caller with a wakeup time and
    // switches; if nothing else is runnable the scheduler simply advances
    // virtual time to that wakeup and resumes it.
    //
    // Getting this wrong is what stalled the game: the delay used to return
    // immediately, so the priority-17 exit-callback thread looped forever on
    // `sceKernelDelayThreadCB` and the priority-32 main thread never ran again.
    for (const auto &entry : {std::pair<std::uint32_t, const char *>{0xCEADEB47u, "sceKernelDelayThread"},
                              std::pair<std::uint32_t, const char *>{0x68DA9E36u, "sceKernelDelayThreadCB"}}) {
        bind(runtime, "ThreadManForUser", entry.first, entry.second,
             [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                 const std::uint64_t microseconds = ctx.gpr[4];
                 psprecomp::AllegrexContext sleeping = ctx;
                 sleeping.set_gpr(2, 0u);
                 sleeping.pc = ctx.gpr[31];
                 g_table.ready.push_back({g_table.current_uid, sleeping,
                                          g_virtual_time_us + microseconds});
                 (void)activate_next(rt, ctx, "delay");
             });
    }
}

// Partition memory. A bump allocator over the same arena the thread stacks come
// from, with a uid per block so `sceKernelGetBlockHeadAddr` can resolve it.
//
// Freeing is accepted and recorded but the memory is not reused. That is a
// deliberate simplification for getting the game started, not a design: a title
// that allocates and frees in a loop would exhaust the arena. If that shows up,
// this is the thing to replace.
struct MemoryBlock {
    std::string name;
    std::uint32_t address{};
    std::uint32_t size{};
    bool freed{false};
};

std::map<std::int32_t, MemoryBlock> g_blocks;

// Pools de bloques fijos. Se guarda el reparto real porque el juego pide bloques
// y escribe en ellos: devolver una direccion cualquiera acabaria en corrupcion
// silenciosa, que es mucho peor que un fallo limpio.
struct FixedPool {
    std::string name;
    std::uint32_t base{};
    std::uint32_t block_size{};
    std::uint32_t block_count{};
    std::uint32_t next_free{};
};
std::map<std::int32_t, FixedPool> g_pools;
std::int32_t g_next_block_uid = 0x1000;

void install_memory_manager(psprecomp::Runtime &runtime) {
    // sceKernelAllocPartitionMemory(partition, name, type, size, address)
    runtime.register_hle("SysMemUserForUser", 0x237DBD4Fu,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::string name = ctx.gpr[5] != 0u
                ? rt.memory().read_c_string(ctx.gpr[5], 128u) : "unnamed";
            const std::uint32_t type = ctx.gpr[6];
            const std::uint32_t size = ctx.gpr[7];
            const std::uint32_t requested_alignment = ctx.gpr[8];
            // Types 3 and 4 are the aligned variants and carry the alignment in
            // the fifth argument; the others are 256-byte aligned like the
            // kernel's own blocks.
            std::uint32_t alignment = (type == 3u || type == 4u) ? requested_alignment : 0x100u;
            if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) alignment = 0x100u;

            const std::uint32_t base = (g_table.arena_next + alignment - 1u) & ~(alignment - 1u);
            const std::uint32_t aligned_size = (size + 0xFFu) & ~0xFFu;
            if (base + aligned_size > g_table.arena_end ||
                !rt.memory().contains(base, aligned_size)) {
                ctx.set_gpr(2, kErrorNoMemory);
                return;
            }
            g_table.arena_next = base + aligned_size;
            rt.memory().zero(base, aligned_size);

            const std::int32_t uid = g_next_block_uid++;
            g_blocks.emplace(uid, MemoryBlock{name, base, aligned_size, false});
            trace("alloc: uid=" + std::to_string(uid) + " name=" + name +
                  " size=" + std::to_string(size) + " en " + hex32(base));
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });

    // sceKernelCreateFpl(name, partition, attr, blocksize, blocks, opt)
    //
    // A fixed-size memory pool. Notably the module imports ONLY the create call:
    // there is no `sceKernelAllocateFpl`, `FreeFpl` or `DeleteFpl` in its import
    // table, so nothing here ever hands blocks out. What it has to do is reserve
    // the backing memory for real and return a usable uid, because a negative
    // return is an error the game would react to.
    runtime.register_hle("ThreadManForUser", 0xC07BB470u,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::string name = ctx.gpr[4] != 0u
                ? rt.memory().read_c_string(ctx.gpr[4], 128u) : "unnamed";
            const std::uint32_t block_size = ctx.gpr[7];
            const std::uint32_t block_count = ctx.gpr[8];
            const std::uint64_t total = static_cast<std::uint64_t>(block_size) * block_count;
            if (block_size == 0u || block_count == 0u || total > 0xFFFFFFFFull) {
                ctx.set_gpr(2, kErrorIllegalMemSize);
                return;
            }
            const std::uint32_t aligned_size =
                (static_cast<std::uint32_t>(total) + 0xFFu) & ~0xFFu;
            const std::uint32_t base = (g_table.arena_next + 0xFFu) & ~0xFFu;
            if (base + aligned_size > g_table.arena_end ||
                !rt.memory().contains(base, aligned_size)) {
                ctx.set_gpr(2, kErrorNoMemory);
                return;
            }
            g_table.arena_next = base + aligned_size;
            rt.memory().zero(base, aligned_size);
            const std::int32_t uid = g_next_block_uid++;
            g_blocks.emplace(uid, MemoryBlock{name, base, aligned_size, false});
            g_pools.emplace(uid, FixedPool{name, base, block_size, block_count, 0u});
            trace("fpl: uid=" + std::to_string(uid) + " name=" + name + " " +
                  std::to_string(block_count) + "x" + std::to_string(block_size) +
                  " en " + hex32(base));
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });

    // sceKernelAllocateFplCB(uid, data, timeout)
    //
    // Hands out one block from a fixed-size pool and writes its address through
    // `data`. The pool's memory was really reserved at create time, so the
    // address the game receives is one it can safely write to.
    //
    // The module imports no `FreeFpl`, so blocks are handed out and never
    // returned; running the pool dry is reported as a real error rather than
    // wrapping around, because reusing a live block would corrupt whatever the
    // game already put there. The `CB` suffix means the call may run pending
    // callbacks while it waits, and there is nothing to wait for here.
    runtime.register_hle("ThreadManForUser", 0xE7282CB6u,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto found = g_pools.find(static_cast<std::int32_t>(ctx.gpr[4]));
            if (found == g_pools.end()) {
                ctx.set_gpr(2, kErrorUnknownThreadId);
                return;
            }
            FixedPool &pool = found->second;
            if (pool.next_free >= pool.block_count) {
                ctx.set_gpr(2, kErrorNoMemory);
                return;
            }
            const std::uint32_t address = pool.base + pool.next_free * pool.block_size;
            ++pool.next_free;
            if (ctx.gpr[5] != 0u) rt.memory().store32(ctx.gpr[5], address);
            trace("fpl-alloc: uid=" + std::to_string(found->first) + " bloque " +
                  std::to_string(pool.next_free) + "/" + std::to_string(pool.block_count) +
                  " en " + hex32(address));
            ctx.set_gpr(2, 0u);
        });

    // sceKernelGetBlockHeadAddr(uid)
    runtime.register_hle("SysMemUserForUser", 0x9D9A5BA1u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto found = g_blocks.find(uid);
            ctx.set_gpr(2, found == g_blocks.end() ? 0u : found->second.address);
        });

    // sceKernelFreePartitionMemory(uid)
    runtime.register_hle("SysMemUserForUser", 0xB6D61D02u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto found = g_blocks.find(uid);
            if (found == g_blocks.end()) { ctx.set_gpr(2, kErrorNoMemory); return; }
            found->second.freed = true;
            ctx.set_gpr(2, 0u);
        });

    // sceKernelPrintf is variadic; the format string would have to be walked
    // against the guest stack to render it. Accepted and ignored.
    runtime.register_hle("SysMemUserForUser", 0x13A5ABEFu,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
}

// Synchronisation primitives.
//
// The wait policy is the important part and it is deliberate: a wait that cannot
// be satisfied right now does NOT silently succeed. It re-queues the caller,
// runs whatever else is ready, and if nothing is, the run stops saying so. A
// wait that returned success without its condition being true would corrupt the
// guest's state quietly, which is exactly the failure mode this profile keeps
// being bitten by.
struct Semaphore {
    std::string name;
    std::int32_t count{};
    std::int32_t max_count{};
};

struct EventFlag {
    std::string name;
    std::uint32_t bits{};
};

struct Callback {
    std::string name;
    std::uint32_t entry{};
    std::uint32_t argument{};
};

struct LwMutex {
    std::uint32_t workarea{};
};

std::map<std::int32_t, Semaphore> g_semaphores;
std::map<std::int32_t, EventFlag> g_event_flags;
std::map<std::int32_t, Callback> g_callbacks;
std::map<std::int32_t, LwMutex> g_lw_mutexes;
std::int32_t g_next_sync_uid = 0x2000;

constexpr std::uint32_t kErrorWaitTimeout = 0x800201A8u;
constexpr std::uint32_t kErrorUnknownSemaId = 0x800201AEu;
constexpr std::uint32_t kErrorUnknownEventFlagId = 0x800201B1u;

// Blocks the current thread by parking its continuation at the call site so the
// wait is retried when it next runs, then switches away. Returns false when
// nothing else can run, which is a real deadlock and is reported as one.
bool park_and_switch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                     const char *what) {
    if (g_table.ready.empty()) {
        runtime.stop(std::string("bloqueo: ") + what +
                     " no se puede satisfacer y no hay otro hilo ejecutable");
        return false;
    }
    psprecomp::AllegrexContext waiting = ctx;
    // Resume at the import STUB, not at `ra`. Resuming at the return address
    // would leave the call as if it had returned, so the wait would never be
    // re-evaluated and the caller would carry on with a condition that is still
    // false. Re-entering the stub re-issues the whole call.
    waiting.pc = g_current_hle_pc != 0u ? g_current_hle_pc : ctx.gpr[31];
    trace(std::string("aparca: uid=") + std::to_string(g_table.current_uid) + " por " + what +
          ", reanuda en " + hex32(waiting.pc) + " (hle_pc=" + hex32(g_current_hle_pc) +
          ", ra=" + hex32(ctx.gpr[31]) + ")");

    // A retried wait has to COST virtual time, or the world stops moving.
    //
    // Without this the parked thread is immediately runnable again, so it always
    // counts as awake; `activate_next` only advances virtual time when nothing
    // is awake, so a sleeping thread never reaches its wakeup and the waiter
    // spins forever on a condition only that sleeper could satisfy. Measured
    // exactly that: `user_main` waiting on `Main` while `Main` sat in a delay
    // that could never expire.
    //
    // Charging a small slice makes the retry behave like the spin it is, lets
    // time reach the earliest wakeup, and keeps the whole thing deterministic.
    constexpr std::uint64_t kWaitRetrySliceUs = 100u;
    g_table.ready.push_back({g_table.current_uid, waiting,
                             g_virtual_time_us + kWaitRetrySliceUs});
    return activate_next(runtime, ctx, "wait");
}

void install_sync(psprecomp::Runtime &runtime) {
    bind(runtime, "ThreadManForUser", 0x19CFF145u, "sceKernelCreateLwMutex",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t workarea = ctx.gpr[4];
             const std::int32_t initial_count = static_cast<std::int32_t>(ctx.gpr[7]);
             if (!rt.memory().contains(workarea, 32u) || initial_count < 0) {
                 ctx.set_gpr(2, 0x800200D3u);
                 return;
             }
             const std::int32_t uid = g_next_sync_uid++;
             // SceLwMutexWorkarea is guest-owned: lockLevel, lockThread, attr,
             // waiters, uid and three padding words.
             rt.memory().store32(workarea + 0u, static_cast<std::uint32_t>(initial_count));
             rt.memory().store32(workarea + 4u,
                 initial_count != 0 ? static_cast<std::uint32_t>(g_table.current_uid)
                                    : 0xFFFFFFFFu);
             rt.memory().store32(workarea + 8u, ctx.gpr[6]);
             rt.memory().store32(workarea + 12u, 0u);
             rt.memory().store32(workarea + 16u, static_cast<std::uint32_t>(uid));
             rt.memory().zero(workarea + 20u, 12u);
             g_lw_mutexes.emplace(uid, LwMutex{workarea});
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "ThreadManForUser", 0x60107536u, "sceKernelDeleteLwMutex",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t workarea = ctx.gpr[4];
             if (!rt.memory().contains(workarea, 20u)) {
                 ctx.set_gpr(2, 0x800200D3u);
                 return;
             }
             const auto uid = static_cast<std::int32_t>(rt.memory().load32(workarea + 16u));
             const auto found = g_lw_mutexes.find(uid);
             if (found == g_lw_mutexes.end() || found->second.workarea != workarea) {
                 ctx.set_gpr(2, 0x800201C3u);
                 return;
             }
             g_lw_mutexes.erase(found);
             rt.memory().store32(workarea + 16u, 0xFFFFFFFFu);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "Kernel_Library", 0xBEA46419u, "sceKernelLockLwMutex",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t workarea = ctx.gpr[4];
             const std::int32_t count = static_cast<std::int32_t>(ctx.gpr[5]);
             if (!rt.memory().contains(workarea, 20u) || count <= 0) {
                 ctx.set_gpr(2, 0x800200D3u);
                 return;
             }
             const auto uid = static_cast<std::int32_t>(rt.memory().load32(workarea + 16u));
             const auto found = g_lw_mutexes.find(uid);
             if (found == g_lw_mutexes.end() || found->second.workarea != workarea) {
                 ctx.set_gpr(2, 0x800201C3u);
                 return;
             }
             const auto level = static_cast<std::int32_t>(rt.memory().load32(workarea));
             const auto owner = static_cast<std::int32_t>(rt.memory().load32(workarea + 4u));
             const std::uint32_t attributes = rt.memory().load32(workarea + 8u);
             if (level == 0) {
                 rt.memory().store32(workarea, static_cast<std::uint32_t>(count));
                 rt.memory().store32(workarea + 4u,
                                     static_cast<std::uint32_t>(g_table.current_uid));
                 ctx.set_gpr(2, 0u);
                 return;
             }
             if (owner == g_table.current_uid) {
                 if ((attributes & 0x0200u) == 0u) {
                     ctx.set_gpr(2, 0x800201C0u);
                     return;
                 }
                 rt.memory().store32(workarea,
                     static_cast<std::uint32_t>(level + count));
                 ctx.set_gpr(2, 0u);
                 return;
             }
             const std::uint32_t timeout = ctx.gpr[6];
             if (timeout != 0u && rt.memory().contains(timeout, 4u) &&
                 rt.memory().load32(timeout) == 0u) {
                 ctx.set_gpr(2, kErrorWaitTimeout);
                 return;
             }
             (void)park_and_switch(rt, ctx, "lwmutex");
         });

    bind(runtime, "Kernel_Library", 0x15B6446Bu, "sceKernelUnlockLwMutex",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t workarea = ctx.gpr[4];
             const std::int32_t count = static_cast<std::int32_t>(ctx.gpr[5]);
             if (!rt.memory().contains(workarea, 20u) || count <= 0) {
                 ctx.set_gpr(2, 0x800200D3u);
                 return;
             }
             const auto uid = static_cast<std::int32_t>(rt.memory().load32(workarea + 16u));
             const auto found = g_lw_mutexes.find(uid);
             if (found == g_lw_mutexes.end() || found->second.workarea != workarea) {
                 ctx.set_gpr(2, 0x800201C3u);
                 return;
             }
             const auto level = static_cast<std::int32_t>(rt.memory().load32(workarea));
             const auto owner = static_cast<std::int32_t>(rt.memory().load32(workarea + 4u));
             if (owner != g_table.current_uid || level < count) {
                 ctx.set_gpr(2, 0x800201C1u);
                 return;
             }
             const std::int32_t remaining = level - count;
             rt.memory().store32(workarea, static_cast<std::uint32_t>(remaining));
             if (remaining == 0) rt.memory().store32(workarea + 4u, 0xFFFFFFFFu);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "ThreadManForUser", 0xE81CAF8Fu, "sceKernelCreateCallback",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::string name = ctx.gpr[4] != 0u
                 ? rt.memory().read_c_string(ctx.gpr[4], 128u) : "unnamed";
             const std::int32_t uid = g_next_sync_uid++;
             g_callbacks.emplace(uid, Callback{name, ctx.gpr[5], ctx.gpr[6]});
             ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
         });

    bind(runtime, "ThreadManForUser", 0xD6DA4BA1u, "sceKernelCreateSema",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::string name = ctx.gpr[4] != 0u
                 ? rt.memory().read_c_string(ctx.gpr[4], 128u) : "unnamed";
             const std::int32_t initial = static_cast<std::int32_t>(ctx.gpr[6]);
             const std::int32_t maximum = static_cast<std::int32_t>(ctx.gpr[7]);
             const std::int32_t uid = g_next_sync_uid++;
             g_semaphores.emplace(uid, Semaphore{name, initial, maximum});
             ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
         });

    bind(runtime, "ThreadManForUser", 0x28B6489Cu, "sceKernelDeleteSema",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
             ctx.set_gpr(2, g_semaphores.erase(uid) != 0u ? 0u : kErrorUnknownSemaId);
         });

    bind(runtime, "ThreadManForUser", 0x3F53E640u, "sceKernelSignalSema",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const auto found = g_semaphores.find(static_cast<std::int32_t>(ctx.gpr[4]));
             if (found == g_semaphores.end()) { ctx.set_gpr(2, kErrorUnknownSemaId); return; }
             found->second.count += static_cast<std::int32_t>(ctx.gpr[5]);
             if (found->second.max_count > 0 && found->second.count > found->second.max_count) {
                 found->second.count = found->second.max_count;
             }
             ctx.set_gpr(2, 0u);
         });

    for (const auto &entry : {std::pair<std::uint32_t, const char *>{0x4E3A1105u, "sceKernelWaitSema"},
                              std::pair<std::uint32_t, const char *>{0x6D212BACu, "sceKernelWaitSemaCB"}}) {
        bind(runtime, "ThreadManForUser", entry.first, entry.second,
             [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                 const auto found = g_semaphores.find(static_cast<std::int32_t>(ctx.gpr[4]));
                 if (found == g_semaphores.end()) { ctx.set_gpr(2, kErrorUnknownSemaId); return; }
                 const std::int32_t needed = static_cast<std::int32_t>(ctx.gpr[5]);
                 if (found->second.count >= needed) {
                     found->second.count -= needed;
                     ctx.set_gpr(2, 0u);
                     return;
                 }
                 (void)park_and_switch(rt, ctx, "sceKernelWaitSema");
             });
    }

    bind(runtime, "ThreadManForUser", 0x55C20A00u, "sceKernelCreateEventFlag",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::string name = ctx.gpr[4] != 0u
                 ? rt.memory().read_c_string(ctx.gpr[4], 128u) : "unnamed";
             const std::int32_t uid = g_next_sync_uid++;
             g_event_flags.emplace(uid, EventFlag{name, ctx.gpr[6]});
             ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
         });

    bind(runtime, "ThreadManForUser", 0xEF9E4C70u, "sceKernelDeleteEventFlag",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
             ctx.set_gpr(2, g_event_flags.erase(uid) != 0u ? 0u : kErrorUnknownEventFlagId);
         });

    bind(runtime, "ThreadManForUser", 0x1FB15A32u, "sceKernelSetEventFlag",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const auto found = g_event_flags.find(static_cast<std::int32_t>(ctx.gpr[4]));
             if (found == g_event_flags.end()) { ctx.set_gpr(2, kErrorUnknownEventFlagId); return; }
             found->second.bits |= ctx.gpr[5];
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "ThreadManForUser", 0x812346E4u, "sceKernelClearEventFlag",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const auto found = g_event_flags.find(static_cast<std::int32_t>(ctx.gpr[4]));
             if (found == g_event_flags.end()) { ctx.set_gpr(2, kErrorUnknownEventFlagId); return; }
             found->second.bits &= ctx.gpr[5];
             ctx.set_gpr(2, 0u);
         });

    // Wait modes: bit 0 selects OR over AND, 0x10 clears all the flag's bits on
    // success and 0x20 clears only the ones waited for.
    auto match_event = [](EventFlag &flag, std::uint32_t bits, std::uint32_t mode,
                          std::uint32_t &observed) {
        const bool any = (mode & 1u) != 0u;
        const bool satisfied = any ? (flag.bits & bits) != 0u : (flag.bits & bits) == bits;
        if (!satisfied) return false;
        observed = flag.bits;
        if ((mode & 0x10u) != 0u) flag.bits = 0u;
        else if ((mode & 0x20u) != 0u) flag.bits &= ~bits;
        return true;
    };

    bind(runtime, "ThreadManForUser", 0x30FD48F0u, "sceKernelPollEventFlag",
         [match_event](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const auto found = g_event_flags.find(static_cast<std::int32_t>(ctx.gpr[4]));
             if (found == g_event_flags.end()) { ctx.set_gpr(2, kErrorUnknownEventFlagId); return; }
             std::uint32_t observed = 0u;
             const bool ok = match_event(found->second, ctx.gpr[5], ctx.gpr[6], observed);
             if (ctx.gpr[7] != 0u && rt.memory().contains(ctx.gpr[7], 4u)) {
                 rt.memory().store32(ctx.gpr[7], observed);
             }
             // A poll that does not match reports a timeout instead of blocking.
             ctx.set_gpr(2, ok ? 0u : kErrorWaitTimeout);
         });

    bind(runtime, "ThreadManForUser", 0x402FCF22u, "sceKernelWaitEventFlag",
         [match_event](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const auto found = g_event_flags.find(static_cast<std::int32_t>(ctx.gpr[4]));
             if (found == g_event_flags.end()) { ctx.set_gpr(2, kErrorUnknownEventFlagId); return; }
             std::uint32_t observed = 0u;
             if (match_event(found->second, ctx.gpr[5], ctx.gpr[6], observed)) {
                 if (ctx.gpr[7] != 0u && rt.memory().contains(ctx.gpr[7], 4u)) {
                     rt.memory().store32(ctx.gpr[7], observed);
                 }
                 ctx.set_gpr(2, 0u);
                 return;
             }
             (void)park_and_switch(rt, ctx, "sceKernelWaitEventFlag");
         });

    bind(runtime, "ThreadManForUser", 0xA66B0120u, "sceKernelReferEventFlagStatus",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const auto found = g_event_flags.find(static_cast<std::int32_t>(ctx.gpr[4]));
             if (found == g_event_flags.end()) { ctx.set_gpr(2, kErrorUnknownEventFlagId); return; }
             // SceKernelEventFlagInfo: size, name[32], attr, initPattern,
             // currentPattern, numWaitThreads.
             const std::uint32_t out = ctx.gpr[5];
             if (out != 0u && rt.memory().contains(out, 52u)) {
                 rt.memory().store32(out + 44u, found->second.bits);
                 rt.memory().store32(out + 48u, 0u);
             }
             ctx.set_gpr(2, 0u);
         });

    // Waiting for a thread that has already ended returns at once; otherwise the
    // same park-or-report policy applies.
    bind(runtime, "ThreadManForUser", 0x278C0DF5u, "sceKernelWaitThreadEnd",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::int32_t target = static_cast<std::int32_t>(ctx.gpr[4]);
             const auto found = g_table.threads.find(target);
             if (found == g_table.threads.end()) {
                 trace("wait-end: uid=" + std::to_string(target) + " DESCONOCIDO, retorna 0");
                 ctx.set_gpr(2, 0u);
                 return;
             }
             if (found->second.state == ThreadState::Ended) {
                 trace("wait-end: uid=" + std::to_string(target) + " ya terminado, retorna 0");
                 ctx.set_gpr(2, 0u);
                 return;
             }
             if (g_table.ready.empty()) {
                 rt.stop("bloqueo: se espera a un hilo y no hay nada mas que ejecutar");
                 return;
             }
             // Se guarda con el `pc` de retorno normal y el resultado ya puesto:
             // cuando el objetivo termine, este hilo continua como si la llamada
             // hubiera devuelto exito. Nada de reentrar en el stub.
             psprecomp::AllegrexContext blocked = ctx;
             blocked.pc = ctx.gpr[31];
             blocked.set_gpr(2, 0u);
             trace("wait-end: uid=" + std::to_string(g_table.current_uid) + " bloqueado hasta que termine uid=" +
                   std::to_string(target) + " (" + found->second.name + "), reanuda en " +
                   hex32(blocked.pc));
             g_thread_waiters[target].push_back({g_table.current_uid, blocked, g_virtual_time_us});
             (void)activate_next(rt, ctx, "wait-thread-end");
         });

    bind_success(runtime, "ThreadManForUser", 0xEA748E31u, "sceKernelChangeCurrentThreadAttr", 0u);
}

// System services: display, pad, power, clock, UMD.
//
// There is no window and no GPU behind any of this. What it provides is a
// coherent, deterministic *model*: a frame counter that advances, a vblank wait
// that costs one frame of virtual time and yields, a pad that reports nothing
// pressed, a console on mains power with a full battery, and a UMD that is
// present and ready. That is enough for the game's main loop to turn.
//
// Where a value would be a guess it is not invented: `sceImpose`'s two calls
// have no public name beyond their NID, so they report 0 and are marked as such.
constexpr std::uint32_t kMicrosecondsPerFrame = 16'667u; // 59.94 Hz

std::uint32_t g_vcount = 0u;
VblankHook g_vblank_hook;
std::uint32_t g_framebuffer = 0u;
std::uint32_t g_framebuffer_width = 512u;
std::uint32_t g_pixel_format = 3u;
void trace_ge_vblank();

struct SubInterruptRecord {
    std::uint32_t handler{};
    std::uint32_t argument{};
    bool enabled{};
    bool occurred{};
};

std::unordered_map<std::uint64_t, SubInterruptRecord> g_sub_interrupts;
std::unordered_map<std::int32_t, psprecomp::AllegrexContext>
    g_sub_interrupt_returns;
std::uint64_t g_next_sub_interrupt_us{};

std::uint64_t sub_interrupt_key(const std::uint32_t interrupt_number,
                                const std::uint32_t sub_number) {
    return (static_cast<std::uint64_t>(interrupt_number) << 32u) | sub_number;
}

void sub_interrupt_return(psprecomp::Runtime& rt,
                          psprecomp::AllegrexContext& ctx) {
    const auto found = g_sub_interrupt_returns.find(g_table.current_uid);
    if (found == g_sub_interrupt_returns.end()) {
        rt.stop("retorno de sub-interrupcion sin contexto pendiente");
        return;
    }
    ctx = found->second;
    g_sub_interrupt_returns.erase(found);
}

void inject_periodic_sub_interrupt(psprecomp::Runtime& runtime,
                                   Continuation& continuation) {
    constexpr std::uint64_t period_us = 16'667u;
    const auto interrupt = g_sub_interrupts.find(sub_interrupt_key(30u, 0u));
    if (!g_interrupts_enabled || interrupt == g_sub_interrupts.end() ||
        !interrupt->second.enabled || interrupt->second.handler == 0u ||
        g_next_sub_interrupt_us == 0u ||
        g_virtual_time_us < g_next_sub_interrupt_us ||
        !g_sub_interrupt_returns.empty()) {
        return;
    }

    do {
        g_next_sub_interrupt_us += period_us;
    } while (g_next_sub_interrupt_us <= g_virtual_time_us);

    g_sub_interrupt_returns.emplace(continuation.uid, continuation.context);
    continuation.context.set_gpr(4, 0u);
    continuation.context.set_gpr(5, interrupt->second.argument);
    continuation.context.set_gpr(31, 8u);
    continuation.context.pc = interrupt->second.handler;
    interrupt->second.occurred = true;

    // Movie presentation is driven by this hardware interrupt rather than by
    // sceDisplayWaitVblank*. Publish the same frame boundary to the native
    // presenter so its 60 Hz loop can capture PMF frames as well as menu/battle
    // frames.
    ++g_vcount;
    trace_ge_vblank();
    if (g_vblank_hook) g_vblank_hook(runtime, g_virtual_time_us, g_vcount);
}

void trace_ge_vblank();

void install_system_services(psprecomp::Runtime &runtime) {
    // Fate's movie path registers the public PSP VBlank sub-interrupt (30/0)
    // before it starts decoding. Keep the guest handler and run it at the next
    // VBlank; accepting registration without ever delivering the interrupt
    // leaves the original movie thread waiting forever.
    bind(runtime, "InterruptManager", 0xCA04A2B9u,
         "sceKernelRegisterSubIntrHandler",
         [](psprecomp::Runtime&, psprecomp::AllegrexContext& ctx) {
             const std::uint32_t interrupt_number = ctx.gpr[4];
             const std::uint32_t sub_number = ctx.gpr[5];
             const std::uint32_t handler = ctx.gpr[6];
             if (interrupt_number >= 67u || handler == 0u) {
                 ctx.set_gpr(2, 0x80020064u);
                 return;
             }
             const std::uint64_t key = sub_interrupt_key(interrupt_number, sub_number);
             if (g_sub_interrupts.contains(key)) {
                 ctx.set_gpr(2, 0x80020067u);
                 return;
             }
             g_sub_interrupts.emplace(
                 key, SubInterruptRecord{handler, ctx.gpr[7], false, false});
             if (std::getenv("PSPRECOMP_INTR_DIAG") != nullptr) {
                 std::printf("[intr-fate] register int=%u sub=%u handler=%s arg=%s\n",
                             interrupt_number, sub_number, hex32(handler).c_str(),
                             hex32(ctx.gpr[7]).c_str());
             }
             ctx.set_gpr(2, 0u);
         });
    bind(runtime, "InterruptManager", 0xD61E6961u,
         "sceKernelReleaseSubIntrHandler",
         [](psprecomp::Runtime&, psprecomp::AllegrexContext& ctx) {
             ctx.set_gpr(2, g_sub_interrupts.erase(
                                    sub_interrupt_key(ctx.gpr[4], ctx.gpr[5])) == 1u
                                ? 0u
                                : 0x80020068u);
         });
    bind(runtime, "InterruptManager", 0xFB8E22ECu, "sceKernelEnableSubIntr",
         [](psprecomp::Runtime&, psprecomp::AllegrexContext& ctx) {
             const auto found = g_sub_interrupts.find(
                 sub_interrupt_key(ctx.gpr[4], ctx.gpr[5]));
             if (found == g_sub_interrupts.end()) {
                 ctx.set_gpr(2, 0x80020068u);
                 return;
             }
             found->second.enabled = true;
             g_next_sub_interrupt_us = g_virtual_time_us + kMicrosecondsPerFrame;
             ctx.set_gpr(2, 0u);
         });

    // --- display -----------------------------------------------------------
    bind_success(runtime, "sceDisplay", 0x0E20F177u, "sceDisplaySetMode", 0u);

    bind(runtime, "sceDisplay", 0x289D82FEu, "sceDisplaySetFrameBuf",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             g_framebuffer = ctx.gpr[4];
             g_framebuffer_width = ctx.gpr[5];
             g_pixel_format = ctx.gpr[6];
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceDisplay", 0xEEDA2E54u, "sceDisplayGetFrameBuf",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t out[3] = {g_framebuffer, g_framebuffer_width, g_pixel_format};
             for (std::uint32_t index = 0; index < 3u; ++index) {
                 const std::uint32_t pointer = ctx.gpr[4 + index];
                 if (pointer != 0u && rt.memory().contains(pointer, 4u)) {
                     rt.memory().store32(pointer, out[index]);
                 }
             }
             ctx.set_gpr(2, 0u);
         });

    // The one that makes the main loop turn. A vblank wait costs a frame of
    // virtual time and gives up the CPU, exactly like a delay.
    bind(runtime, "sceDisplay", 0x46F186C3u, "sceDisplayWaitVblankStartCB",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             ++g_vcount;
             trace_ge_vblank();
             if (g_vblank_hook) g_vblank_hook(rt, g_virtual_time_us, g_vcount);
             psprecomp::AllegrexContext sleeping = ctx;
             sleeping.set_gpr(2, 0u);
             sleeping.pc = ctx.gpr[31];
             g_table.ready.push_back({g_table.current_uid, sleeping,
                                      g_virtual_time_us + kMicrosecondsPerFrame});
             (void)activate_next(rt, ctx, "vblank");
         });

    bind(runtime, "sceDisplay", 0x9C6EAAD7u, "sceDisplayGetVcount",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, g_vcount); });
    bind_success(runtime, "sceDisplay", 0x773DD3A3u, "sceDisplayGetCurrentHcount", 0u);
    // Returns a float in $f0, so v0 is not the answer; 0 there is correct.
    bind(runtime, "sceDisplay", 0xDBA6C4C4u, "sceDisplayGetFramePerSec",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             ctx.fpr[0] = 59.94f;
             ctx.set_gpr(2, 0u);
         });

    // --- pad ---------------------------------------------------------------
    bind_success(runtime, "sceCtrl", 0x1F4011E6u, "sceCtrlSetSamplingMode", 0u);
    bind_success(runtime, "sceCtrl", 0x6A2774F3u, "sceCtrlSetSamplingCycle", 0u);
    // SceCtrlData is 16 bytes: timestamp, buttons, analogue x and y, padding.
    // Nothing is pressed and both sticks are centred at 128.
    for (const auto &entry : {std::pair<std::uint32_t, const char *>{0x1F803938u, "sceCtrlReadBufferPositive"},
                              std::pair<std::uint32_t, const char *>{0x3A622550u, "sceCtrlPeekBufferPositive"}}) {
        bind(runtime, "sceCtrl", entry.first, entry.second,
             [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                 const std::uint32_t out = ctx.gpr[4];
                 const std::uint32_t count = std::max(1u, ctx.gpr[5]);
                 if (out != 0u && rt.memory().contains(out, count * 16u)) {
                     for (std::uint32_t index = 0; index < count; ++index) {
                         const std::uint32_t base = out + index * 16u;
                         rt.memory().store32(base, static_cast<std::uint32_t>(g_virtual_time_us));
                         rt.memory().store32(base + 4u, g_pad_buttons);
                         rt.memory().store8(base + 8u, g_pad_analog_x);
                         rt.memory().store8(base + 9u, g_pad_analog_y);
                         rt.memory().zero(base + 10u, 6u);
                     }
                 }
                 ctx.set_gpr(2, count);
             });
    }

    // --- power and suspend --------------------------------------------------
    bind_success(runtime, "scePower", 0x04B7766Eu, "scePowerRegisterCallback", 0u);
    bind_success(runtime, "scePower", 0x0AFD0D8Bu, "scePowerIsBatteryExist", 1u);
    bind_success(runtime, "scePower", 0x2085D15Du, "scePowerGetBatteryLifePercent", 100u);
    bind_success(runtime, "scePower", 0x87440F5Eu, "scePowerIsPowerOnline", 1u);
    bind_success(runtime, "scePower", 0xD3075926u, "scePowerIsLowBattery", 0u);
    bind_success(runtime, "scePower", 0xBD681969u, "scePowerGetBusClockFrequencyInt", 111u);
    bind_success(runtime, "scePower", 0xFDB5BFE9u, "scePowerGetCpuClockFrequencyInt", 222u);
    bind_success(runtime, "scePower", 0xEBD177D6u, "scePowerSetClockFrequency", 0u);
    // No public name beyond the NID; reports success and nothing else.
    bind_success(runtime, "scePower", 0x34F9C463u, "scePower_34F9C463", 0u);

    bind_success(runtime, "sceSuspendForUser", 0x090CCB3Fu, "sceKernelPowerTick", 0u);
    bind_success(runtime, "sceSuspendForUser", 0x3AEE7261u, "sceKernelPowerUnlock", 0u);
    bind_success(runtime, "sceSuspendForUser", 0xEADB1BD7u, "sceKernelPowerLock", 0u);

    // --- clock --------------------------------------------------------------
    // sceRtcGetCurrentTick writes a 64-bit microsecond tick.
    bind(runtime, "sceRtc", 0x3F7AD767u, "sceRtcGetCurrentTick",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t out = ctx.gpr[4];
             if (out != 0u && rt.memory().contains(out, 8u)) {
                 rt.memory().store32(out, static_cast<std::uint32_t>(g_virtual_time_us));
                 rt.memory().store32(out + 4u, static_cast<std::uint32_t>(g_virtual_time_us >> 32u));
             }
             ctx.set_gpr(2, 0u);
         });

    // --- UMD and wireless ---------------------------------------------------
    // The disc is present and the drive is ready. 0x02 is "medium inserted" and
    // 0x12 is "present and ready", which is what a game polls for.
    bind_success(runtime, "sceUmdUser", 0x46EBB729u, "sceUmdCheckMedium", 1u);
    // PSP_UMD_PRESENT (0x02) | PSP_UMD_INITED (0x10) | PSP_UMD_READY (0x20).
    //
    // READY was missing, and the game asks for exactly that bit: every FPACK
    // read is preceded by `sceUmdGetDriveStat` followed by
    // `sceUmdWaitDriveStatCB(0x20)`. Reporting 0x12 said "the disc is in and
    // initialised but not ready to read", so the game opened, seeked, read its
    // entry header, threw it away and started over -- 9038 times against the
    // same sector in one run.
    bind_success(runtime, "sceUmdUser", 0x6B4A146Cu, "sceUmdGetDriveStat", 0x32u);
    bind_success(runtime, "sceUmdUser", 0x8EF08FCEu, "sceUmdWaitDriveStat", 0u);
    bind_success(runtime, "sceUmdUser", 0x4A9E5E29u, "sceUmdWaitDriveStatCB", 0u);
    bind_success(runtime, "sceUmdUser", 0x6AF9B50Au, "sceUmdCancelWaitDriveStat", 0u);
    bind_success(runtime, "sceUmdUser", 0xAEE7404Du, "sceUmdRegisterUMDCallBack", 0u);
    bind_success(runtime, "sceUmdUser", 0xC6183D47u, "sceUmdActivate", 0u);

    // The wireless switch is off. Reporting it on would invite the game into a
    // networking path nothing here implements.
    bind_success(runtime, "sceWlanDrv", 0xD7763699u, "sceWlanGetSwitchState", 0u);

    // Two `sceImpose` calls with no public name beyond their NID. They report 0,
    // and that is recorded as "unknown", not as "does nothing".
    bind_success(runtime, "sceImpose", 0x24FD7BCFu, "sceImpose_24FD7BCF", 0u);
    bind_success(runtime, "sceImpose", 0x36AA6E91u, "sceImpose_36AA6E91", 0u);
}

// Graphics engine compatibility boundary. The HLE still completes lists
// immediately so the guest can run, but an opt-in bounded trace records the
// real PSP command stream that the new native backend must implement. The trace
// never follows guest pointers and never interprets gameplay.
std::int32_t g_next_display_list_id = 0x100;
std::uint32_t g_ge_finish_func = 0u;
std::uint32_t g_ge_finish_arg = 0u;
std::uint32_t g_ge_callback_stack = 0u;
GeDrawHook g_ge_draw_hook;
std::ofstream g_ge_trace;
std::uint64_t g_ge_trace_sequence = 0u;
std::uint32_t g_ge_trace_events_after_first = 0u;
std::uint32_t g_ge_trace_enqueue_count = 0u;
bool g_ge_first_enqueue_seen = false;
bool g_ge_first_textured_draw_seen = false;

constexpr std::uint32_t kGeTraceWordLimit = 32u;
constexpr std::uint32_t kGeTraceScanWordLimit = 4096u;
constexpr std::uint32_t kGeTraceEnqueueLimit = 32u;
constexpr std::uint32_t kGeTraceEventLimit = 256u;

struct GeTraceList {
    std::int32_t id{-1};
    std::uint32_t list{};
    std::uint32_t stall{};
};

struct GeRuntimeList {
    std::uint32_t list{};
    std::uint32_t stall{};
    std::uint64_t replay_vcount{};
};

std::array<GeTraceList, kGeTraceEnqueueLimit> g_ge_trace_lists{};
fuc::renderer::psp::GeCommandState g_ge_trace_command_state{};
std::map<std::int32_t, GeRuntimeList> g_ge_runtime_lists{};
fuc::renderer::psp::GeListConsumer g_ge_consumer{};

struct GeFlowCounters {
    std::uint64_t jumps{};
    std::uint64_t bjumps{};
    std::uint64_t calls{};
    std::uint64_t returns{};
    std::uint64_t ends{};
    std::uint64_t ends_inside_call{};
    std::uint64_t execution_caps{};
    std::uint64_t execution_loops{};
    std::uint64_t invalid_addresses{};
    std::uint64_t returns_without_call{};
    std::size_t max_call_depth{};
    std::map<std::uint32_t, std::uint64_t> call_targets;
    std::map<std::uint32_t, std::uint64_t> execution_cap_starts;
    std::map<std::uint8_t, std::uint64_t> signal_behaviors;
};
GeFlowCounters g_ge_flow{};

constexpr std::uint32_t canonical_ge_address(const std::uint32_t address) {
    return address & 0x1FFFFFFFu;
}

void open_ge_trace_from_environment() {
    g_ge_trace.close();
    g_ge_trace.clear();
    g_ge_trace_sequence = 0u;
    g_ge_trace_events_after_first = 0u;
    g_ge_trace_enqueue_count = 0u;
    g_ge_first_enqueue_seen = false;
    g_ge_first_textured_draw_seen = false;
    g_ge_trace_lists = {};
    g_ge_trace_command_state = {};
    const char *raw_path = std::getenv("PSPRECOMP_GE_TRACE");
    if (raw_path == nullptr || *raw_path == '\0') return;

    const std::filesystem::path path(raw_path);
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    g_ge_trace.open(path, std::ios::trunc);
    if (!g_ge_trace) throw psprecomp::Error("no se pudo abrir PSPRECOMP_GE_TRACE");
}

void trace_ge_simple(const char *event, const std::int32_t list_id,
                     const std::uint32_t address) {
    if (!g_ge_trace || !g_ge_first_enqueue_seen ||
        g_ge_trace_events_after_first >= kGeTraceEventLimit) return;
    ++g_ge_trace_events_after_first;
    g_ge_trace << "{\"sequence\":" << ++g_ge_trace_sequence
               << ",\"event\":\"" << event << "\",\"guestUs\":"
               << g_virtual_time_us << ",\"vcount\":" << g_vcount
               << ",\"listId\":" << list_id << ",\"address\":\""
               << hex32(address) << "\",\"canonicalAddress\":\""
               << hex32(canonical_ge_address(address)) << "\"}\n";
    g_ge_trace.flush();
}

void trace_ge_vblank() { trace_ge_simple("vblank", -1, 0u); }

void trace_ge_enqueue(psprecomp::Runtime &runtime, const std::int32_t list_id,
                      const std::uint32_t list, const std::uint32_t stall,
                      const std::uint32_t callback_id,
                      const std::uint32_t argument) {
    if (!g_ge_trace || g_ge_trace_enqueue_count >= kGeTraceEnqueueLimit) return;
    const std::uint32_t enqueue_index = g_ge_trace_enqueue_count++;
    g_ge_first_enqueue_seen = true;
    g_ge_trace_lists[enqueue_index] = {list_id, list, stall};
    const bool list_valid = (list & 3u) == 0u && runtime.memory().contains(list, 4u);
    const bool stall_valid = stall == 0u ||
                             ((stall & 3u) == 0u && runtime.memory().contains(stall, 4u));
    const bool argument_valid = argument == 0u || runtime.memory().contains(argument, 4u);

    std::uint32_t scan_limit = kGeTraceScanWordLimit;
    const std::uint32_t canonical_list = canonical_ge_address(list);
    const std::uint32_t canonical_stall = canonical_ge_address(stall);
    if (stall != 0u && canonical_stall >= canonical_list &&
        ((canonical_stall - canonical_list) & 3u) == 0u) {
        scan_limit = std::min(scan_limit, (canonical_stall - canonical_list) / 4u);
    }

    std::uint32_t scanned_words = 0u;
    std::uint32_t prim_count = 0u;
    std::uint32_t first_prim_address = 0u;
    std::uint32_t first_prim_word = 0u;
    bool saw_end = false;
    bool hit_invalid_memory = false;
    if (list_valid) {
        for (; scanned_words < scan_limit; ++scanned_words) {
            const std::uint64_t address64 = static_cast<std::uint64_t>(list) +
                                            scanned_words * 4ull;
            if (address64 > 0xFFFFFFFFull) {
                hit_invalid_memory = true;
                break;
            }
            const auto address = static_cast<std::uint32_t>(address64);
            if (!runtime.memory().contains(address, 4u)) {
                hit_invalid_memory = true;
                break;
            }
            const std::uint32_t word = runtime.memory().load32(address);
            const std::uint32_t opcode = word >> 24u;
            g_ge_trace_command_state.apply(fuc::renderer::psp::decode_command(word));
            if (opcode == 0x04u) {
                if (prim_count == 0u) {
                    first_prim_address = address;
                    first_prim_word = word;
                }
                ++prim_count;
            }
            if (opcode == 0x0Cu) {
                ++scanned_words;
                saw_end = true;
                break;
            }
        }
    }

    g_ge_trace << "{\"sequence\":" << ++g_ge_trace_sequence
               << ",\"event\":\"enqueue\",\"guestUs\":" << g_virtual_time_us
               << ",\"vcount\":" << g_vcount << ",\"enqueueIndex\":"
               << enqueue_index << ",\"listId\":" << list_id
               << ",\"list\":\"" << hex32(list) << "\",\"canonicalList\":\""
               << hex32(canonical_list) << "\",\"stall\":\"" << hex32(stall)
               << "\",\"canonicalStall\":\"" << hex32(canonical_stall)
               << "\",\"callbackId\":" << callback_id
               << ",\"argument\":\"" << hex32(argument)
               << "\",\"listValid\":" << (list_valid ? "true" : "false")
               << ",\"stallValid\":" << (stall_valid ? "true" : "false")
               << ",\"argumentValid\":" << (argument_valid ? "true" : "false")
               << ",\"scanLimitWords\":" << scan_limit
               << ",\"scannedWords\":" << scanned_words
               << ",\"primCount\":" << prim_count
               << ",\"firstPrimAddress\":\"" << hex32(first_prim_address)
               << "\",\"firstPrimWord\":\"" << hex32(first_prim_word)
               << "\",\"sawEnd\":" << (saw_end ? "true" : "false")
               << ",\"hitInvalidMemory\":"
               << (hit_invalid_memory ? "true" : "false")
               << ",\"words\":[";

    bool first_word = true;
    if (list_valid && enqueue_index == 0u) {
        for (std::uint32_t index = 0u; index < kGeTraceWordLimit; ++index) {
            const std::uint64_t address64 = static_cast<std::uint64_t>(list) + index * 4ull;
            if (address64 > 0xFFFFFFFFull) break;
            const auto address = static_cast<std::uint32_t>(address64);
            if (stall != 0u && address >= stall) break;
            if (!runtime.memory().contains(address, 4u)) break;
            const std::uint32_t word = runtime.memory().load32(address);
            if (!first_word) g_ge_trace << ',';
            first_word = false;
            g_ge_trace << "{\"index\":" << index << ",\"address\":\""
                       << hex32(address) << "\",\"word\":\"" << hex32(word)
                       << "\",\"opcode\":" << (word >> 24u)
                       << ",\"payload\":" << (word & 0x00FFFFFFu) << '}';
        }
    }
    g_ge_trace << "]}\n";
    g_ge_trace.flush();
}

void trace_ge_stall_update(psprecomp::Runtime &runtime, const std::int32_t list_id,
                           const std::uint32_t new_stall) {
    if (!g_ge_trace || g_ge_trace_events_after_first >= kGeTraceEventLimit) return;

    GeTraceList *tracked = nullptr;
    for (std::uint32_t index = 0u; index < g_ge_trace_enqueue_count; ++index) {
        if (g_ge_trace_lists[index].id == list_id) {
            tracked = &g_ge_trace_lists[index];
            break;
        }
    }
    if (tracked == nullptr || tracked->stall == new_stall) return;

    const std::uint32_t old_stall = tracked->stall;
    const std::uint32_t scan_start = old_stall == 0u ? tracked->list : old_stall;
    tracked->stall = new_stall;
    const std::uint32_t canonical_start = canonical_ge_address(scan_start);
    const std::uint32_t canonical_end = canonical_ge_address(new_stall);
    const bool aligned_range = new_stall != 0u && canonical_end >= canonical_start &&
                               ((canonical_end - canonical_start) & 3u) == 0u;
    const std::uint32_t available_words = aligned_range
        ? (canonical_end - canonical_start) / 4u
        : 0u;
    const std::uint32_t scan_limit = std::min(available_words, kGeTraceScanWordLimit);

    std::uint32_t scanned_words = 0u;
    std::uint32_t prim_count = 0u;
    std::uint32_t first_prim_address = 0u;
    std::uint32_t first_prim_word = 0u;
    std::uint32_t first_vaddr_word = 0u;
    std::uint32_t first_iaddr_word = 0u;
    std::uint32_t last_vertex_type_word = 0u;
    fuc::renderer::psp::PrimitiveDraw first_draw{};
    bool captured_first_draw = false;
    bool hit_invalid_memory = false;
    for (; scanned_words < scan_limit; ++scanned_words) {
        const std::uint64_t address64 = static_cast<std::uint64_t>(scan_start) +
                                        scanned_words * 4ull;
        if (address64 > 0xFFFFFFFFull) {
            hit_invalid_memory = true;
            break;
        }
        const auto address = static_cast<std::uint32_t>(address64);
        if (!runtime.memory().contains(address, 4u)) {
            hit_invalid_memory = true;
            break;
        }
        const std::uint32_t word = runtime.memory().load32(address);
        const std::uint32_t opcode = word >> 24u;
        fuc::renderer::psp::PrimitiveDraw draw{};
        const bool emitted_draw = g_ge_trace_command_state.apply(
            fuc::renderer::psp::decode_command(word), &draw);
        if (emitted_draw && !captured_first_draw) {
            first_draw = draw;
            captured_first_draw = true;
        }
        if (opcode == static_cast<std::uint32_t>(
                          fuc::renderer::psp::GeOpcode::jump)) {
            const std::uint32_t target = g_ge_trace_command_state.relative_address(
                word & 0x00FFFFFCu);
            if (target > canonical_start && target < canonical_end &&
                ((target - canonical_start) & 3u) == 0u) {
                scanned_words = (target - canonical_start) / 4u - 1u;
                continue;
            }
        }
        if (opcode == 0x01u && first_vaddr_word == 0u) first_vaddr_word = word;
        if (opcode == 0x02u && first_iaddr_word == 0u) first_iaddr_word = word;
        if (opcode == 0x12u) last_vertex_type_word = word;
        if (opcode == 0x04u) {
            if (prim_count == 0u) {
                first_prim_address = address;
                first_prim_word = word;
            }
            ++prim_count;
        }
    }

    ++g_ge_trace_events_after_first;
    g_ge_trace << "{\"sequence\":" << ++g_ge_trace_sequence
               << ",\"event\":\"update_stall\",\"guestUs\":" << g_virtual_time_us
               << ",\"vcount\":" << g_vcount << ",\"listId\":" << list_id
               << ",\"oldStall\":\"" << hex32(old_stall)
               << "\",\"newStall\":\"" << hex32(new_stall)
               << "\",\"canonicalStart\":\"" << hex32(canonical_start)
               << "\",\"canonicalEnd\":\"" << hex32(canonical_end)
               << "\",\"alignedRange\":" << (aligned_range ? "true" : "false")
               << ",\"availableWords\":" << available_words
               << ",\"scannedWords\":" << scanned_words
               << ",\"primCount\":" << prim_count
               << ",\"firstPrimAddress\":\"" << hex32(first_prim_address)
               << "\",\"firstPrimWord\":\"" << hex32(first_prim_word)
               << "\",\"firstVaddrWord\":\"" << hex32(first_vaddr_word)
               << "\",\"firstIaddrWord\":\"" << hex32(first_iaddr_word)
               << "\",\"lastVertexTypeWord\":\"" << hex32(last_vertex_type_word)
               << "\",\"effectiveVertexAddress\":\""
               << hex32(first_draw.vertex_address)
               << "\",\"effectiveIndexAddress\":\""
               << hex32(first_draw.index_address)
               << "\",\"effectiveVertexType\":\""
               << hex32(first_draw.vertex_type)
               << "\",\"textureEnabled\":"
               << (first_draw.texture_enabled ? "true" : "false")
               << ",\"textureAddress\":\"" << hex32(first_draw.texture_address)
               << "\",\"textureWidth\":" << first_draw.texture_width
               << ",\"textureHeight\":" << first_draw.texture_height
               << ",\"textureFormat\":"
               << static_cast<std::uint32_t>(first_draw.texture_format)
               << ",\"hitInvalidMemory\":"
               << (hit_invalid_memory ? "true" : "false")
               << ",\"commandWords\":[";
    if (prim_count != 0u) {
        const std::uint32_t command_word_count = std::min(scan_limit, 64u);
        for (std::uint32_t index = 0u; index < command_word_count; ++index) {
            if (index != 0u) g_ge_trace << ',';
            const std::uint32_t address = scan_start + index * 4u;
            g_ge_trace << "\"" << hex32(runtime.memory().load32(address)) << "\"";
        }
    }
    g_ge_trace << "],\"vertexBytes\":[";
    if (captured_first_draw && runtime.memory().contains(first_draw.vertex_address, 32u)) {
        for (std::uint32_t index = 0u; index < 32u; ++index) {
            if (index != 0u) g_ge_trace << ',';
            g_ge_trace << static_cast<std::uint32_t>(
                runtime.memory().load8(first_draw.vertex_address + index));
        }
    }
    g_ge_trace << "],\"decodedVertices\":[";
    if (captured_first_draw) {
        const auto type = fuc::renderer::psp::decode_vertex_type(
            fuc::renderer::psp::decode_command(0x12000000u |
                                               first_draw.vertex_type));
        const auto layout = fuc::renderer::psp::vertex_layout(type);
        const std::uint32_t vertex_count =
            std::min<std::uint32_t>(first_draw.vertex_count, 8u);
        bool first_vertex = true;
        for (std::uint32_t index = 0u; index < vertex_count; ++index) {
            const std::uint32_t address =
                first_draw.vertex_address + index * layout.stride;
            if (layout.stride == 0u ||
                !runtime.memory().contains(address, layout.stride)) break;
            std::vector<std::uint8_t> bytes(layout.stride);
            for (std::uint32_t byte = 0u; byte < layout.stride; ++byte) {
                bytes[byte] = runtime.memory().load8(address + byte);
            }
            fuc::renderer::psp::ThroughVertex vertex{};
            if (!fuc::renderer::psp::decode_through_vertex(type, bytes, vertex)) break;
            if (!first_vertex) g_ge_trace << ',';
            first_vertex = false;
            g_ge_trace << "{\"address\":\"" << hex32(address)
                       << "\",\"color\":\"" << hex32(vertex.color)
                       << "\",\"u\":" << vertex.u << ",\"v\":" << vertex.v
                       << ",\"hasTextureCoordinates\":"
                       << (vertex.has_texture_coordinates ? "true" : "false")
                       << ",\"x\":" << vertex.x << ",\"y\":" << vertex.y
                       << ",\"z\":" << vertex.z << '}';
        }
    }
    g_ge_trace << "]}\n";
    g_ge_trace.flush();
}

void consume_ge_range(psprecomp::Runtime &runtime, const std::uint32_t raw_start,
                      const std::uint32_t raw_end) {
    const std::uint32_t start = canonical_ge_address(raw_start);
    const std::uint32_t end = canonical_ge_address(raw_end);
    if (end < start || ((end - start) & 3u) != 0u) return;
    if (start == end) return;

    // CALL/RET are fundamental PSP GE commands: Fate keeps its character and
    // stage packets in static sublists and calls them from the small command
    // buffer grown through stall updates.  Flattening only the producer range
    // executes those sublists once at best, after which later color clears
    // erase them.  Walk the guest list with a bounded GE return stack so every
    // frame replays the original PSP packets exactly where the game calls them.
    // The combat list is larger than the 4096-word diagnostic scan cap: its
    // first half contains Saber and the second half Archer. Keep tracing
    // compact, but let the actual PSP GE walker finish the bounded live list.
    // Top-level jumps may not cross the stall boundary and backward loops are
    // stopped below, so this is a safety ceiling rather than a blind scan.
    constexpr std::uint32_t kGeExecutionWordLimit = 65536u;
    std::vector<std::uint32_t> return_stack;
    return_stack.reserve(32u);
    std::vector<std::uint32_t> list_stack;
    list_stack.reserve(32u);
    std::vector<fuc::renderer::psp::PrimitiveDraw> draws;
    std::uint32_t pc = start;
    std::uint32_t current_list = start;
    std::uint32_t executed = 0u;
    bool stopped = false;
    for (; executed < kGeExecutionWordLimit; ++executed) {
        if (!runtime.memory().contains(pc, 4u)) {
            ++g_ge_flow.invalid_addresses;
            break;
        }
        const std::uint32_t word = runtime.memory().load32(pc);
        const auto command = fuc::renderer::psp::decode_command(word);
        fuc::renderer::psp::PrimitiveDraw draw{};
        if (g_ge_consumer.consume_command(word, &draw)) {
            draw.command_address = pc;
            draw.list_address = current_list;
            draws.push_back(draw);
        }
        if (command.opcode == fuc::renderer::psp::GeOpcode::signal)
            ++g_ge_flow.signal_behaviors[static_cast<std::uint8_t>(
                (command.payload >> 16u) & 0xFFu)];

        if (command.opcode == fuc::renderer::psp::GeOpcode::end) {
            ++g_ge_flow.ends;
            if (!return_stack.empty()) ++g_ge_flow.ends_inside_call;
            stopped = true;
            break;
        }
        if (command.opcode == fuc::renderer::psp::GeOpcode::call) {
            ++g_ge_flow.calls;
            ++g_ge_flow.call_targets[canonical_ge_address(
                g_ge_consumer.state().relative_address(command.payload))];
            if (return_stack.size() >= 32u) break;
            return_stack.push_back(pc + 4u);
            list_stack.push_back(current_list);
            g_ge_flow.max_call_depth = std::max(
                g_ge_flow.max_call_depth, return_stack.size());
            pc = canonical_ge_address(
                g_ge_consumer.state().relative_address(command.payload));
            current_list = pc;
            continue;
        }
        if (command.opcode == fuc::renderer::psp::GeOpcode::ret) {
            ++g_ge_flow.returns;
            if (return_stack.empty()) {
                ++g_ge_flow.returns_without_call;
                break;
            }
            pc = return_stack.back();
            return_stack.pop_back();
            current_list = list_stack.back();
            list_stack.pop_back();
            continue;
        }
        if (command.opcode == fuc::renderer::psp::GeOpcode::jump) {
            ++g_ge_flow.jumps;
            const std::uint32_t target = canonical_ge_address(
                g_ge_consumer.state().relative_address(command.payload));
            // At top level the stall address is the GPU's hard visibility
            // boundary. A JUMP beyond it is not executable yet; the next stall
            // update will expose and consume that range. CALL targets are
            // different: they are complete static sublists and are handled
            // above with a return stack.
            if (return_stack.empty() && (target < start || target >= end)) {
                stopped = true;
                break;
            }
            // A backward/self JUMP without a changing condition is a GE-side
            // wait loop. The synchronous host cannot make it progress while
            // executing the same range, so stop at the loop boundary instead
            // of burning the complete word cap.
            if (target <= pc) {
                ++g_ge_flow.execution_loops;
                stopped = true;
                break;
            }
            pc = target;
            continue;
        }
        if (command.opcode == fuc::renderer::psp::GeOpcode::bjump)
            ++g_ge_flow.bjumps;
        pc += 4u;
        if (return_stack.empty() && pc >= end) break;
    }
    if (!stopped && executed == kGeExecutionWordLimit) {
        ++g_ge_flow.execution_caps;
        ++g_ge_flow.execution_cap_starts[start];
    }

    for (const auto &draw : draws) {
        if (g_ge_trace && !g_ge_first_textured_draw_seen &&
            draw.texture_enabled && draw.texture_address != 0u) {
            g_ge_first_textured_draw_seen = true;
            g_ge_trace << "{\"sequence\":" << ++g_ge_trace_sequence
                       << ",\"event\":\"first_textured_draw\",\"guestUs\":"
                       << g_virtual_time_us << ",\"vcount\":" << g_vcount
                       << ",\"segmentStart\":\"" << hex32(start)
                       << "\",\"primitiveType\":"
                       << static_cast<std::uint32_t>(draw.type)
                       << ",\"vertexCount\":" << draw.vertex_count
                       << ",\"vertexAddress\":\"" << hex32(draw.vertex_address)
                       << "\",\"indexAddress\":\"" << hex32(draw.index_address)
                       << "\",\"vertexType\":\"" << hex32(draw.vertex_type)
                       << "\",\"textureAddress\":\"" << hex32(draw.texture_address)
                       << "\",\"textureBufferWidth\":" << draw.texture_buffer_width
                       << ",\"textureWidth\":" << draw.texture_width
                       << ",\"textureHeight\":" << draw.texture_height
                       << ",\"textureFormat\":"
                       << static_cast<std::uint32_t>(draw.texture_format)
                       << ",\"textureSwizzled\":"
                       << (draw.texture_swizzled ? "true" : "false")
                       << ",\"clutAddress\":\"" << hex32(draw.clut_address)
                       << "\",\"clutFormat\":"
                       << static_cast<std::uint32_t>(draw.clut_format)
                       << ",\"clutShift\":"
                       << static_cast<std::uint32_t>(draw.clut_shift)
                       << ",\"clutMask\":"
                       << static_cast<std::uint32_t>(draw.clut_mask)
                       << ",\"clutStart\":" << draw.clut_start
                       << ",\"vertexBytes\":[";
            for (std::uint32_t index = 0u; index < 64u; ++index) {
                if (!runtime.memory().contains(draw.vertex_address + index, 1u)) break;
                if (index != 0u) g_ge_trace << ',';
                g_ge_trace << static_cast<std::uint32_t>(
                    runtime.memory().load8(draw.vertex_address + index));
            }
            g_ge_trace << "],\"textureBytes\":[";
            for (std::uint32_t index = 0u; index < 512u; ++index) {
                if (!runtime.memory().contains(draw.texture_address + index, 1u)) break;
                if (index != 0u) g_ge_trace << ',';
                g_ge_trace << static_cast<std::uint32_t>(
                    runtime.memory().load8(draw.texture_address + index));
            }
            g_ge_trace << "],\"clutBytes\":[";
            for (std::uint32_t index = 0u; index < 64u; ++index) {
                if (!runtime.memory().contains(draw.clut_address + index, 1u)) break;
                if (index != 0u) g_ge_trace << ',';
                g_ge_trace << static_cast<std::uint32_t>(
                    runtime.memory().load8(draw.clut_address + index));
            }
            g_ge_trace << "]}\n";
            g_ge_trace.flush();
        }
        if (g_ge_draw_hook) g_ge_draw_hook(runtime, draw);
    }
}

void consume_ge_enqueue(psprecomp::Runtime &runtime, const std::int32_t list_id,
                        const std::uint32_t list, const std::uint32_t stall) {
    g_ge_runtime_lists[list_id] = {list, stall, g_vcount};
    if (stall != 0u) {
        consume_ge_range(runtime, list, stall);
        return;
    }

    // A non-stalled list is bounded to the same 4096-word research limit. END
    // stops the consumer; the cap prevents a corrupt guest pointer from turning
    // a platform callback into an unbounded memory walk.
    const std::uint32_t canonical_list = canonical_ge_address(list);
    consume_ge_range(runtime, list,
                     canonical_list + kGeTraceScanWordLimit * 4u);
}

void consume_ge_stall_update(psprecomp::Runtime &runtime,
                             const std::int32_t list_id,
                             const std::uint32_t new_stall) {
    const auto found = g_ge_runtime_lists.find(list_id);
    if (found == g_ge_runtime_lists.end()) return;
    GeRuntimeList &tracked = found->second;
    const std::uint32_t start = tracked.stall == 0u ? tracked.list : tracked.stall;
    tracked.stall = new_stall;
    if (new_stall == 0u) return;

    // Fate reuses its GE command buffer.  When the producer wraps, the new
    // stall is below (or can be equal to) the previous one because the words
    // from LIST onward were rewritten for the next frame.  Treating every
    // update as a monotonically growing range silently dropped that complete
    // frame -- notably the actor/stage lists -- after their first submission.
    // Reconsume the rewritten prefix on wrap/reuse; the ordinary forward case
    // still consumes only newly exposed words.
    const std::uint32_t canonical_start = canonical_ge_address(start);
    const std::uint32_t canonical_end = canonical_ge_address(new_stall);
    if (canonical_end < canonical_start) {
        consume_ge_range(runtime, tracked.list, new_stall);
        tracked.replay_vcount = g_vcount;
    } else if (canonical_end == canonical_start) {
        // The game updates the same stall address many times while waiting.
        // The PSP GE does not restart the list for every identical write.
        // Replaying once per VBlank preserves command-buffer reuse without
        // multiplying static CALL sublists hundreds of thousands of times.
        if (tracked.replay_vcount != g_vcount) {
            consume_ge_range(runtime, tracked.list, new_stall);
            tracked.replay_vcount = g_vcount;
        }
    } else {
        consume_ge_range(runtime, start, new_stall);
    }
}

// How a guest callback gets run: as a queued continuation on its own small
// stack, not by re-entering the recompiled code from inside the HLE handler.
//
// The two options were a synchronous `Runtime::invoke_isolated_aot` and this.
// This one was chosen because the profile already has a scheduler with priority
// and wake times, so a callback is just another runnable thing, and because
// re-entering guest code from inside a handler would put a host frame in the
// middle of a guest call chain, which the thread switching then has to unwind
// around. It is a design decision and it is revisable; it lives here rather than
// being spread across call sites so that changing it is a local edit.
//
// The callback runs at priority 0 so it preempts anything waiting on it, and
// returns to the trampoline at address 0, which ends it like any thread.
void schedule_ge_finish_callback(psprecomp::Runtime &runtime, std::int32_t list_id) {
    if (g_ge_finish_func == 0u) return;
    if (g_ge_callback_stack == 0u) {
        std::uint32_t bottom = 0u;
        std::uint32_t top = 0u;
        if (!allocate_stack(16u * 1024u, bottom, top)) return;
        g_ge_callback_stack = top;
    }

    const std::int32_t uid = g_table.next_uid++;
    ThreadRecord record;
    record.name = "ge_finish_callback";
    record.entry = g_ge_finish_func;
    record.initial_priority = 0u;
    record.priority = 0u; // above everything, so the waiter is released at once
    record.stack_bottom = g_ge_callback_stack - 16u * 1024u;
    record.stack_top = g_ge_callback_stack;
    record.stack_size = 16u * 1024u;
    record.state = ThreadState::Ready;
    g_table.threads.emplace(uid, record);

    const std::uint32_t kernel_context = record.stack_top - 0x100u;
    runtime.memory().zero(record.stack_bottom, record.stack_size);
    runtime.memory().store32(kernel_context + 0xC0u, static_cast<std::uint32_t>(uid));

    psprecomp::AllegrexContext callback{};
    callback.set_gpr(4, static_cast<std::uint32_t>(list_id)); // a0: list id
    callback.set_gpr(5, g_ge_finish_arg);                     // a1: user argument
    callback.set_gpr(26, kernel_context);
    callback.set_gpr(28, g_gp);
    callback.set_gpr(29, kernel_context - 64u);
    callback.set_gpr(30, kernel_context - 64u);
    callback.set_gpr(31, 0u);
    callback.pc = g_ge_finish_func;
    g_table.ready.push_back({uid, callback, g_virtual_time_us});
    trace("ge-finish: uid=" + std::to_string(uid) + " lista=" + std::to_string(list_id) +
          " func=" + hex32(g_ge_finish_func));
}

// sceUtilityLoadModule(module_id)
//
// On hardware this loads a firmware PRX -- the ATRAC3, font, AV codec or SAS
// modules -- so the game can then call into the library it provides. Here those
// libraries ARE the HLE: there is no PRX to load and nothing to do, so the
// honest answer is success. Reporting failure would make the game abandon the
// feature the module backs.
//
struct SavedataState {
    bool active{false};
    std::uint32_t parameter{};
    std::uint32_t parameter_size{};
    std::uint32_t mode{};
    std::uint32_t status{};
    bool io_finished{false};
};

SavedataState g_savedata;
std::filesystem::path g_savedata_root;
std::filesystem::path g_game_data_root;
std::filesystem::path g_font_root;

constexpr std::uint32_t kSavedataGameOffset = 60u;
constexpr std::uint32_t kSavedataSaveOffset = 76u;
constexpr std::uint32_t kSavedataFileOffset = 100u;
constexpr std::uint32_t kSavedataBufferOffset = 116u;
constexpr std::uint32_t kSavedataCapacityOffset = 120u;
constexpr std::uint32_t kSavedataSizeOffset = 124u;

std::filesystem::path savedata_directory(psprecomp::Runtime &rt,
                                         std::uint32_t parameter) {
    const std::string game = read_fixed_string(rt, parameter + kSavedataGameOffset, 13u);
    const std::string save = read_fixed_string(rt, parameter + kSavedataSaveOffset, 20u);
    const std::filesystem::path root = g_savedata_root.empty()
                                           ? std::filesystem::current_path() / "save" / "game"
                                           : g_savedata_root;
    return root / (game + save);
}

std::filesystem::path savedata_data_path(psprecomp::Runtime &rt,
                                         std::uint32_t parameter) {
    std::string file = read_fixed_string(rt, parameter + kSavedataFileOffset, 13u);
    if (file.empty()) file = "DATA.BIN";
    return savedata_directory(rt, parameter) / file;
}

bool write_guest_file(psprecomp::Runtime &rt, const std::filesystem::path &path,
                      std::uint32_t source, std::uint32_t size) {
    if (size != 0u && (source == 0u || !rt.memory().contains(source, size))) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    std::vector<std::uint8_t> bytes(size);
    for (std::uint32_t index = 0u; index < size; ++index)
        bytes[index] = rt.memory().load8(source + index);
    if (!bytes.empty())
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool write_savedata_auxiliary(psprecomp::Runtime &rt, std::uint32_t parameter,
                              std::uint32_t descriptor_offset, const char *name) {
    const std::uint32_t descriptor = parameter + descriptor_offset;
    const std::uint32_t buffer = rt.memory().load32(descriptor);
    const std::uint32_t capacity = rt.memory().load32(descriptor + 4u);
    const std::uint32_t size = rt.memory().load32(descriptor + 8u);
    if (buffer == 0u || size == 0u) return true;
    if (size > capacity) return false;
    return write_guest_file(rt, savedata_directory(rt, parameter) / name, buffer, size);
}

std::uint32_t load_savedata(psprecomp::Runtime &rt, std::uint32_t parameter) {
    const auto path = savedata_data_path(rt, parameter);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return kSavedataLoadNoData;
    const std::uint64_t file_size = std::filesystem::file_size(path, error);
    if (error || file_size > 0xFFFFFFFFull) return 0x80110305u;
    const std::uint32_t destination = rt.memory().load32(parameter + kSavedataBufferOffset);
    const std::uint32_t capacity = rt.memory().load32(parameter + kSavedataCapacityOffset);
    if (file_size > capacity || (file_size != 0u &&
        (destination == 0u || !rt.memory().contains(destination, static_cast<std::size_t>(file_size)))))
        return 0x80110308u;
    std::ifstream input(path, std::ios::binary);
    if (!input) return 0x80110305u;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
    if (!bytes.empty()) input.read(reinterpret_cast<char *>(bytes.data()),
                                   static_cast<std::streamsize>(bytes.size()));
    if (!input && !bytes.empty()) return 0x80110305u;
    if (!bytes.empty()) rt.memory().copy_in(destination, bytes);
    rt.memory().store32(parameter + kSavedataSizeOffset,
                        static_cast<std::uint32_t>(bytes.size()));
    return 0u;
}

std::uint32_t save_savedata(psprecomp::Runtime &rt, std::uint32_t parameter) {
    const std::uint32_t source = rt.memory().load32(parameter + kSavedataBufferOffset);
    const std::uint32_t capacity = rt.memory().load32(parameter + kSavedataCapacityOffset);
    const std::uint32_t size = rt.memory().load32(parameter + kSavedataSizeOffset);
    if (size > capacity || (size != 0u &&
        (source == 0u || !rt.memory().contains(source, size)))) return 0x80110388u;
    if (!write_guest_file(rt, savedata_data_path(rt, parameter), source, size) ||
        !write_savedata_auxiliary(rt, parameter, 0x584u, "ICON0.PNG") ||
        !write_savedata_auxiliary(rt, parameter, 0x594u, "ICON1.PMF") ||
        !write_savedata_auxiliary(rt, parameter, 0x5A4u, "PIC1.PNG") ||
        !write_savedata_auxiliary(rt, parameter, 0x5B4u, "SND0.AT3"))
        return 0x80110385u;
    return 0u;
}

std::uint32_t execute_savedata(psprecomp::Runtime &rt, std::uint32_t parameter,
                               std::uint32_t mode) {
    switch (mode) {
    case 0u: // AUTOLOAD
    case 2u: // LOAD
    case 4u: // LISTLOAD after the game selected saveName
        return load_savedata(rt, parameter);
    case 1u: // AUTOSAVE
    case 3u: // SAVE
    case 5u: // LISTSAVE after the game selected saveName
        return save_savedata(rt, parameter);
    case 9u: // AUTODELETE
    case 10u: { // DELETE
        std::error_code error;
        const auto directory = savedata_directory(rt, parameter);
        if (!std::filesystem::exists(directory, error) || error) return 0x80110347u;
        return std::filesystem::remove_all(directory, error) != 0u && !error
            ? 0u : 0x80110345u;
    }
    default:
        // Unsupported utility dialogs report a PSP error to the game. They do
        // not terminate the recompiled process.
        return 0x80110300u;
    }
}

void install_utility(psprecomp::Runtime &runtime) {
    bind(runtime, "sceUtility", 0x2A2B3DE0u, "sceUtilityLoadModule",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             ctx.set_gpr(2, 0u);
         });

    // Native local Savedata. The game keeps its PSP parameter structure and
    // buffers; this HLE only replaces the Memory Stick filesystem boundary.
    bind(runtime, "sceUtility", 0x50C4CD57u, "sceUtilitySavedataInitStart",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t parameter = ctx.gpr[4];
             if (g_savedata.active) {
                 ctx.set_gpr(2, kUtilityInvalidStatus);
                 return;
             }
             if ((parameter & 3u) != 0u || !rt.memory().contains(parameter, 4u)) {
                 ctx.set_gpr(2, kUtilityInvalidParamSize);
                 return;
             }
             const std::uint32_t size = rt.memory().load32(parameter);
             if ((size != 1480u && size != 1500u && size != 1536u) ||
                 !rt.memory().contains(parameter, size)) {
                 ctx.set_gpr(2, kUtilityInvalidParamSize);
                 return;
             }

             const std::uint32_t mode = rt.memory().load32(parameter + 48u);
             const std::string game = read_fixed_string(rt, parameter + 60u, 13u);
             const std::string save = read_fixed_string(rt, parameter + 76u, 20u);
             const std::string file = read_fixed_string(rt, parameter + 100u, 13u);
             const auto unsafe_component = [](const std::string &value) {
                 return value == "." || value == ".." ||
                        value.find('/') != std::string::npos ||
                        value.find('\\') != std::string::npos;
             };
             if (mode > 22u || unsafe_component(game) || unsafe_component(save) ||
                 unsafe_component(file)) {
                 ctx.set_gpr(2, kUtilityInvalidParamSize);
                 return;
             }

             g_savedata.active = true;
             g_savedata.parameter = parameter;
             g_savedata.parameter_size = size;
             g_savedata.mode = mode;
             g_savedata.status = 1u; // SCE_UTILITY_STATUS_INITIALIZE
             std::printf("[savedata] size=%u mode=%u game=%s save=%s file=%s "
                         "buffer=%s capacity=%u requested=%u\n",
                         size, mode, game.c_str(), save.c_str(), file.c_str(),
                         hex32(rt.memory().load32(parameter + 116u)).c_str(),
                         rt.memory().load32(parameter + 120u),
                         rt.memory().load32(parameter + 124u));
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceUtility", 0x8874DBE0u, "sceUtilitySavedataGetStatus",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             if (!g_savedata.active) {
                 ctx.set_gpr(2, 0u); // SCE_UTILITY_STATUS_NONE
                 return;
             }
             const std::uint32_t reported = g_savedata.status;
             // El firmware hace estas transiciones con retrasos. El runtime usa
             // llamadas como frontera determinista: cada estado se observa al
             // menos una vez antes de avanzar.
             if (reported == 1u) g_savedata.status = 2u; // INITIALIZE -> RUNNING
             else if (reported == 4u) {
                 g_savedata = {}; // SHUTDOWN -> NONE despues de reportarlo
             }
             ctx.set_gpr(2, reported);
         });

    bind(runtime, "sceUtility", 0xD4B95FFBu, "sceUtilitySavedataUpdate",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             if (!g_savedata.active || g_savedata.status != 2u) {
                 ctx.set_gpr(2, kUtilityInvalidStatus);
                 return;
             }
             const std::uint32_t result =
                 execute_savedata(rt, g_savedata.parameter, g_savedata.mode);
             if (rt.memory().contains(g_savedata.parameter + 28u, 4u)) {
                 rt.memory().store32(g_savedata.parameter + 28u, result);
             }
             g_savedata.io_finished = true;
             g_savedata.status = 3u; // SCE_UTILITY_STATUS_FINISHED
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceUtility", 0x9790B33Cu, "sceUtilitySavedataShutdownStart",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             if (!g_savedata.active || g_savedata.status != 3u ||
                 !g_savedata.io_finished) {
                 ctx.set_gpr(2, kUtilityInvalidStatus);
                 return;
             }
             g_savedata.status = 4u; // SCE_UTILITY_STATUS_SHUTDOWN
             ctx.set_gpr(2, 0u);
         });

}

void install_graphics(psprecomp::Runtime &runtime) {
    // The PSP's embedded VRAM starts here; the guest uses the returned address
    // to place its own buffers, so this one is a real answer, not a placeholder.
    bind_success(runtime, "sceGe_user", 0xE47E40E4u, "sceGeEdramGetAddr",
                 psprecomp::GuestMemory::kVramPhysicalBase);

    // sceGeSetCallback takes a PspGeCallbackData: signal_func, signal_arg,
    // finish_func, finish_arg.
    bind(runtime, "sceGe_user", 0xA4FC06A4u, "sceGeSetCallback",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t data = ctx.gpr[4];
             if (data != 0u && rt.memory().contains(data, 16u)) {
                 g_ge_finish_func = rt.memory().load32(data + 8u);
                 g_ge_finish_arg = rt.memory().load32(data + 12u);
             }
             ctx.set_gpr(2, 0u);
         });
    bind(runtime, "sceGe_user", 0x05DB22CEu, "sceGeUnsetCallback",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             g_ge_finish_func = 0u;
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceGe_user", 0xAB49E76Au, "sceGeListEnQueue",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::int32_t id = g_next_display_list_id++;
             consume_ge_enqueue(rt, id, ctx.gpr[4], ctx.gpr[5]);
             trace_ge_enqueue(rt, id, ctx.gpr[4], ctx.gpr[5], ctx.gpr[6],
                              ctx.gpr[7]);
             // Nothing walks the list, but the game is told it finished -- and
             // being told is the part that matters, because it waits for the
             // callback and would otherwise spin forever.
             schedule_ge_finish_callback(rt, id);
             ctx.set_gpr(2, static_cast<std::uint32_t>(id));
         });
    bind(runtime, "sceGe_user", 0xE0D68148u, "sceGeListUpdateStallAddr",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             consume_ge_stall_update(rt, static_cast<std::int32_t>(ctx.gpr[4]),
                                     ctx.gpr[5]);
             trace_ge_stall_update(rt, static_cast<std::int32_t>(ctx.gpr[4]),
                                   ctx.gpr[5]);
             ctx.set_gpr(2, 0u);
         });
    // Sync calls: the queue is always already drained.
    bind_success(runtime, "sceGe_user", 0x03444EB4u, "sceGeListSync", 0u);
    bind_success(runtime, "sceGe_user", 0xB287BD61u, "sceGeDrawSync", 0u);

    // The three standard streams. Only their descriptors are asked for.
    bind_success(runtime, "StdioForUser", 0x172D316Eu, "sceKernelStdin", 0u);
    bind_success(runtime, "StdioForUser", 0xA6BAB2E9u, "sceKernelStdout", 1u);
    bind_success(runtime, "StdioForUser", 0xF78BA90Au, "sceKernelStderr", 2u);
}

// File I/O, served from the user's own extracted game data.
//
// Paths arrive as PSP device paths such as `disc0:/PSP_GAME/USRDIR/...`.
// `Runtime::translate_path` strips the device, rejects any `..`, and resolves
// what is left under the game root the host set, so nothing outside that
// directory is reachable.
//
// The asynchronous variants are implemented synchronously: the work is done
// during the call and the "async result" is whatever it returned. That is a
// simplification, and a visible one -- a game that relies on overlapping I/O
// with computation will simply see it complete sooner than it should.
struct OpenFile {
    std::string psp_path;
    std::fstream stream;
    std::int64_t async_result{0};
    // The raw UMD device addresses by SECTOR, not by byte. Measured: the loader
    // seeked to 82192 on `umd0:` two thousand times without advancing. As a byte
    // offset that is not even 2048-aligned and lands in the ISO9660 metadata; as
    // a sector it is byte 168329216, where the image really does contain
    // `cmn/boot.seq`. Treating it as bytes made every read return filesystem
    // structures, so the loader retried the same sector forever.
    bool sector_addressed{false};
};

// UMD sector size.
constexpr std::int64_t kUmdSectorBytes = 2048;

std::map<std::int32_t, OpenFile> g_files;
std::int32_t g_next_file_uid = 0x3000;

constexpr std::uint32_t kErrorFileNotFound = 0x80010002u;
constexpr std::uint32_t kErrorBadFileDescriptor = 0x80020323u;

OpenFile *find_file(std::uint32_t raw_uid) {
    const auto found = g_files.find(static_cast<std::int32_t>(raw_uid));
    return found == g_files.end() ? nullptr : &found->second;
}

// Where the raw UMD device resolves to. Set once by the host, which owns the
// decision about which disc image the recompiled module is running against.
std::filesystem::path g_umd_image;

// Is this the UMD device itself rather than a file on it?
//
// The game does not read its data file by file: it opens
// `disc0:/PSP_GAME/USRDIR/data/fpack/sector.lst`, which is a sector map, and
// then opens `umd0:` to read the disc by raw sector. Measured: without this the
// second open failed and the game retried `sceIoOpen` 499978 times in one run.
//
// A device path is one with nothing after the colon.
bool is_umd_device(const std::string &psp_path) {
    const auto colon = psp_path.find(':');
    if (colon == std::string::npos) return false;
    const std::string device = psp_path.substr(0, colon);
    const std::string rest = psp_path.substr(colon + 1u);
    const bool empty_rest = rest.empty() || rest == "/" || rest == "\\";
    return empty_rest && (device == "umd0" || device == "umd1" || device == "disc0");
}

std::int32_t open_file(psprecomp::Runtime &rt, const std::string &psp_path, std::uint32_t flags) {
    std::filesystem::path host_path;
    bool device_open = false;
    if (is_umd_device(psp_path)) {
        // Raw device access goes to the disc image, read-only. Without an image
        // the honest answer is "no such file": pretending the device opened
        // would turn every later sector read into silent garbage.
        if (g_umd_image.empty() || !std::filesystem::exists(g_umd_image)) {
            return static_cast<std::int32_t>(kErrorFileNotFound);
        }
        host_path = g_umd_image;
        flags &= ~0x2u; // never writable
        device_open = true;
    } else {
        try {
            host_path = rt.translate_path(psp_path);
        } catch (const std::exception &) {
            return static_cast<std::int32_t>(kErrorFileNotFound);
        }
    }
    // PSP open flags: 1 read, 2 write, 0x200 create, 0x400 truncate.
    const bool wants_write = (flags & 0x2u) != 0u;
    const bool wants_create = (flags & 0x200u) != 0u;
    if (!wants_write && !wants_create && !std::filesystem::exists(host_path)) {
        return static_cast<std::int32_t>(kErrorFileNotFound);
    }
    auto mode = std::ios::binary | std::ios::in;
    if (wants_write) mode |= std::ios::out;
    if ((flags & 0x400u) != 0u) mode |= std::ios::trunc;

    OpenFile entry;
    entry.psp_path = psp_path;
    entry.sector_addressed = device_open;
    entry.stream.open(host_path, mode);
    if (!entry.stream.is_open() && wants_create) {
        entry.stream.open(host_path, std::ios::binary | std::ios::out);
        entry.stream.close();
        entry.stream.open(host_path, mode);
    }
    if (!entry.stream.is_open()) return static_cast<std::int32_t>(kErrorFileNotFound);

    const std::int32_t uid = g_next_file_uid++;
    g_files.emplace(uid, std::move(entry));
    trace("open: uid=" + std::to_string(uid) + " " + psp_path);
    return uid;
}

std::int32_t read_file(psprecomp::Runtime &rt, OpenFile &file, std::uint32_t destination,
                       std::uint32_t size) {
    if (size == 0u) return 0;
    // Same units rule as the seek: on the raw UMD device a length is a count of
    // sectors, on an ordinary file it is a count of bytes.
    //
    // This was briefly "fixed" to a byte count on the strength of a coincidence
    // -- a 0x22 read whose sector happened to hold a 34-byte header. The game
    // itself settles it. Before reading it computes `ceil(byteLength / 2048)`
    // with a `>> 11` and stores it at 0x08A1CC30, passes THAT as the size, and
    // afterwards compares the returned count against the same word
    // (sub_08957BB8, at guest 0x08957EF0). The declared sizes match exactly:
    // cmn/boot.seq is 74352 bytes -> 37 sectors -> the 0x25 it asks for, and
    // snd/se/system.pbd is 69504 -> 34 -> 0x22. Sectors, both in and out.
    const std::uint32_t bytes = file.sector_addressed
        ? static_cast<std::uint32_t>(size * static_cast<std::uint32_t>(kUmdSectorBytes))
        : size;
    if (!rt.memory().contains(destination, bytes)) {
        return static_cast<std::int32_t>(kErrorBadFileDescriptor);
    }
    std::vector<char> buffer(bytes);
    file.stream.read(buffer.data(), static_cast<std::streamsize>(bytes));
    const auto read = static_cast<std::uint32_t>(file.stream.gcount());
    file.stream.clear(); // a short read at EOF is not an error to the guest
    if (read != 0u) {
        rt.memory().copy_in(destination,
                            std::span<const std::uint8_t>(
                                reinterpret_cast<const std::uint8_t *>(buffer.data()), read));
    }
    // The guest counts in whatever unit it asked in, and on the device that is
    // sectors -- it compares this against its own sector count.
    return static_cast<std::int32_t>(
        file.sector_addressed ? read / static_cast<std::uint32_t>(kUmdSectorBytes) : read);
}

void install_file_io(psprecomp::Runtime &runtime) {
    bind(runtime, "IoFileMgrForUser", 0x109F50BCu, "sceIoOpen",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::string path = ctx.gpr[4] != 0u
                 ? rt.memory().read_c_string(ctx.gpr[4], 256u) : std::string();
             // The path is the single most informative thing about where the
             // boot has got to, and the generic HLE trace only shows the
             // POINTER. Half of these are built in RAM at run time, so the
             // pointer alone says nothing.
             // The guest timestamp is what makes this trace usable as a
             // timeline: `--script` places presses on guest time, so knowing
             // WHEN a screen loaded its music is what lets the next press be
             // aimed rather than guessed.
             if (g_trace_hle)
                 std::printf("[io] t=%llu.%03llus open '%s'\n",
                             static_cast<unsigned long long>(g_virtual_time_us / 1000000ull),
                             static_cast<unsigned long long>((g_virtual_time_us / 1000ull) % 1000ull),
                             path.c_str());
             ctx.set_gpr(2, static_cast<std::uint32_t>(open_file(rt, path, ctx.gpr[5])));
         });

    bind(runtime, "IoFileMgrForUser", 0x810C4BC3u, "sceIoClose",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             ctx.set_gpr(2, g_files.erase(static_cast<std::int32_t>(ctx.gpr[4])) != 0u
                               ? 0u : kErrorBadFileDescriptor);
         });

    bind(runtime, "IoFileMgrForUser", 0x6A638D83u, "sceIoRead",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             OpenFile *file = find_file(ctx.gpr[4]);
             if (file == nullptr) { ctx.set_gpr(2, kErrorBadFileDescriptor); return; }
             ctx.set_gpr(2, static_cast<std::uint32_t>(read_file(rt, *file, ctx.gpr[5], ctx.gpr[6])));
         });

    bind(runtime, "IoFileMgrForUser", 0x42EC03ACu, "sceIoWrite",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             OpenFile *file = find_file(ctx.gpr[4]);
             if (file == nullptr) { ctx.set_gpr(2, kErrorBadFileDescriptor); return; }
             const std::uint32_t size = ctx.gpr[6];
             if (size == 0u || !rt.memory().contains(ctx.gpr[5], size)) { ctx.set_gpr(2, 0u); return; }
             std::vector<std::uint8_t> buffer(size);
             rt.memory().copy_out(ctx.gpr[5], buffer);
             file->stream.write(reinterpret_cast<const char *>(buffer.data()),
                                static_cast<std::streamsize>(size));
             ctx.set_gpr(2, file->stream ? size : 0u);
         });

    // sceIoLseek takes a 64-bit offset. Under o32 that pair lands in a2/a3 -- a1
    // is skipped for alignment -- and `whence` follows on the stack, at 16(sp).
    // The result is 64-bit too and comes back in v0/v1.
    bind(runtime, "IoFileMgrForUser", 0x27EB27B8u, "sceIoLseek",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             OpenFile *file = find_file(ctx.gpr[4]);
             if (file == nullptr) {
                 ctx.set_gpr(2, kErrorBadFileDescriptor);
                 ctx.set_gpr(3, 0xFFFFFFFFu);
                 return;
             }
             std::int64_t offset = static_cast<std::int64_t>(
                 (static_cast<std::uint64_t>(ctx.gpr[7]) << 32u) | ctx.gpr[6]);
             // On the raw device the units are sectors; on a file they are bytes.
             if (file->sector_addressed) offset *= kUmdSectorBytes;
             // `whence` arrives in $8 ($t0), NOT at 16(sp).
             //
             // Plain o32 would put a fifth argument on the stack, and reading it
             // from 16(sp) is what this profile did. It was wrong, and silently:
             // the word there happened to be the caller's read buffer pointer,
             // which is neither 1 nor 2, so every seek fell through to
             // SEEK_SET and returned 0.
             //
             // The generated code settles it. Both call sites in `sub_08956...`
             // load $8 immediately before the jump, with exactly the two values
             // the size idiom needs:
             //
             //     gpr[6]=0; gpr[7]=0; gpr[8]=2  -> lseek(fd, 0, SEEK_END)
             //     gpr[6]=0; gpr[7]=0; gpr[8]=0  -> lseek(fd, 0, SEEK_SET)
             //
             // and the result of the first is kept in $19 and handed to
             // `sceIoRead` as the size. With whence lost, that size was 0: the
             // game read nothing and then dereferenced the buffer it never
             // filled. That is PSP-VOICE-001.
             const std::uint32_t whence = ctx.gpr[8];
             const auto direction = whence == 1u ? std::ios::cur
                                  : whence == 2u ? std::ios::end
                                                 : std::ios::beg;
             file->stream.clear();
             file->stream.seekg(static_cast<std::streamoff>(offset), direction);
             std::int64_t position = static_cast<std::int64_t>(file->stream.tellg());
             if (file->sector_addressed && position > 0) position /= kUmdSectorBytes;
             // PSP-VOICE-001: the whence word decides everything here and it
             // travels on the STACK, not in a register, so a wrong stack read
             // is indistinguishable from a seek to 0. Printed with the raw
             // arguments so the caller's intent is visible instead of guessed.
             if (g_trace_hle)
                 std::printf("[seek] fd=%d a1=%08X a2=%08X a3=%08X sp=%08X "
                             "16(sp)=%08X whence=%u -> %lld\n",
                             static_cast<int>(ctx.gpr[4]), ctx.gpr[5], ctx.gpr[6],
                             ctx.gpr[7], ctx.gpr[29],
                             rt.memory().contains(ctx.gpr[29] + 16u, 4u)
                                 ? rt.memory().load32(ctx.gpr[29] + 16u) : 0xFFFFFFFFu,
                             whence, static_cast<long long>(position));
             trace("io-seek fd=" + std::to_string(ctx.gpr[4]) + " a " +
                   std::to_string(position) + " (whence=" + std::to_string(whence) + ")");
             ctx.set_gpr(2, static_cast<std::uint32_t>(position));
             ctx.set_gpr(3, static_cast<std::uint32_t>(static_cast<std::uint64_t>(position) >> 32u));
         });

    bind(runtime, "IoFileMgrForUser", 0xACE946E8u, "sceIoGetstat",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::string path = ctx.gpr[4] != 0u
                 ? rt.memory().read_c_string(ctx.gpr[4], 256u) : std::string();
             std::filesystem::path host_path;
             try { host_path = rt.translate_path(path); }
             catch (const std::exception &) { ctx.set_gpr(2, kErrorFileNotFound); return; }
             std::error_code error;
             const bool exists = std::filesystem::exists(host_path, error);
             if (!exists) { ctx.set_gpr(2, kErrorFileNotFound); return; }
             const bool directory = std::filesystem::is_directory(host_path, error);
             const std::uint64_t size = directory ? 0u
                 : static_cast<std::uint64_t>(std::filesystem::file_size(host_path, error));
             // SceIoStat: mode, attr, size(64) at +8, then three time structs.
             const std::uint32_t out = ctx.gpr[5];
             if (out != 0u && rt.memory().contains(out, 52u)) {
                 rt.memory().zero(out, 52u);
                 rt.memory().store32(out, directory ? 0x1000u : 0x2000u);
                 rt.memory().store32(out + 4u, directory ? 0x0010u : 0x0020u);
                 rt.memory().store32(out + 8u, static_cast<std::uint32_t>(size));
                 rt.memory().store32(out + 12u, static_cast<std::uint32_t>(size >> 32u));
             }
             ctx.set_gpr(2, 0u);
         });

    // Asynchronous variants, done synchronously. The completion value is stored
    // so a later wait or poll can hand it back.
    bind(runtime, "IoFileMgrForUser", 0x89AA9906u, "sceIoOpenAsync",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::string path = ctx.gpr[4] != 0u
                 ? rt.memory().read_c_string(ctx.gpr[4], 256u) : std::string();
             const std::int32_t uid = open_file(rt, path, ctx.gpr[5]);
             if (uid > 0) g_files[uid].async_result = uid;
             ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
         });

    bind(runtime, "IoFileMgrForUser", 0xA0B5A7C2u, "sceIoReadAsync",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             OpenFile *file = find_file(ctx.gpr[4]);
             if (file == nullptr) { ctx.set_gpr(2, kErrorBadFileDescriptor); return; }
             file->async_result = read_file(rt, *file, ctx.gpr[5], ctx.gpr[6]);
             ctx.set_gpr(2, 0u);
         });

    for (const auto &entry : {std::pair<std::uint32_t, const char *>{0xE23EEC33u, "sceIoWaitAsync"},
                              std::pair<std::uint32_t, const char *>{0x35DBD746u, "sceIoWaitAsyncCB"},
                              std::pair<std::uint32_t, const char *>{0x3251EA56u, "sceIoPollAsync"}}) {
        bind(runtime, "IoFileMgrForUser", entry.first, entry.second,
             [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                 OpenFile *file = find_file(ctx.gpr[4]);
                 if (file == nullptr) { ctx.set_gpr(2, kErrorBadFileDescriptor); return; }
                 // The work already happened, so the result is always ready.
                 const std::uint32_t out = ctx.gpr[5];
                 if (out != 0u && rt.memory().contains(out, 8u)) {
                     rt.memory().store32(out, static_cast<std::uint32_t>(file->async_result));
                     rt.memory().store32(out + 4u, static_cast<std::uint32_t>(
                         static_cast<std::uint64_t>(file->async_result) >> 32u));
                 }
                 ctx.set_gpr(2, 0u);
             });
    }

    // Renaming inside the user's own extracted data is not something this
    // profile should be doing, so it reports failure rather than touching it.
    bind_success(runtime, "IoFileMgrForUser", 0x779103A0u, "sceIoRename", kErrorFileNotFound);
}

// Calls that are genuinely nothing on a host, as opposed to calls that are being
// ignored. Keeping the distinction visible matters: a cache writeback really is
// a no-op against coherent host memory, whereas returning 0 from something that
// should have produced a pointer is a lie that surfaces later as a memory fault.
void install_benign(psprecomp::Runtime &runtime) {
    // UtilsForUser: the Allegrex data-cache maintenance family. Guest memory here
    // is an ordinary host buffer, so there is no cache to write back or
    // invalidate and success is the correct and complete behaviour.
    for (const std::uint32_t nid : {
             0x79D1C3FAu, // sceKernelDcacheWritebackAll
             0xB435DEC5u, // sceKernelDcacheWritebackInvalidateAll
             0x34B9FA9Eu, // sceKernelDcacheWritebackInvalidateRange
             0x3EE30821u, // sceKernelDcacheWritebackRange
             0x6AD345D7u, // sceKernelSetGPO, drives the debug LEDs
             0x71EC4271u,
             0x91E4F6A7u,
             0x27CC57F0u,
         }) {
        runtime.register_hle("UtilsForUser", nid,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    }

    // Kernel_Library interrupt control. These two are confirmed against the
    // public PSP library documentation rather than assumed: 0x092968F4 is
    // `sceKernelCpuSuspendIntr`, which takes nothing and returns the previous
    // interrupt state, and 0x5F10D406 is `sceKernelCpuResumeIntr`, which takes
    // that value back.
    //
    // This profile runs one guest thread at a time and never switches inside a
    // critical section, so there is genuinely nothing to mask; the saved flag
    // only has to round-trip. The game uses the pair around its own module
    // registry, at `0x08812BC4`.
    //
    // The other two Kernel_Library NIDs this module imports, 0x15B6446B and
    // 0xBEA46419, stay unbound: their behaviour was not established, and a
    // guessed return value is worse than none. That lesson was learned here --
    // an earlier guess sent a bad value into an index computation and the run
    // died reading `0x00000004`, far from the cause.
    bind(runtime, "Kernel_Library", 0x092968F4u, "sceKernelCpuSuspendIntr",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             ctx.set_gpr(2, g_interrupts_enabled ? 1u : 0u);
             g_interrupts_enabled = false;
         });
    // Re-enabling interrupts is a scheduling point, and it has to be one.
    //
    // Guest spinlocks are written assuming preemption: a thread takes the lock,
    // finds it busy, releases interrupts and loops, expecting something else to
    // run in between. A purely cooperative scheduler never switches there, so the
    // spinning thread holds the CPU forever. Measured before this: 2.222.217
    // suspend/resume pairs in a row while a priority-0 GE callback sat queued and
    // never ran.
    //
    // So when interrupts come back on and a strictly higher-priority thread is
    // ready and awake, it takes over. Strictly higher keeps it from ping-ponging
    // between equals.
    bind(runtime, "Kernel_Library", 0x5F10D406u, "sceKernelCpuResumeIntr",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             g_interrupts_enabled = ctx.gpr[4] != 0u;
             ctx.set_gpr(2, 0u);
             if (!g_interrupts_enabled) return;
             const std::uint32_t mine = priority_of(g_table.current_uid);
             const bool better_waiting = std::any_of(
                 g_table.ready.begin(), g_table.ready.end(),
                 [mine](const Continuation &c) {
                     return c.wake_at_us <= g_virtual_time_us && priority_of(c.uid) < mine;
                 });
             if (!better_waiting) return;
             psprecomp::AllegrexContext preempted = ctx;
             preempted.pc = ctx.gpr[31];
             g_table.ready.push_back({g_table.current_uid, preempted, g_virtual_time_us});
             (void)activate_next(rt, ctx, "resume-intr-preempt");
         });

}

// Module management. The recompiled game is the only executable module. A
// small number of disc PRXs are nevertheless just delivery vehicles for
// libraries already supplied by this profile's HLE. PPSSPP treats those as
// fake/HLE modules too: their lifecycle is real from the guest's point of view,
// but their PRX code is not executed.
constexpr std::uint32_t kSelfModuleId = 0x1000u;
constexpr std::uint32_t kErrorUnknownModule = 0x80020134u;

struct HleModuleRecord {
    std::string path;
    bool started{};
};

std::int32_t g_next_module_uid = 0x4000;
std::map<std::int32_t, HleModuleRecord> g_hle_modules;

constexpr std::uint32_t kFontErrorOutOfMemory = 0x80460001u;
constexpr std::uint32_t kFontErrorInvalidLibrary = 0x80460002u;
constexpr std::uint32_t kFontErrorInvalidParameter = 0x80460003u;
constexpr std::uint32_t kFontErrorTooManyOpenFonts = 0x80460009u;
constexpr std::uint32_t kPspInternalFontCount = 18u;
constexpr std::uint32_t kPspFontStyleSize = 0xA8u;

struct PspFontRegistryStyle {
    float size;
    std::uint16_t family;
    std::uint16_t style;
    std::uint16_t language;
    std::uint16_t country;
    const char* name;
    const char* file_name;
};

// Exact sceFontGetFontList metadata observed from the PSP-compatible service
// while Fate used the user's unlocked menu state.  This is firmware registry
// metadata only; no host font, replacement glyph, or PS2 asset is involved.
constexpr std::array<PspFontRegistryStyle, kPspInternalFontCount>
    kPspFontRegistryStyles{{
        {10.125f, 1u, 103u, 1u, 1u, "FTT-NewRodin Pro DB",    "jpn0.pgf"},
        {10.125f, 1u,   1u, 2u, 1u, "FTT-NewRodin Pro Latin", "ltn0.pgf"},
        {10.125f, 2u,   1u, 2u, 1u, "FTT-Matisse Pro Latin",  "ltn1.pgf"},
        {10.125f, 1u,   2u, 2u, 1u, "FTT-NewRodin Pro Latin", "ltn2.pgf"},
        {10.125f, 2u,   2u, 2u, 1u, "FTT-Matisse Pro Latin",  "ltn3.pgf"},
        {10.125f, 1u,   5u, 2u, 1u, "FTT-NewRodin Pro Latin", "ltn4.pgf"},
        {10.125f, 2u,   5u, 2u, 1u, "FTT-Matisse Pro Latin",  "ltn5.pgf"},
        {10.125f, 1u,   6u, 2u, 1u, "FTT-NewRodin Pro Latin", "ltn6.pgf"},
        {10.125f, 2u,   6u, 2u, 1u, "FTT-Matisse Pro Latin",  "ltn7.pgf"},
        { 7.000f, 1u,   1u, 2u, 1u, "FTT-NewRodin Pro Latin", "ltn8.pgf"},
        { 7.000f, 2u,   1u, 2u, 1u, "FTT-Matisse Pro Latin",  "ltn9.pgf"},
        { 7.000f, 1u,   2u, 2u, 1u, "FTT-NewRodin Pro Latin", "ltn10.pgf"},
        { 7.000f, 2u,   2u, 2u, 1u, "FTT-Matisse Pro Latin",  "ltn11.pgf"},
        { 7.000f, 1u,   5u, 2u, 1u, "FTT-NewRodin Pro Latin", "ltn12.pgf"},
        { 7.000f, 2u,   5u, 2u, 1u, "FTT-Matisse Pro Latin",  "ltn13.pgf"},
        { 7.000f, 1u,   6u, 2u, 1u, "FTT-NewRodin Pro Latin", "ltn14.pgf"},
        { 7.000f, 2u,   6u, 2u, 1u, "FTT-Matisse Pro Latin",  "ltn15.pgf"},
        {10.125f, 1u,   1u, 3u, 3u, "AsiaNHH(512Johab)",      "kr0.pgf"},
    }};

struct PspFontAllocationSizes {
    std::uint32_t full;
    std::uint32_t stingy;
};

constexpr std::array<PspFontAllocationSizes, kPspInternalFontCount>
    kPspFontAllocationSizes{{
        {1581700u, 145844u}, { 69108u, 16680u}, { 65124u, 16920u},
        {  72948u,  16872u}, { 67700u, 17112u}, { 72828u, 16648u},
        {  68220u,  16928u}, { 77032u, 16792u}, { 71144u, 17160u},
        {  41000u,  16192u}, { 40164u, 16476u}, { 42692u, 16300u},
        {  41488u,  16656u}, { 43136u, 16176u}, { 41772u, 16436u},
        {  45184u,  16272u}, { 43044u, 16704u}, {394192u, 51856u},
    }};

struct FontLibraryState {
    std::array<std::uint32_t, 11> params{};
    std::uint32_t handle{};
    std::uint32_t slot_count{};
    std::uint16_t alt_character{0x5Fu};
    std::array<std::int32_t, 9> slot_font_indices{
        -1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::array<std::uint32_t, 9> slot_ref_counts{};
    std::array<std::uint32_t, 9> slot_allocations{};
    std::uint32_t char_info_allocation{};
};

struct FontNewLibPending {
    psprecomp::AllegrexContext resume{};
    std::array<std::uint32_t, 11> params{};
    std::uint32_t error_address{};
    std::uint32_t allocation_size{};
};

struct FontOpenPending {
    psprecomp::AllegrexContext resume{};
    std::uint32_t library{};
    std::uint32_t error_address{};
    std::uint32_t handle{};
    std::uint32_t slot{};
};

enum class FontCharInfoCallbackStage { free_previous, allocate };

struct FontCharInfoPending {
    psprecomp::AllegrexContext resume{};
    std::uint32_t library{};
    std::uint32_t allocation_size{};
    FontCharInfoCallbackStage stage{FontCharInfoCallbackStage::allocate};
};

std::map<std::uint32_t, FontLibraryState> g_font_libraries;
std::map<std::int32_t, FontNewLibPending> g_font_newlib_pending;
std::map<std::int32_t, FontOpenPending> g_font_open_pending;
std::map<std::int32_t, FontCharInfoPending> g_font_char_info_pending;
std::map<std::uint32_t, fuc::port::psp::PgfFont> g_psp_fonts;

void write_psp_font_style(psprecomp::Runtime& rt, std::uint32_t address,
                          const PspFontRegistryStyle& style) {
    rt.memory().zero(address, kPspFontStyleSize);
    rt.memory().store32(address + 0x00u, std::bit_cast<std::uint32_t>(style.size));
    rt.memory().store32(address + 0x04u, std::bit_cast<std::uint32_t>(style.size));
    rt.memory().store32(address + 0x08u, std::bit_cast<std::uint32_t>(128.0f));
    rt.memory().store32(address + 0x0Cu, std::bit_cast<std::uint32_t>(128.0f));
    rt.memory().store16(address + 0x14u, style.family);
    rt.memory().store16(address + 0x16u, style.style);
    rt.memory().store16(address + 0x1Au, style.language);
    rt.memory().store16(address + 0x1Eu, style.country);
    for (std::uint32_t index = 0u; style.name[index] != '\0' && index < 63u; ++index)
        rt.memory().store8(address + 0x20u + index,
                           static_cast<std::uint8_t>(style.name[index]));
    for (std::uint32_t index = 0u; style.file_name[index] != '\0' && index < 63u; ++index)
        rt.memory().store8(address + 0x60u + index,
                           static_cast<std::uint8_t>(style.file_name[index]));
}

bool find_open_psp_font(std::uint32_t handle, FontLibraryState*& library,
                        std::uint32_t& slot, std::uint32_t& font_index) {
    for (auto& [library_address, candidate] : g_font_libraries) {
        for (std::uint32_t index = 0u; index < candidate.slot_count; ++index) {
            if (candidate.slot_ref_counts[index] == 0u ||
                library_address + 0x4Cu + index * 0x4Cu != handle)
                continue;
            library = &candidate;
            slot = index;
            font_index = static_cast<std::uint32_t>(candidate.slot_font_indices[index]);
            return font_index < kPspInternalFontCount;
        }
    }
    return false;
}

enum class PgfLoadStatus { success, missing, wrong_size, invalid };

PgfLoadStatus load_psp_font(const std::uint32_t font_index,
                           fuc::port::psp::PgfFont*& font) {
    if (font_index >= kPspInternalFontCount) return PgfLoadStatus::invalid;
    if (const auto cached = g_psp_fonts.find(font_index); cached != g_psp_fonts.end()) {
        font = &cached->second;
        return PgfLoadStatus::success;
    }
    const char* test_root = std::getenv("FUC_PSP_FONT_TEST_ROOT");
    const bool test_mode = test_root != nullptr && test_root[0] != '\0';
    const std::filesystem::path root = test_mode
        ? std::filesystem::path(test_root)
        : (g_font_root.empty() ? std::filesystem::path("private_original/psp_flash0/font")
                               : g_font_root);
    const std::filesystem::path path =
        root / kPspFontRegistryStyles[font_index].file_name;
    std::error_code error;
    const std::uint64_t size = std::filesystem::file_size(path, error);
    if (error) return PgfLoadStatus::missing;
    // The firmware registry gives the exact original file sizes. Replacement
    // fixtures are accepted only through the explicit test-only environment.
    if (!test_mode && size != kPspFontAllocationSizes[font_index].full)
        return PgfLoadStatus::wrong_size;
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size == 0u || !input.read(reinterpret_cast<char*>(bytes.data()),
                                  static_cast<std::streamsize>(bytes.size())))
        return PgfLoadStatus::invalid;
    fuc::port::psp::PgfFont parsed;
    if (!parsed.load(bytes)) return PgfLoadStatus::invalid;
    auto [where, inserted] = g_psp_fonts.emplace(font_index, std::move(parsed));
    (void)inserted;
    font = &where->second;
    return PgfLoadStatus::success;
}

bool is_hle_backed_module(const std::string &path) {
    // Fate ships this PRX on its own disc, then imports sceLibFont from it. The
    // import wrappers are already part of the recompiled main ELF; loading the
    // PRX here only makes that HLE-backed library available. Do not broaden
    // this to arbitrary PRXs: an unknown module may contain game logic that has
    // not been recompiled and must keep failing visibly.
    return path == "disc0:/PSP_GAME/USRDIR/data/module/libfont.prx";
}

struct AudioChannelState {
    bool reserved{};
    std::uint32_t sample_count{};
    std::uint32_t format{};
    std::uint64_t busy_until_us{};
};

std::array<AudioChannelState, 8> g_audio_channels{};
AudioPcmHook g_audio_pcm_hook;
std::array<std::uint64_t, 8> g_audio_channel_buffers{};
std::array<std::uint64_t, 8> g_audio_channel_samples{};

// Guest-visible PSP software mixer state. VAG voices are decoded from the
// game's own PSP buffers and mixed into the exact buffer later submitted via
// sceAudio; no replacement or cross-platform audio asset is involved.
enum class SasVoiceType { Off, Vag, Noise };

struct SasVoiceState {
    SasVoiceType type{SasVoiceType::Off};
    bool on{};
    bool playing{};
    bool paused{};
    bool loop{};
    std::uint32_t data_address{};
    std::uint32_t data_size{};
    std::uint64_t remaining_samples{};
    std::int32_t pitch{0x1000};
    std::array<std::int32_t, 4> volumes{};
    std::uint32_t simple_adsr1{};
    std::uint32_t simple_adsr2{};
    std::uint32_t envelope_height{};
    std::vector<std::int16_t> pcm{};
    std::uint64_t sample_cursor_12{};
    std::size_t loop_start_sample{};
    bool loop_start_valid{};
};

struct SasState {
    bool initialized{};
    std::uint32_t core_address{};
    std::uint32_t grain_size{};
    std::uint32_t max_voices{};
    std::uint32_t output_mode{};
    std::uint32_t sample_rate{};
    std::array<SasVoiceState, 32> voices{};
    std::int32_t reverb_type{-1};
    std::int32_t reverb_delay{};
    std::int32_t reverb_feedback{};
    std::uint32_t reverb_left{};
    std::uint32_t reverb_right{};
    bool dry{};
    bool wet{};
};

SasState g_sas{};

struct VagFlowCounters {
    std::uint64_t voices{};
    std::uint64_t source_blocks{};
    std::uint64_t decoded_blocks{};
    std::uint64_t trailing_blocks_skipped{};
    std::array<std::uint64_t, 8> terminal_flags{};
};

VagFlowCounters g_vag_flow{};
std::uint64_t g_sas_trace_events{};

bool sas_trace_enabled() {
    if (std::getenv("PSPRECOMP_SAS_TRACE") == nullptr ||
        g_sas_trace_events >= 5000u)
        return false;
    const char* from = std::getenv("PSPRECOMP_SAS_TRACE_FROM_US");
    const char* until = std::getenv("PSPRECOMP_SAS_TRACE_UNTIL_US");
    const std::uint64_t from_us = from != nullptr
        ? std::strtoull(from, nullptr, 10) : 0u;
    const std::uint64_t until_us = until != nullptr
        ? std::strtoull(until, nullptr, 10)
        : std::numeric_limits<std::uint64_t>::max();
    return g_virtual_time_us >= from_us && g_virtual_time_us <= until_us;
}

template <typename... Args>
void sas_trace(const char* format, Args... args) {
    if (!sas_trace_enabled()) return;
    ++g_sas_trace_events;
    std::printf("[sas] ");
    std::printf(format, args...);
    std::printf("\n");
}

constexpr std::uint32_t kSasInvalidGrain = 0x80420001u;
constexpr std::uint32_t kSasInvalidMaxVoices = 0x80420002u;
constexpr std::uint32_t kSasInvalidOutputMode = 0x80420003u;
constexpr std::uint32_t kSasInvalidSampleRate = 0x80420004u;
constexpr std::uint32_t kSasBadAddress = 0x80420005u;
constexpr std::uint32_t kSasInvalidVoice = 0x80420010u;
constexpr std::uint32_t kSasInvalidNoise = 0x80420011u;
constexpr std::uint32_t kSasInvalidPitch = 0x80420012u;
constexpr std::uint32_t kSasInvalidAdsrMode = 0x80420013u;
constexpr std::uint32_t kSasInvalidParameter = 0x80420014u;
constexpr std::uint32_t kSasInvalidLoop = 0x80420015u;
constexpr std::uint32_t kSasInvalidState = 0x80420016u;
constexpr std::uint32_t kSasInvalidVolume = 0x80420018u;
constexpr std::uint32_t kSasInvalidFxType = 0x80420020u;
constexpr std::uint32_t kSasInvalidFxFeedback = 0x80420021u;
constexpr std::uint32_t kSasInvalidFxDelay = 0x80420022u;
constexpr std::uint32_t kSasInvalidFxVolume = 0x80420023u;
constexpr std::uint32_t kSasNotInitialized = 0x80420100u;
constexpr std::uint32_t kSasEnvelopeMaximum = 0x40000000u;

bool sas_valid_core(std::uint32_t core) {
    return g_sas.initialized && core == g_sas.core_address;
}

SasVoiceState *sas_voice(std::uint32_t core, std::int32_t index,
                         psprecomp::AllegrexContext &ctx) {
    if (!sas_valid_core(core)) {
        ctx.set_gpr(2, kSasNotInitialized);
        return nullptr;
    }
    if (index < 0 || index >= 32) {
        ctx.set_gpr(2, kSasInvalidVoice);
        return nullptr;
    }
    return &g_sas.voices[static_cast<std::size_t>(index)];
}

void sas_advance_grain() {
    for (auto &voice : g_sas.voices) {
        if (!voice.playing || voice.paused) continue;
        voice.envelope_height = voice.on ? kSasEnvelopeMaximum : 0u;
        if (!voice.on) {
            voice.playing = false;
            continue;
        }
        if (voice.type != SasVoiceType::Vag || voice.loop) continue;
        const std::uint64_t consumed =
            std::max<std::uint64_t>(1u,
                (static_cast<std::uint64_t>(g_sas.grain_size) *
                 static_cast<std::uint32_t>(voice.pitch)) / 0x1000u);
        if (consumed >= voice.remaining_samples) {
            voice.remaining_samples = 0u;
            voice.playing = false;
            voice.on = false;
            voice.envelope_height = 0u;
        } else {
            voice.remaining_samples -= consumed;
        }
    }
}

// ATRAC3/ATRAC3+ streams.
//
// NOTHING here decodes audio. The PSP's ATRAC3plus is Sony's proprietary codec
// and this profile does not implement it; what it provides is the bookkeeping
// half of the public contract, so a game that opens a stream gets a real
// handle back and can proceed instead of stalling on a failed open.
//
// The contract implemented is the documented one: sceAtracSetDataAndGetID takes
// a buffer holding a RIFF/WAVE header and returns a NEW ATRAC ID, or a negative
// error. Reporting failure instead would make the game abandon whatever the
// stream backs, and the retry loops this project has already met show what that
// costs.
constexpr std::uint32_t kErrorAtracNoId = 0x80630011u;      // ALL_DATA_LOADED / no free id
constexpr std::uint32_t kErrorAtracBadParam = 0x80630007u;  // invalid parameter
constexpr std::uint32_t kErrorAtracNoLoopInformation = 0x80630021u;
constexpr std::uint32_t kErrorAtracSecondBufferNotNeeded = 0x80630022u;
constexpr std::size_t kMaxAtracIds = 8u;

struct AtracStream {
    bool in_use{};
    std::uint32_t buffer{};
    std::uint32_t size{};
    // Where playback has got to, in samples, and how long the stream is.
    // Without these the decoder has no way to ever finish, and a stream that
    // never finishes is as bad as one that finishes instantly: the first makes
    // the game's bgm thread spin forever, the second makes it exit.
    std::uint32_t position{};
    std::uint32_t total_samples{};
    std::uint32_t loop_start{};
    std::uint32_t loop_end{};
    bool has_loop{};
    // How many times the guest asked for the stream to repeat. -1 means
    // forever, which is what a background track uses.
    std::int32_t loops{};
    std::uint32_t diagnostic_reads{};
    std::filesystem::path source_path{};
    std::uint32_t sample_rate{44100u};
    vcs::AudioStreamDecoder decoder{};
};

// ATRAC3plus decodes 2048 samples per frame, and its frames are 0x800 bytes on
// this platform. Both numbers come from the public format description; they are
// used only to give the stream a plausible length and pace, never to decode.
constexpr std::uint32_t kAtracSamplesPerFrame = 2048u;
constexpr std::uint32_t kAtracFrameBytes = 0x800u;
std::array<AtracStream, kMaxAtracIds> g_atrac{};
// Decoded ATRAC buffers are later submitted to sceAudio in smaller slices.
// Preserve the byte range, not only the base address, so diagnostics identify
// every slice without changing the PCM path itself.
std::map<std::uint32_t, std::uint32_t> g_atrac_pcm_buffers;
std::uint64_t g_atrac_decoded_samples{};
std::uint64_t g_atrac_decoded_nonzero{};
std::uint64_t g_atrac_output_buffers{};
std::uint64_t g_atrac_output_nonzero{};

std::uint16_t atrac_le16(const std::span<const std::uint8_t> bytes,
                         const std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset] |
        (static_cast<std::uint16_t>(bytes[offset + 1u]) << 8u));
}

std::uint32_t atrac_le32(const std::span<const std::uint8_t> bytes,
                         const std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

std::filesystem::path find_atrac_source(
    const std::span<const std::uint8_t> header) {
    if (header.size() < 12u) return {};
    const std::uint64_t declared_size =
        static_cast<std::uint64_t>(atrac_le32(header, 4u)) + 8u;
    if (g_game_data_root.empty()) return {};
    const std::filesystem::path root = g_game_data_root;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
         iterator != end && !error; iterator.increment(error)) {
        if (!iterator->is_regular_file(error)) continue;
        std::string extension = iterator->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](const unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        if (extension != ".at3" && extension != ".aa3" && extension != ".oma")
            continue;
        if (iterator->file_size(error) != declared_size || error) continue;
        std::vector<std::uint8_t> candidate(header.size());
        std::ifstream input(iterator->path(), std::ios::binary);
        input.read(reinterpret_cast<char*>(candidate.data()),
                   static_cast<std::streamsize>(candidate.size()));
        if (input.gcount() == static_cast<std::streamsize>(candidate.size()) &&
            std::equal(candidate.begin(), candidate.end(), header.begin()))
            return std::filesystem::absolute(iterator->path());
    }
    return {};
}

void install_atrac(psprecomp::Runtime &runtime) {
    bind(runtime, "sceAtrac3plus", 0x7A20E7AFu, "sceAtracSetDataAndGetID",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t buffer = ctx.gpr[4];
             const std::uint32_t size = ctx.gpr[5];
             // A stream has to start with a readable RIFF header. Checking it
             // keeps a wild pointer from being handed back a valid-looking id.
             if (buffer == 0u || size < 12u || !rt.memory().contains(buffer, 12u)) {
                 ctx.set_gpr(2, kErrorAtracBadParam);
                 return;
             }
             const std::uint32_t riff = rt.memory().load32(buffer);
             if (riff != 0x46464952u) {  // 'RIFF', little-endian
                 ctx.set_gpr(2, kErrorAtracBadParam);
                 return;
             }
             for (std::size_t index = 0; index < kMaxAtracIds; ++index) {
                 if (g_atrac[index].in_use) continue;
                 AtracStream stream{};
                 stream.in_use = true;
                 stream.buffer = buffer;
                 stream.size = size;
                 stream.position = 0u;
                 // The stream's length comes from the RIFF header the game just
                 // handed over, not from the buffer size.
                 //
                 // Deriving it from the buffer was wrong and the numbers said
                 // so: it gave 39 frames, about 1.8 seconds, for a background
                 // track. The buffer is a STREAMING window that the game refills
                 // as it plays -- which is what sceAtracGetRemainFrame and the
                 // second-buffer calls are for -- so its size says nothing about
                 // how long the music is. The `fact` chunk does, and reading it
                 // is reading a number the guest provided rather than inventing
                 // one.
                 stream.total_samples = 0u;
                 const std::size_t metadata_size = std::min<std::size_t>(size, 4096u);
                 if (const auto* metadata_pointer = rt.memory().raw_pointer(
                         buffer, metadata_size)) {
                     const auto metadata = fuc::port::psp::parse_atrac_riff_metadata(
                         {metadata_pointer, metadata_size});
                     stream.total_samples = metadata.total_samples;
                     stream.loop_start = metadata.loop_start;
                     stream.loop_end = metadata.loop_end;
                     stream.has_loop = metadata.has_loop;
                 }
                 // No `fact` chunk: fall back to the buffer, and say so in the
                 // only way that matters -- the stream will end early rather
                 // than pretend to a length nobody stated.
                 if (stream.total_samples == 0u)
                     stream.total_samples = (size / kAtracFrameBytes) * kAtracSamplesPerFrame;
                 const std::size_t header_size = std::min<std::size_t>(size, 256u);
                 if (const auto* header_pointer = rt.memory().raw_pointer(
                         buffer, header_size)) {
                     const std::span<const std::uint8_t> header(header_pointer,
                                                                header_size);
                     for (std::size_t offset = 12u; offset + 24u <= header.size();) {
                         const std::uint32_t chunk_size = atrac_le32(header, offset + 4u);
                         if (std::memcmp(header.data() + offset, "fmt ", 4u) == 0 &&
                             chunk_size >= 16u) {
                             stream.sample_rate = atrac_le32(header, offset + 12u);
                             break;
                         }
                         const std::uint64_t next = static_cast<std::uint64_t>(offset) +
                             8u + chunk_size + (chunk_size & 1u);
                         if (next > header.size()) break;
                         offset = static_cast<std::size_t>(next);
                     }
                     stream.source_path = find_atrac_source(header);
                     const bool decoder_opened = !stream.source_path.empty() &&
                         stream.decoder.open(stream.source_path,
                                             stream.sample_rate, 2u, 0u);
                     if (std::getenv("PSPRECOMP_ATRAC_DIAG") != nullptr) {
                         std::printf("[atrac-fate] id=%zu declared=%u buffer=%u rate=%u samples=%u loop=%u:%u-%u opened=%u source=%s\n",
                                     index,
                                     header.size() >= 8u ? atrac_le32(header, 4u) + 8u : 0u,
                                     size, stream.sample_rate, stream.total_samples,
                                     stream.has_loop ? 1u : 0u, stream.loop_start,
                                     stream.loop_end, decoder_opened ? 1u : 0u,
                                     stream.source_path.empty()
                                         ? "<sin-coincidencia>"
                                         : stream.source_path.string().c_str());
                     }
                 }
                 g_atrac[index] = std::move(stream);
                 ctx.set_gpr(2, static_cast<std::uint32_t>(index));
                 return;
             }
             ctx.set_gpr(2, kErrorAtracNoId);
         });

    bind(runtime, "sceAtrac3plus", 0x83E85EA0u, "sceAtracGetSecondBufferInfo",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t id = ctx.gpr[4];
             if (id >= kMaxAtracIds || !g_atrac[id].in_use) {
                 ctx.set_gpr(2, kErrorAtracBadParam);
                 return;
             }
             // The whole stream was handed over in one buffer, so there is no
             // second one to describe. The documented answer for that is the
             // NOT_NEEDED error, which is what a game checks for before
             // skipping its second-buffer setup -- not a failure.
             for (const std::uint32_t out : {ctx.gpr[5], ctx.gpr[6]}) {
                 if (out != 0u && rt.memory().contains(out, 4u)) rt.memory().store32(out, 0u);
             }
             ctx.set_gpr(2, kErrorAtracSecondBufferNotNeeded);
         });

    bind(runtime, "sceAtrac3plus", 0xA2BBA8BEu, "sceAtracGetSoundSample",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t id = ctx.gpr[4];
             if (id >= kMaxAtracIds || !g_atrac[id].in_use) {
                 ctx.set_gpr(2, kErrorAtracBadParam);
                 return;
             }
             const std::uint32_t end_sample = ctx.gpr[5];
             const std::uint32_t loop_start = ctx.gpr[6];
             const std::uint32_t loop_end = ctx.gpr[7];
             if (end_sample != 0u && rt.memory().contains(end_sample, 4u))
                 rt.memory().store32(end_sample, g_atrac[id].total_samples);
             if (loop_start != 0u && rt.memory().contains(loop_start, 4u))
                 rt.memory().store32(loop_start, g_atrac[id].has_loop
                     ? g_atrac[id].loop_start : 0xFFFFFFFFu);
             if (loop_end != 0u && rt.memory().contains(loop_end, 4u))
                 rt.memory().store32(loop_end, g_atrac[id].has_loop
                     ? g_atrac[id].loop_end : 0xFFFFFFFFu);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceAtrac3plus", 0x61EB33F5u, "sceAtracReleaseAtracID",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t id = ctx.gpr[4];
             if (id >= kMaxAtracIds || !g_atrac[id].in_use) {
                 ctx.set_gpr(2, kErrorAtracBadParam);
                 return;
             }
             // Freeing the slot matters even without a decoder: there are only
             // eight, and a game that opens and closes streams would otherwise
             // run out and start getting refused.
             g_atrac[id] = AtracStream{};
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceAtrac3plus", 0xFAA4F89Bu, "sceAtracGetLoopStatus",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t id = ctx.gpr[4];
             if (id >= kMaxAtracIds || !g_atrac[id].in_use) {
                 ctx.set_gpr(2, kErrorAtracBadParam);
                 return;
             }
             const std::uint32_t out_loop_num = ctx.gpr[5];
             const std::uint32_t out_loop_status = ctx.gpr[6];
             if (out_loop_num != 0u && rt.memory().contains(out_loop_num, 4u))
                 rt.memory().store32(out_loop_num,
                                     static_cast<std::uint32_t>(g_atrac[id].loops));
             if (out_loop_status != 0u && rt.memory().contains(out_loop_status, 4u))
                 rt.memory().store32(out_loop_status, g_atrac[id].has_loop ? 1u : 0u);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceAtrac3plus", 0x6A8C3CD5u, "sceAtracDecodeData",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t id = ctx.gpr[4];
             if (id >= kMaxAtracIds || !g_atrac[id].in_use) {
                 ctx.set_gpr(2, kErrorAtracBadParam);
                 return;
             }
             AtracStream &stream = g_atrac[id];
             // Decode the original PSP ATRAC file identified from the RIFF
             // header supplied by the guest.  The decoder is the in-process
             // PSPRecomp media backend; no replacement asset or subprocess is
             // involved. If a private source is absent, retain timed silence
             // so the guest's audio thread remains deterministic.
             const std::uint32_t playback_end = stream.loops != 0 && stream.has_loop
                 ? stream.loop_end + 1u : stream.total_samples;
             const std::uint32_t remaining = playback_end > stream.position
                 ? playback_end - stream.position : 0u;
             std::uint32_t samples = remaining < kAtracSamplesPerFrame
                 ? remaining : kAtracSamplesPerFrame;
             const std::uint32_t pcm = ctx.gpr[5];
             if (samples != 0u && pcm != 0u) {
                 // Stereo 16-bit is what this game asks the hardware for.
                 const std::uint32_t bytes = samples * 4u;
                 if (rt.memory().contains(pcm, bytes)) {
                     auto* destination = rt.memory().raw_pointer(pcm, bytes);
                     std::size_t decoded = 0u;
                     if (stream.decoder.is_open())
                         decoded = stream.decoder.read({destination, bytes});
                     if (decoded != 0u) {
                         g_atrac_pcm_buffers[pcm] = pcm + static_cast<std::uint32_t>(decoded);
                         const auto* decoded_pcm =
                             reinterpret_cast<const std::int16_t*>(destination);
                         const std::size_t decoded_samples = decoded / 2u;
                         g_atrac_decoded_samples += decoded_samples;
                         g_atrac_decoded_nonzero += static_cast<std::uint64_t>(
                             std::count_if(decoded_pcm, decoded_pcm + decoded_samples,
                                           [](const std::int16_t sample) {
                                               return sample != 0;
                                           }));
                     }
                     if (std::getenv("PSPRECOMP_ATRAC_DIAG") != nullptr) {
                         if (stream.diagnostic_reads < 8u) {
                             std::printf("[atrac-fate] decode id=%u pcm=%08X requested=%u decoded=%zu pos=%u/%u\n",
                                         id, pcm, bytes, decoded, stream.position,
                                         stream.total_samples);
                             ++stream.diagnostic_reads;
                         }
                     }
                     if (decoded == 0u) {
                         rt.memory().zero(pcm, bytes);
                     } else {
                         samples = static_cast<std::uint32_t>(decoded / 4u);
                         if (decoded < bytes)
                             std::memset(destination + decoded, 0, bytes - decoded);
                     }
                 }
             }
             stream.position += samples;
             bool finished = stream.position >= playback_end;
             if (finished && stream.loops != 0 && stream.has_loop) {
                 // The guest asked for this to repeat, so it repeats. A finite
                 // count is spent one pass at a time; -1 means forever.
                 if (stream.loops > 0) --stream.loops;
                 stream.position = stream.loop_start;
                 stream.decoder.close();
                 if (!stream.source_path.empty())
                     (void)stream.decoder.open(stream.source_path,
                                               stream.sample_rate, 2u,
                                               stream.loop_start);
                 if (std::getenv("PSPRECOMP_ATRAC_DIAG") != nullptr)
                     std::printf("[atrac-fate] restart id=%u loops=%d at=%u\n",
                                 id, stream.loops, stream.loop_start);
                 finished = false;
             }

             const std::uint32_t out_samples_written = ctx.gpr[6];
             const std::uint32_t out_end = ctx.gpr[7];
             if (out_samples_written != 0u && rt.memory().contains(out_samples_written, 4u))
                 rt.memory().store32(out_samples_written, samples);
             if (out_end != 0u && rt.memory().contains(out_end, 4u))
                 rt.memory().store32(out_end, finished ? 1u : 0u);
             // The fifth argument is the remaining-frame count, passed on the
             // stack under the o32 ABI rather than in a register.
             const std::uint32_t out_remain = ctx.gpr[29] + 16u;
             if (rt.memory().contains(out_remain, 4u)) {
                 const std::uint32_t pointer = rt.memory().load32(out_remain);
                 if (pointer != 0u && rt.memory().contains(pointer, 4u))
                     rt.memory().store32(pointer, 0xFFFFFFFFu);
             }
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceAtrac3plus", 0xE23E3A35u, "sceAtracGetNextDecodePosition",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t id = ctx.gpr[4];
             if (id >= kMaxAtracIds || !g_atrac[id].in_use) {
                 ctx.set_gpr(2, kErrorAtracBadParam);
                 return;
             }
             const std::uint32_t out = ctx.gpr[5];
             if (out != 0u && rt.memory().contains(out, 4u))
                 rt.memory().store32(out, g_atrac[id].position);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceAtrac3plus", 0x9AE849A7u, "sceAtracGetRemainFrame",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t id = ctx.gpr[4];
             if (id >= kMaxAtracIds || !g_atrac[id].in_use) {
                 ctx.set_gpr(2, kErrorAtracBadParam);
                 return;
             }
             // -1 is the documented "all data is already on memory", and here it
             // is simply true: sceAtracSetDataAndGetID was handed the whole
             // buffer, so there is nothing left to stream in.
             const std::uint32_t out = ctx.gpr[5];
             if (out != 0u && rt.memory().contains(out, 4u))
                 rt.memory().store32(out, 0xFFFFFFFFu);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceAtrac3plus", 0x868120B5u, "sceAtracSetLoopNum",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t id = ctx.gpr[4];
             if (id >= kMaxAtracIds || !g_atrac[id].in_use) {
                 ctx.set_gpr(2, kErrorAtracBadParam);
                 return;
             }
             g_atrac[id].loops = static_cast<std::int32_t>(ctx.gpr[5]);
             if (std::getenv("PSPRECOMP_ATRAC_DIAG") != nullptr)
                 std::printf("[atrac-fate] set-loop id=%u loops=%d has-loop=%u range=%u-%u\n",
                             id, g_atrac[id].loops, g_atrac[id].has_loop ? 1u : 0u,
                             g_atrac[id].loop_start, g_atrac[id].loop_end);
             ctx.set_gpr(2, g_atrac[id].has_loop ? 0u
                                                 : kErrorAtracNoLoopInformation);
         });
}

// PSP Movie Format / sceMpeg.
//
// Fate uses the firmware YCbCr path: decode one AVC picture, then call
// sceMpegAvcCsc several times to copy 128-pixel-wide crops into GE texture
// buffers.  The decoder below keeps the decoded picture as host-side HLE state
// (the YCbCr allocation is opaque to the game) and each CSC call writes the
// exact crop to guest memory in the pixel format selected by the PSP guest.
struct MpegStream {
    std::uint32_t type{};
    std::uint32_t number{};
    bool needs_reset{true};
};

struct MpegContext {
    std::uint32_t handle_address{};
    std::uint32_t ring_address{};
    fuc::port::psp::PsmfHeader header{};
    std::unordered_map<std::uint32_t, MpegStream> streams;
    std::array<bool, 2u> avc_es_buffers{};
    std::uint32_t pixel_mode{3u};
    std::uint32_t video_au_count{};
    std::uint32_t audio_au_count{};
    std::uint32_t decoded_video_frames{};
    std::uint32_t consumed_video_packets{};
    std::filesystem::path source_path;
    vcs::VideoStreamDecoder video;
    vcs::PmfAudioDecoder audio;
    std::filesystem::path audio_source;
    std::vector<std::uint8_t> current_rgba;
    bool analyzed{};
};

struct MpegCallbackFrame {
    psprecomp::AllegrexContext resume{};
    std::uint32_t ring{};
    std::int32_t remaining{};
    std::int32_t requested_this_round{};
    std::int32_t total{};
};

std::unordered_map<std::uint32_t, MpegContext> g_mpeg;
std::unordered_map<std::int32_t, MpegCallbackFrame> g_mpeg_callbacks;
std::uint32_t g_next_mpeg_stream_id{1u};
std::uint64_t g_mpeg_video_frames{};
std::uint64_t g_mpeg_audio_bytes{};
std::uint64_t g_mpeg_csc_crops{};

void write_mpeg_timestamp(psprecomp::Runtime& rt, const std::uint32_t address,
                          const std::uint64_t value) {
    rt.memory().store32(address, static_cast<std::uint32_t>(value >> 32u));
    rt.memory().store32(address + 4u, static_cast<std::uint32_t>(value));
}

std::filesystem::path find_pmf_source(
    const std::span<const std::uint8_t> header,
    const fuc::port::psp::PsmfHeader& parsed) {
    const std::uint64_t expected_size =
        static_cast<std::uint64_t>(parsed.stream_offset) + parsed.stream_size;
    if (g_game_data_root.empty()) return {};
    const std::filesystem::path root = g_game_data_root;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
         iterator != end && !error; iterator.increment(error)) {
        if (!iterator->is_regular_file(error)) continue;
        std::string extension = iterator->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](const unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        if (extension != ".pmf" || iterator->file_size(error) != expected_size || error)
            continue;
        std::vector<std::uint8_t> candidate(header.size());
        std::ifstream input(iterator->path(), std::ios::binary);
        input.read(reinterpret_cast<char*>(candidate.data()),
                   static_cast<std::streamsize>(candidate.size()));
        if (input.gcount() == static_cast<std::streamsize>(candidate.size()) &&
            std::equal(candidate.begin(), candidate.end(), header.begin())) {
            return iterator->path();
        }
    }
    return {};
}

void reset_mpeg_video(MpegContext& state) {
    state.video.close();
    state.audio.close();
    state.audio_source.clear();
    state.current_rgba.clear();
    state.decoded_video_frames = 0u;
    state.consumed_video_packets = 0u;
}

bool read_mpeg_video_frame(MpegContext& state) {
    if (!state.video.is_open()) {
        if (state.source_path.empty() || !state.video.open(state.source_path)) return false;
    }
    const std::size_t bytes = static_cast<std::size_t>(state.header.width) *
                              state.header.height * 4u;
    state.current_rgba.resize(bytes);
    if (state.video.read(state.current_rgba) < bytes) {
        state.current_rgba.clear();
        return false;
    }
    ++state.decoded_video_frames;
    ++g_mpeg_video_frames;
    return true;
}

void delay_mpeg_result(psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx,
                       const std::uint32_t microseconds,
                       const std::uint32_t result) {
    if (g_table.ready.empty()) {
        g_virtual_time_us += microseconds;
        ctx.set_gpr(2, result);
        return;
    }
    psprecomp::AllegrexContext sleeping = ctx;
    sleeping.pc = ctx.gpr[31];
    sleeping.set_gpr(2, result);
    g_table.ready.push_back({g_table.current_uid, sleeping,
                             g_virtual_time_us + microseconds});
    (void)activate_next(rt, ctx, "mpeg-delay");
}

void start_mpeg_callback(psprecomp::AllegrexContext& ctx,
                         MpegCallbackFrame& frame,
                         const std::uint32_t data,
                         const std::uint32_t callback,
                         const std::uint32_t argument,
                         const std::int32_t packets) {
    frame.requested_this_round = packets;
    ctx.set_gpr(4, data);
    ctx.set_gpr(5, static_cast<std::uint32_t>(packets));
    ctx.set_gpr(6, argument);
    ctx.set_gpr(31, 4u);
    ctx.pc = callback;
}

void mpeg_callback_return(psprecomp::Runtime& rt,
                          psprecomp::AllegrexContext& ctx) {
    const auto found = g_mpeg_callbacks.find(g_table.current_uid);
    if (found == g_mpeg_callbacks.end()) {
        rt.stop("retorno MPEG sin callback pendiente");
        return;
    }
    MpegCallbackFrame& frame = found->second;
    if (!rt.memory().contains(frame.ring, 48u)) {
        rt.stop("callback MPEG retorno a ringbuffer invalido");
        return;
    }
    const auto callback_result = static_cast<std::int32_t>(ctx.gpr[2]);
    const auto packets = static_cast<std::int32_t>(rt.memory().load32(frame.ring));
    auto write_position = static_cast<std::int32_t>(rt.memory().load32(frame.ring + 8u));
    auto used = static_cast<std::int32_t>(rt.memory().load32(frame.ring + 12u));
    if (packets <= 0) {
        rt.stop("callback MPEG retorno a ringbuffer sin paquetes");
        return;
    }
    if (callback_result > 0) {
        const std::int32_t accepted = std::min(
            {callback_result, frame.requested_this_round, std::max(0, packets - used)});
        frame.total += accepted;
        write_position += accepted;
        used += accepted;
        rt.memory().store32(frame.ring + 4u,
                            rt.memory().load32(frame.ring + 4u) + accepted);
        rt.memory().store32(frame.ring + 8u, static_cast<std::uint32_t>(write_position));
        rt.memory().store32(frame.ring + 12u, static_cast<std::uint32_t>(used));
    }
    if (callback_result > 0 && frame.remaining > 0 && used < packets) {
        const std::int32_t write_offset = write_position % packets;
        const std::int32_t desired = std::min(
            {frame.remaining, packets - write_offset, packets - used});
        if (desired > 0) {
            frame.remaining -= desired;
            ctx = frame.resume;
            start_mpeg_callback(ctx, frame,
                rt.memory().load32(frame.ring + 20u) +
                    static_cast<std::uint32_t>(write_offset) * 2048u,
                rt.memory().load32(frame.ring + 24u),
                rt.memory().load32(frame.ring + 28u), desired);
            return;
        }
    }
    ctx = frame.resume;
    ctx.set_gpr(2, callback_result < 0 && frame.total == 0
                       ? static_cast<std::uint32_t>(callback_result)
                       : static_cast<std::uint32_t>(frame.total));
    g_mpeg_callbacks.erase(found);
}

std::uint16_t pack_movie_pixel(const std::uint8_t r, const std::uint8_t g,
                               const std::uint8_t b,
                               const std::uint32_t mode) {
    switch (mode) {
    case 0u:
        return static_cast<std::uint16_t>((r >> 3u) | ((g >> 2u) << 5u) |
                                          ((b >> 3u) << 11u));
    case 1u:
        return static_cast<std::uint16_t>((r >> 3u) | ((g >> 3u) << 5u) |
                                          ((b >> 3u) << 10u));
    default:
        return static_cast<std::uint16_t>((r >> 4u) | ((g >> 4u) << 4u) |
                                          ((b >> 4u) << 8u));
    }
}

void consume_mpeg_video_packets(psprecomp::Runtime& rt, MpegContext& state) {
    const std::uint64_t duration = state.header.last_timestamp - state.header.first_timestamp;
    const std::uint32_t frames = std::max<std::uint32_t>(
        1u, static_cast<std::uint32_t>(duration / 3003u));
    const std::uint32_t packets = (state.header.stream_size + 2047u) / 2048u;
    const std::uint32_t target = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        packets, static_cast<std::uint64_t>(state.decoded_video_frames) * packets / frames));
    const std::uint32_t consumed = target - state.consumed_video_packets;
    state.consumed_video_packets = target;
    if (consumed != 0u && rt.memory().contains(state.ring_address, 48u)) {
        const std::uint32_t used = rt.memory().load32(state.ring_address + 12u);
        rt.memory().store32(state.ring_address + 12u,
                            used > consumed ? used - consumed : 0u);
    }
}

void install_mpeg(psprecomp::Runtime& runtime) {
    bind_success(runtime, "sceMpeg", 0x682A619Bu, "sceMpegInit");
    bind_success(runtime, "sceMpeg", 0x874624D6u, "sceMpegFinish");

    bind(runtime, "sceMpeg", 0xD7A29F46u, "sceMpegRingbufferQueryMemSize",
         [](psprecomp::Runtime&, psprecomp::AllegrexContext& ctx) {
             const auto packets = static_cast<std::int32_t>(ctx.gpr[4]);
             ctx.set_gpr(2, packets < 0 ? 0x80610103u
                                        : static_cast<std::uint32_t>(packets) * 2152u);
         });
    bind(runtime, "sceMpeg", 0xC132E22Fu, "sceMpegQueryMemSize",
         [](psprecomp::Runtime&, psprecomp::AllegrexContext& ctx) {
             ctx.set_gpr(2, 0x10000u);
         });
    bind(runtime, "sceMpeg", 0x37295ED8u, "sceMpegRingbufferConstruct",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const std::uint32_t ring = ctx.gpr[4];
             const auto packets = static_cast<std::int32_t>(ctx.gpr[5]);
             const std::uint32_t data = ctx.gpr[6];
             const std::uint32_t size = ctx.gpr[7];
             if (packets < 0 || !rt.memory().contains(ring, 48u) ||
                 static_cast<std::uint64_t>(packets) * 2152u > size ||
                 !rt.memory().contains(data, static_cast<std::size_t>(packets) * 2048u)) {
                 ctx.set_gpr(2, 0x80610103u);
                 return;
             }
             rt.memory().zero(ring, 48u);
             rt.memory().store32(ring, static_cast<std::uint32_t>(packets));
             rt.memory().store32(ring + 16u, 2048u);
             rt.memory().store32(ring + 20u, data);
             rt.memory().store32(ring + 24u, ctx.gpr[8]);
             rt.memory().store32(ring + 28u, ctx.gpr[9]);
             rt.memory().store32(ring + 32u,
                                 data + static_cast<std::uint32_t>(packets) * 2048u);
             rt.memory().store32(ring + 44u, ctx.gpr[28]);
             ctx.set_gpr(2, 0u);
         });
    bind_success(runtime, "sceMpeg", 0x13407F13u, "sceMpegRingbufferDestruct");

    bind(runtime, "sceMpeg", 0xD8C5F121u, "sceMpegCreate",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const std::uint32_t mpeg = ctx.gpr[4];
             const std::uint32_t data = ctx.gpr[5];
             const std::uint32_t size = ctx.gpr[6];
             const std::uint32_t ring = ctx.gpr[7];
             if (size < 0x10000u || !rt.memory().contains(mpeg, 4u) ||
                 !rt.memory().contains(data, size) || !rt.memory().contains(ring, 48u)) {
                 ctx.set_gpr(2, 0x80610103u);
                 return;
             }
             const std::uint32_t handle = data + 0x30u;
             if (!rt.memory().contains(handle, 24u)) {
                 ctx.set_gpr(2, kErrorIllegalAddress);
                 return;
             }
             constexpr std::array<std::uint8_t, 8u> magic{'L','I','B','M','P','E','G',0};
             constexpr std::array<std::uint8_t, 4u> version{'0','0','1',0};
             rt.memory().store32(mpeg, handle);
             rt.memory().copy_in(handle, magic);
             rt.memory().copy_in(handle + 8u, version);
             rt.memory().store32(handle + 12u, 0xFFFFFFFFu);
             rt.memory().store32(handle + 16u, ring);
             rt.memory().store32(handle + 20u, rt.memory().load32(ring + 32u));
             rt.memory().store32(ring + 40u, mpeg);
             MpegContext state{};
             state.handle_address = handle;
             state.ring_address = ring;
             g_mpeg.insert_or_assign(mpeg, std::move(state));
             delay_mpeg_result(rt, ctx, 29000u, 0u);
         });
    bind(runtime, "sceMpeg", 0x606A4649u, "sceMpegDelete",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             if (!rt.memory().contains(ctx.gpr[4], 4u)) {
                 ctx.set_gpr(2, kErrorIllegalAddress);
                 return;
             }
             g_mpeg.erase(ctx.gpr[4]);
             delay_mpeg_result(rt, ctx, 40000u, 0u);
         });

    bind(runtime, "sceMpeg", 0x21FF80E4u, "sceMpegQueryStreamOffset",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const auto found = g_mpeg.find(ctx.gpr[4]);
             if (found == g_mpeg.end() || !rt.memory().contains(ctx.gpr[5], 2048u) ||
                 !rt.memory().contains(ctx.gpr[6], 4u)) {
                 ctx.set_gpr(2, 0x80610103u);
                 return;
             }
             std::array<std::uint8_t, 2048u> bytes{};
             rt.memory().copy_out(ctx.gpr[5], bytes);
             fuc::port::psp::PsmfHeader header{};
             if (!fuc::port::psp::parse_psmf_header(bytes, header) ||
                 (header.stream_offset & 2047u) != 0u) {
                 rt.memory().store32(ctx.gpr[6], 0u);
                 ctx.set_gpr(2, 0x806101FEu);
                 return;
             }
             const auto source = find_pmf_source(bytes, header);
             MpegContext& state = found->second;
             if (source != state.source_path) {
                 reset_mpeg_video(state);
                 state.source_path = source;
                 state.video_au_count = 0u;
                 state.audio_au_count = 0u;
             }
             state.header = header;
             state.analyzed = true;
             rt.memory().store32(ctx.gpr[6], header.stream_offset);
             if (std::getenv("PSPRECOMP_MPEG_DIAG") != nullptr) {
                 std::printf("[mpeg-fate] PSMF %ux%u offset=%u size=%u pts=%llu..%llu source=%s\n",
                     header.width, header.height, header.stream_offset, header.stream_size,
                     static_cast<unsigned long long>(header.first_timestamp),
                     static_cast<unsigned long long>(header.last_timestamp),
                     source.empty() ? "<sin-coincidencia>" : source.string().c_str());
             }
             ctx.set_gpr(2, 0u);
         });
    bind(runtime, "sceMpeg", 0x611E9E11u, "sceMpegQueryStreamSize",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             if (!rt.memory().contains(ctx.gpr[4], 2048u) ||
                 !rt.memory().contains(ctx.gpr[5], 4u)) {
                 ctx.set_gpr(2, 0x80610103u);
                 return;
             }
             std::array<std::uint8_t, 2048u> bytes{};
             rt.memory().copy_out(ctx.gpr[4], bytes);
             fuc::port::psp::PsmfHeader header{};
             if (!fuc::port::psp::parse_psmf_header(bytes, header)) {
                 rt.memory().store32(ctx.gpr[5], 0u);
                 ctx.set_gpr(2, 0x806101FEu);
                 return;
             }
             rt.memory().store32(ctx.gpr[5], header.stream_size);
             ctx.set_gpr(2, 0u);
         });
    bind(runtime, "sceMpeg", 0x42560F23u, "sceMpegRegistStream",
         [](psprecomp::Runtime&, psprecomp::AllegrexContext& ctx) {
             const auto found = g_mpeg.find(ctx.gpr[4]);
             if (found == g_mpeg.end()) { ctx.set_gpr(2, 0x806101FEu); return; }
             const std::uint32_t id = g_next_mpeg_stream_id++;
             found->second.streams.emplace(id, MpegStream{ctx.gpr[5], ctx.gpr[6], true});
             ctx.set_gpr(2, id);
         });
    bind(runtime, "sceMpeg", 0x591A4AA2u, "sceMpegUnRegistStream",
         [](psprecomp::Runtime&, psprecomp::AllegrexContext& ctx) {
             const auto found = g_mpeg.find(ctx.gpr[4]);
             ctx.set_gpr(2, found != g_mpeg.end() && found->second.streams.erase(ctx.gpr[5]) == 1u
                                ? 0u : 0x806101FEu);
         });

    bind(runtime, "sceMpeg", 0xA780CF7Eu, "sceMpegMallocAvcEsBuf",
         [](psprecomp::Runtime&, psprecomp::AllegrexContext& ctx) {
             const auto found = g_mpeg.find(ctx.gpr[4]);
             if (found == g_mpeg.end()) { ctx.set_gpr(2, 0x806101FEu); return; }
             for (std::size_t index = 0; index < found->second.avc_es_buffers.size(); ++index) {
                 if (found->second.avc_es_buffers[index]) continue;
                 found->second.avc_es_buffers[index] = true;
                 ctx.set_gpr(2, static_cast<std::uint32_t>(index + 1u));
                 return;
             }
             ctx.set_gpr(2, 0u);
         });
    bind(runtime, "sceMpeg", 0xCEB870B1u, "sceMpegFreeAvcEsBuf",
         [](psprecomp::Runtime&, psprecomp::AllegrexContext& ctx) {
             const auto found = g_mpeg.find(ctx.gpr[4]);
             const std::uint32_t buffer = ctx.gpr[5];
             if (found == g_mpeg.end() || buffer == 0u || buffer > 2u ||
                 !found->second.avc_es_buffers[buffer - 1u]) {
                 ctx.set_gpr(2, 0x806101FEu);
                 return;
             }
             found->second.avc_es_buffers[buffer - 1u] = false;
             ctx.set_gpr(2, 0u);
         });
    bind(runtime, "sceMpeg", 0x167AFD9Eu, "sceMpegInitAu",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const auto found = g_mpeg.find(ctx.gpr[4]);
             if (found == g_mpeg.end() || !rt.memory().contains(ctx.gpr[6], 24u)) {
                 ctx.set_gpr(2, 0x80610103u);
                 return;
             }
             const std::uint32_t buffer = ctx.gpr[5];
             const bool avc = buffer >= 1u && buffer <= 2u &&
                              found->second.avc_es_buffers[buffer - 1u];
             rt.memory().zero(ctx.gpr[6], 24u);
             if (!avc) {
                 rt.memory().store32(ctx.gpr[6] + 8u, 0xFFFFFFFFu);
                 rt.memory().store32(ctx.gpr[6] + 12u, 0xFFFFFFFFu);
             }
             rt.memory().store32(ctx.gpr[6] + 20u, avc ? 2048u : 2112u);
             ctx.set_gpr(2, 0u);
         });
    bind(runtime, "sceMpeg", 0xF8DCB679u, "sceMpegQueryAtracEsSize",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             if (g_mpeg.find(ctx.gpr[4]) == g_mpeg.end() ||
                 !rt.memory().contains(ctx.gpr[5], 4u) ||
                 !rt.memory().contains(ctx.gpr[6], 4u)) {
                 ctx.set_gpr(2, 0x80610103u);
                 return;
             }
             rt.memory().store32(ctx.gpr[5], 2112u);
             rt.memory().store32(ctx.gpr[6], 8192u);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceMpeg", 0xFE246728u, "sceMpegGetAvcAu",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const auto found = g_mpeg.find(ctx.gpr[4]);
             if (found == g_mpeg.end()) { ctx.set_gpr(2, 0x806101FEu); return; }
             const auto stream = found->second.streams.find(ctx.gpr[5]);
             if (stream == found->second.streams.end() || stream->second.type != 0u ||
                 !rt.memory().contains(ctx.gpr[6], 24u)) {
                 ctx.set_gpr(2, 0x806101FEu);
                 return;
             }
             const std::uint32_t ring = found->second.ring_address;
             if (!rt.memory().contains(ring, 48u) || rt.memory().load32(ring + 12u) == 0u) {
                 write_mpeg_timestamp(rt, ctx.gpr[6], 0u);
                 write_mpeg_timestamp(rt, ctx.gpr[6] + 8u, 0u);
                 ctx.set_gpr(2, 0x80618001u);
                 return;
             }
             const std::uint64_t pts = found->second.header.first_timestamp +
                 static_cast<std::uint64_t>(found->second.video_au_count) * 3003u;
             write_mpeg_timestamp(rt, ctx.gpr[6], pts);
             write_mpeg_timestamp(rt, ctx.gpr[6] + 8u, pts >= 3003u ? pts - 3003u : 0u);
             rt.memory().store32(ctx.gpr[6] + 16u, stream->second.number);
             rt.memory().store32(ctx.gpr[6] + 20u, 2048u);
             if (ctx.gpr[7] != 0u && rt.memory().contains(ctx.gpr[7], 4u))
                 rt.memory().store32(ctx.gpr[7], 1u);
             stream->second.needs_reset = false;
             ++found->second.video_au_count;
             ctx.set_gpr(2, 0u);
         });
    bind(runtime, "sceMpeg", 0xE1CE83A7u, "sceMpegGetAtracAu",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const auto found = g_mpeg.find(ctx.gpr[4]);
             if (found == g_mpeg.end()) { ctx.set_gpr(2, 0x806101FEu); return; }
             const auto stream = found->second.streams.find(ctx.gpr[5]);
             if (stream == found->second.streams.end() ||
                 (stream->second.type != 1u && stream->second.type != 15u) ||
                 !rt.memory().contains(ctx.gpr[6], 24u)) {
                 ctx.set_gpr(2, 0x806101FEu);
                 return;
             }
             const std::uint32_t ring = found->second.ring_address;
             if (!rt.memory().contains(ring, 48u) || rt.memory().load32(ring + 12u) == 0u) {
                 ctx.set_gpr(2, 0x80618001u);
                 return;
             }
             const std::uint64_t pts = found->second.header.first_timestamp +
                 static_cast<std::uint64_t>(found->second.audio_au_count) * 4180u;
             write_mpeg_timestamp(rt, ctx.gpr[6], pts);
             write_mpeg_timestamp(rt, ctx.gpr[6] + 8u, pts);
             rt.memory().store32(ctx.gpr[6] + 16u, stream->second.number);
             rt.memory().store32(ctx.gpr[6] + 20u, 2112u);
             if (ctx.gpr[7] != 0u && rt.memory().contains(ctx.gpr[7], 4u))
                 rt.memory().store32(ctx.gpr[7], 0u);
             stream->second.needs_reset = false;
             ++found->second.audio_au_count;
             ctx.set_gpr(2, 0u);
         });
    bind(runtime, "sceMpeg", 0x800C44DFu, "sceMpegAtracDecode",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const auto found = g_mpeg.find(ctx.gpr[4]);
             if (found == g_mpeg.end() || !rt.memory().contains(ctx.gpr[5], 24u) ||
                 !rt.memory().contains(ctx.gpr[6], 8192u)) {
                 ctx.set_gpr(2, 0x80610103u);
                 return;
             }
             MpegContext& state = found->second;
             if (!state.source_path.empty() &&
                 (!state.audio.is_open() || state.audio_source != state.source_path)) {
                 state.audio_source = state.source_path;
                 (void)state.audio.open(state.source_path);
             }
             std::array<std::uint8_t, 8192u> pcm{};
             const std::size_t decoded = state.audio.is_open() ? state.audio.read(pcm) : 0u;
             if (decoded != 0u) rt.memory().copy_in(ctx.gpr[6], pcm);
             else rt.memory().zero(ctx.gpr[6], pcm.size());
             g_mpeg_audio_bytes += decoded;
             const std::uint64_t pts = state.header.first_timestamp +
                 static_cast<std::uint64_t>(state.audio_au_count) * 4180u;
             write_mpeg_timestamp(rt, ctx.gpr[5], pts);
             delay_mpeg_result(rt, ctx, 3000u, 0u);
         });

    bind(runtime, "sceMpeg", 0xA11C7026u, "sceMpegAvcDecodeMode",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const auto found = g_mpeg.find(ctx.gpr[4]);
             if (found == g_mpeg.end() || !rt.memory().contains(ctx.gpr[5], 8u)) {
                 ctx.set_gpr(2, 0x80610103u);
                 return;
             }
             const std::uint32_t mode = rt.memory().load32(ctx.gpr[5] + 4u);
             if (mode > 3u) { ctx.set_gpr(2, 0x806101FEu); return; }
             found->second.pixel_mode = mode;
             ctx.set_gpr(2, 0u);
         });
    bind(runtime, "sceMpeg", 0x211A057Cu, "sceMpegAvcQueryYCbCrSize",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             if (g_mpeg.find(ctx.gpr[4]) == g_mpeg.end() ||
                 (ctx.gpr[6] & 15u) != 0u || (ctx.gpr[7] & 15u) != 0u ||
                 ctx.gpr[6] > 480u || ctx.gpr[7] > 272u ||
                 !rt.memory().contains(ctx.gpr[8], 4u)) {
                 ctx.set_gpr(2, 0x806101FEu);
                 return;
             }
             const std::uint32_t size = (ctx.gpr[6] / 2u) * (ctx.gpr[7] / 2u) * 6u + 128u;
             rt.memory().store32(ctx.gpr[8], size);
             ctx.set_gpr(2, 0u);
         });
    bind(runtime, "sceMpeg", 0x67179B1Bu, "sceMpegAvcInitYCbCr",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const std::uint32_t size = (ctx.gpr[6] / 2u) * (ctx.gpr[7] / 2u) * 6u + 128u;
             const bool valid = g_mpeg.find(ctx.gpr[4]) != g_mpeg.end() &&
                 (ctx.gpr[6] & 15u) == 0u && (ctx.gpr[7] & 15u) == 0u &&
                 ctx.gpr[6] <= 480u && ctx.gpr[7] <= 272u &&
                 rt.memory().contains(ctx.gpr[8], size);
             ctx.set_gpr(2, valid ? 0u : 0x806101FEu);
         });
    bind(runtime, "sceMpeg", 0xF0EB1125u, "sceMpegAvcDecodeYCbCr",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const auto found = g_mpeg.find(ctx.gpr[4]);
             if (found == g_mpeg.end() || !found->second.analyzed ||
                 !rt.memory().contains(ctx.gpr[5], 24u) ||
                 !rt.memory().contains(ctx.gpr[6], 4u) ||
                 !rt.memory().contains(ctx.gpr[7], 4u)) {
                 ctx.set_gpr(2, 0x80610103u);
                 return;
             }
             const std::uint32_t opaque = rt.memory().load32(ctx.gpr[6]);
             const std::uint32_t opaque_size =
                 (found->second.header.width / 2u) * (found->second.header.height / 2u) * 6u + 128u;
             if (opaque == 0u || !rt.memory().contains(opaque, opaque_size)) {
                 ctx.set_gpr(2, 0x80610103u);
                 return;
             }
             if (!read_mpeg_video_frame(found->second)) {
                 rt.memory().store32(ctx.gpr[7], 0u);
                 ctx.set_gpr(2, 0x80628002u);
                 return;
             }
             consume_mpeg_video_packets(rt, found->second);
             rt.memory().store32(ctx.gpr[7], 1u);
             delay_mpeg_result(rt, ctx,
                               found->second.decoded_video_frames <= 1u ? 3600u : 5400u,
                               0u);
         });
    bind(runtime, "sceMpeg", 0xF2930C9Cu, "sceMpegAvcDecodeStopYCbCr",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             if (g_mpeg.find(ctx.gpr[4]) == g_mpeg.end() ||
                 !rt.memory().contains(ctx.gpr[5], 4u) ||
                 !rt.memory().contains(ctx.gpr[6], 4u)) {
                 ctx.set_gpr(2, 0x80610103u);
                 return;
             }
             rt.memory().store32(ctx.gpr[6], 0u);
             ctx.set_gpr(2, 0u);
         });
    bind(runtime, "sceMpeg", 0x31BD0272u, "sceMpegAvcCsc",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const auto found = g_mpeg.find(ctx.gpr[4]);
             if (found == g_mpeg.end() || found->second.current_rgba.empty() ||
                 !rt.memory().contains(ctx.gpr[6], 16u)) {
                 ctx.set_gpr(2, 0x80610103u);
                 return;
             }
             MpegContext& state = found->second;
             const std::uint32_t x = rt.memory().load32(ctx.gpr[6]);
             const std::uint32_t y = rt.memory().load32(ctx.gpr[6] + 4u);
             std::uint32_t width = rt.memory().load32(ctx.gpr[6] + 8u);
             std::uint32_t height = rt.memory().load32(ctx.gpr[6] + 12u);
             const std::uint32_t frame_width = ctx.gpr[7] == 0u ? state.header.width : ctx.gpr[7];
             if (x >= state.header.width || y >= state.header.height || frame_width > 2048u) {
                 ctx.set_gpr(2, 0x806101FEu);
                 return;
             }
             width = std::min(width, state.header.width - x);
             height = std::min(height, state.header.height - y);
             const std::uint32_t bytes_per_pixel = state.pixel_mode == 3u ? 4u : 2u;
             const std::size_t output_size = static_cast<std::size_t>(frame_width) *
                                             height * bytes_per_pixel;
             if (width > frame_width || !rt.memory().contains(ctx.gpr[8], output_size)) {
                 ctx.set_gpr(2, 0x80610103u);
                 return;
             }
             for (std::uint32_t row = 0u; row < height; ++row) {
                 const std::uint8_t* source = state.current_rgba.data() +
                     (static_cast<std::size_t>(y + row) * state.header.width + x) * 4u;
                 const std::uint32_t destination = ctx.gpr[8] + row * frame_width * bytes_per_pixel;
                 if (state.pixel_mode == 3u) {
                     std::vector<std::uint8_t> line(static_cast<std::size_t>(width) * 4u);
                     for (std::uint32_t column = 0u; column < width; ++column) {
                         line[column * 4u] = source[column * 4u];
                         line[column * 4u + 1u] = source[column * 4u + 1u];
                         line[column * 4u + 2u] = source[column * 4u + 2u];
                         line[column * 4u + 3u] = 0u;
                     }
                     rt.memory().copy_in(destination, line);
                 } else {
                     std::vector<std::uint8_t> line(static_cast<std::size_t>(width) * 2u);
                     for (std::uint32_t column = 0u; column < width; ++column) {
                         const std::uint16_t packed = pack_movie_pixel(
                             source[column * 4u], source[column * 4u + 1u],
                             source[column * 4u + 2u], state.pixel_mode);
                         line[column * 2u] = static_cast<std::uint8_t>(packed);
                         line[column * 2u + 1u] = static_cast<std::uint8_t>(packed >> 8u);
                     }
                     rt.memory().copy_in(destination, line);
                 }
             }
             ++g_mpeg_csc_crops;
             delay_mpeg_result(rt, ctx, 4000u, 0u);
         });
    bind(runtime, "sceMpeg", 0x4571CC64u, "sceMpegAvcDecodeFlush",
         [](psprecomp::Runtime&, psprecomp::AllegrexContext& ctx) {
             const auto found = g_mpeg.find(ctx.gpr[4]);
             if (found == g_mpeg.end()) { ctx.set_gpr(2, 0x806101FEu); return; }
             found->second.current_rgba.clear();
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceMpeg", 0xB5F6DC87u, "sceMpegRingbufferAvailableSize",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             if (!rt.memory().contains(ctx.gpr[4], 48u)) {
                 ctx.set_gpr(2, kErrorIllegalAddress);
                 return;
             }
             const auto packets = static_cast<std::int32_t>(rt.memory().load32(ctx.gpr[4]));
             const auto used = static_cast<std::int32_t>(rt.memory().load32(ctx.gpr[4] + 12u));
             ctx.set_gpr(2, static_cast<std::uint32_t>(std::max(0, packets - used)));
         });
    bind(runtime, "sceMpeg", 0xB240A59Eu, "sceMpegRingbufferPut",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const std::uint32_t ring = ctx.gpr[4];
             if (!rt.memory().contains(ring, 48u)) {
                 ctx.set_gpr(2, kErrorIllegalAddress);
                 return;
             }
             const auto packets = static_cast<std::int32_t>(rt.memory().load32(ring));
             const auto used = static_cast<std::int32_t>(rt.memory().load32(ring + 12u));
             auto requested = static_cast<std::int32_t>(ctx.gpr[5]);
             requested = std::min({requested, static_cast<std::int32_t>(ctx.gpr[6]),
                                   std::max(0, packets - used)});
             const std::uint32_t callback = rt.memory().load32(ring + 24u);
             if (requested <= 0) { ctx.set_gpr(2, 0u); return; }
             if (packets <= 0 || callback == 0u || g_mpeg_callbacks.contains(g_table.current_uid)) {
                 ctx.set_gpr(2, 0x806101FEu);
                 return;
             }
             const auto write_position = static_cast<std::int32_t>(rt.memory().load32(ring + 8u));
             const std::int32_t write_offset = write_position % packets;
             const std::int32_t desired = std::min(requested, packets - write_offset);
             MpegCallbackFrame frame{};
             frame.resume = ctx;
             frame.resume.pc = ctx.gpr[31];
             frame.ring = ring;
             frame.remaining = requested - desired;
             const auto [where, inserted] = g_mpeg_callbacks.emplace(g_table.current_uid,
                                                                      std::move(frame));
             (void)inserted;
             start_mpeg_callback(ctx, where->second,
                 rt.memory().load32(ring + 20u) +
                     static_cast<std::uint32_t>(write_offset) * 2048u,
                 callback, rt.memory().load32(ring + 28u), desired);
         });
}

void font_newlib_callback_return(psprecomp::Runtime& rt,
                                 psprecomp::AllegrexContext& ctx) {
    const auto found = g_font_newlib_pending.find(g_table.current_uid);
    if (found == g_font_newlib_pending.end()) {
        rt.stop("retorno de alloc sceFontNewLib sin llamada pendiente");
        return;
    }
    FontNewLibPending pending = found->second;
    g_font_newlib_pending.erase(found);
    const std::uint32_t allocated = ctx.gpr[2];
    ctx = pending.resume;

    if (allocated == 0u || !rt.memory().contains(allocated, pending.allocation_size)) {
        if (rt.memory().contains(pending.error_address, 4u))
            rt.memory().store32(pending.error_address, kFontErrorOutOfMemory);
        ctx.set_gpr(2, 0u);
        return;
    }

    rt.memory().zero(allocated, pending.allocation_size);
    const std::uint32_t slots = std::min(pending.params[1], 9u);
    pending.params[1] = slots;
    for (std::size_t index = 0; index < pending.params.size(); ++index)
        rt.memory().store32(allocated + static_cast<std::uint32_t>(index * 4u),
                            pending.params[index]);

    // Layout observed from the PSP libfont ABI. The allocation size was also
    // measured directly in Fate: numFonts=4 requests exactly 0x160C bytes.
    rt.memory().store32(allocated + 0x38u, std::bit_cast<std::uint32_t>(128.0f));
    rt.memory().store32(allocated + 0x3Cu, std::bit_cast<std::uint32_t>(128.0f));
    rt.memory().store32(allocated + 0x40u, kPspInternalFontCount);
    rt.memory().store32(allocated + 0x44u,
        allocated + 0x4Cu + slots * 0x4Cu + slots * 0x230u);
    rt.memory().store16(allocated + 0x48u, 0x5Fu);
    for (std::uint32_t index = 0; index < slots; ++index)
        rt.memory().store32(allocated + 0x4Cu + index * 0x4Cu,
                            allocated + 0x4Cu + index * 0x4Cu);

    g_font_libraries.emplace(allocated,
        FontLibraryState{pending.params, allocated, slots, 0x5Fu});
    rt.memory().store32(pending.error_address, 0u);
    delay_mpeg_result(rt, ctx, 30'000u, allocated);
}

void font_open_callback_return(psprecomp::Runtime& rt,
                               psprecomp::AllegrexContext& ctx) {
    const auto found = g_font_open_pending.find(g_table.current_uid);
    if (found == g_font_open_pending.end()) {
        rt.stop("retorno de alloc sceFontOpen sin llamada pendiente");
        return;
    }
    const FontOpenPending pending = found->second;
    g_font_open_pending.erase(found);
    const std::uint32_t allocated = ctx.gpr[2];
    ctx = pending.resume;

    const auto library = g_font_libraries.find(pending.library);
    if (library == g_font_libraries.end() || pending.slot >= library->second.slot_count) {
        rt.stop("biblioteca sceFontOpen desaparecio durante callback");
        return;
    }
    library->second.slot_allocations[pending.slot] = allocated;
    rt.memory().store32(pending.error_address, 0u);
    delay_mpeg_result(rt, ctx, 10'000u, pending.handle);
}

void font_char_info_callback_return(psprecomp::Runtime& rt,
                                    psprecomp::AllegrexContext& ctx) {
    const auto found = g_font_char_info_pending.find(g_table.current_uid);
    if (found == g_font_char_info_pending.end()) {
        rt.stop("retorno de callback sceFontGetCharInfo sin llamada pendiente");
        return;
    }
    FontCharInfoPending& pending = found->second;
    const auto library = g_font_libraries.find(pending.library);
    if (library == g_font_libraries.end()) {
        rt.stop("biblioteca sceFontGetCharInfo desaparecio durante callback");
        return;
    }
    if (pending.stage == FontCharInfoCallbackStage::free_previous) {
        library->second.char_info_allocation = 0u;
        pending.stage = FontCharInfoCallbackStage::allocate;
        ctx = pending.resume;
        ctx.set_gpr(4, library->second.params[0]);
        ctx.set_gpr(5, pending.allocation_size);
        ctx.set_gpr(31, 20u);
        ctx.pc = library->second.params[3];
        return;
    }

    library->second.char_info_allocation = ctx.gpr[2];
    const psprecomp::AllegrexContext resume = pending.resume;
    g_font_char_info_pending.erase(found);
    ctx = resume;
    ctx.set_gpr(2, 0u);
}

void install_font(psprecomp::Runtime& runtime) {
    bind(runtime, "sceLibFont", 0x67F17ED7u, "sceFontNewLib",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const std::uint32_t params_address = ctx.gpr[4];
             const std::uint32_t error_address = ctx.gpr[5];
             if (!rt.memory().contains(params_address, 44u) ||
                 !rt.memory().contains(error_address, 4u)) {
                 ctx.set_gpr(2, kErrorIllegalAddress);
                 return;
             }
             std::array<std::uint32_t, 11> params{};
             for (std::size_t index = 0; index < params.size(); ++index)
                 params[index] = rt.memory().load32(
                     params_address + static_cast<std::uint32_t>(index * 4u));
             const std::uint32_t alloc_callback = params[3];
             const std::uint32_t free_callback = params[4];
             if (alloc_callback == 0u || free_callback == 0u ||
                 !rt.memory().contains(alloc_callback, 4u) ||
                 !rt.memory().contains(free_callback, 4u) ||
                 g_font_newlib_pending.contains(g_table.current_uid)) {
                 rt.memory().store32(error_address, kFontErrorInvalidParameter);
                 ctx.set_gpr(2, 0u);
                 return;
             }

             const std::uint32_t slots = std::min(params[1], 9u);
             const std::uint32_t allocation_size =
                 0x4Cu + slots * 0x4Cu + slots * 0x230u +
                 kPspInternalFontCount * 0xA8u;
             FontNewLibPending pending{};
             pending.resume = ctx;
             pending.resume.pc = ctx.gpr[31];
             pending.params = params;
             pending.error_address = error_address;
             pending.allocation_size = allocation_size;
             g_font_newlib_pending.emplace(g_table.current_uid, std::move(pending));
             rt.memory().store32(error_address, 0u);

             ctx.set_gpr(4, params[0]);
             ctx.set_gpr(5, allocation_size);
             ctx.set_gpr(31, 12u);
             ctx.pc = alloc_callback;
         });

    bind(runtime, "sceLibFont", 0x27F6E642u, "sceFontGetNumFontList",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const std::uint32_t error_address = ctx.gpr[5];
             if (!rt.memory().contains(error_address, 4u)) {
                 ctx.set_gpr(2, kFontErrorInvalidParameter);
                 return;
             }
             if (!g_font_libraries.contains(ctx.gpr[4])) {
                 rt.memory().store32(error_address, kFontErrorInvalidLibrary);
                 ctx.set_gpr(2, 0u);
                 return;
             }
             rt.memory().store32(error_address, 0u);
             ctx.set_gpr(2, kPspInternalFontCount);
         });

    bind(runtime, "sceLibFont", 0xBC75D85Bu, "sceFontGetFontList",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const std::uint32_t library = ctx.gpr[4];
             const std::uint32_t styles_address = ctx.gpr[5];
             const std::int32_t requested = static_cast<std::int32_t>(ctx.gpr[6]);
             if (!g_font_libraries.contains(library)) {
                 ctx.set_gpr(2, kFontErrorInvalidLibrary);
                 return;
             }
             // libfont validates the pointer even when numFonts is zero.
             if (!rt.memory().contains(styles_address, 1u)) {
                 ctx.set_gpr(2, kFontErrorInvalidParameter);
                 return;
             }
             const std::uint32_t count = requested > 0
                 ? std::min<std::uint32_t>(static_cast<std::uint32_t>(requested),
                                           kPspInternalFontCount)
                 : 0u;
             if (count != 0u &&
                 !rt.memory().contains(styles_address, count * kPspFontStyleSize)) {
                 ctx.set_gpr(2, kFontErrorInvalidParameter);
                 return;
             }
             for (std::uint32_t index = 0u; index < count; ++index)
                 write_psp_font_style(rt, styles_address + index * kPspFontStyleSize,
                                      kPspFontRegistryStyles[index]);
             delay_mpeg_result(rt, ctx, 100u, 0u);
         });

    bind(runtime, "sceLibFont", 0xA834319Du, "sceFontOpen",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             const std::uint32_t library_address = ctx.gpr[4];
             const std::uint32_t font_index = ctx.gpr[5];
             const std::uint32_t mode = ctx.gpr[6];
             const std::uint32_t error_address = ctx.gpr[7];
             if (!rt.memory().contains(error_address, 4u)) {
                 ctx.set_gpr(2, 0xFFFFFFFFu);
                 return;
             }
             auto library = g_font_libraries.find(library_address);
             if (library == g_font_libraries.end()) {
                 rt.memory().store32(error_address, kFontErrorInvalidLibrary);
                 ctx.set_gpr(2, 0u);
                 return;
             }
             if (font_index >= kPspInternalFontCount) {
                 rt.memory().store32(error_address, kFontErrorInvalidParameter);
                 ctx.set_gpr(2, 0u);
                 return;
             }

             std::uint32_t slot = library->second.slot_count;
             for (std::uint32_t index = 0u; index < library->second.slot_count; ++index) {
                 if (library->second.slot_ref_counts[index] != 0u &&
                     library->second.slot_font_indices[index] ==
                         static_cast<std::int32_t>(font_index)) {
                     slot = index;
                     break;
                 }
             }
             if (slot == library->second.slot_count) {
                 for (std::uint32_t index = 0u; index < library->second.slot_count; ++index) {
                     if (library->second.slot_ref_counts[index] == 0u) {
                         slot = index;
                         break;
                     }
                 }
             }
             if (slot == library->second.slot_count ||
                 library->second.slot_ref_counts[slot] >= 4u) {
                 rt.memory().store32(error_address, kFontErrorTooManyOpenFonts);
                 ctx.set_gpr(2, 0u);
                 return;
             }

             const std::uint32_t handle = library_address + 0x4Cu + slot * 0x4Cu;
             if (library->second.slot_ref_counts[slot] != 0u) {
                 ++library->second.slot_ref_counts[slot];
                 rt.memory().store32(error_address, 0u);
                 ctx.set_gpr(2, handle);
                 return;
             }
             if (g_font_open_pending.contains(g_table.current_uid)) {
                 rt.memory().store32(error_address, kFontErrorInvalidParameter);
                 ctx.set_gpr(2, 0u);
                 return;
             }

             library->second.slot_font_indices[slot] =
                 static_cast<std::int32_t>(font_index);
             library->second.slot_ref_counts[slot] = 1u;
             const PspFontAllocationSizes sizes = kPspFontAllocationSizes[font_index];
             const std::uint32_t allocation_size = mode == 1u
                 ? sizes.full + 12u : sizes.stingy;
             FontOpenPending pending{};
             pending.resume = ctx;
             pending.resume.pc = ctx.gpr[31];
             pending.library = library_address;
             pending.error_address = error_address;
             pending.handle = handle;
             pending.slot = slot;
             g_font_open_pending.emplace(g_table.current_uid, std::move(pending));
             rt.memory().store32(error_address, 0u);

             ctx.set_gpr(4, library->second.params[0]);
             ctx.set_gpr(5, allocation_size);
             ctx.set_gpr(31, 16u);
             ctx.pc = library->second.params[3];
         });

    bind(runtime, "sceLibFont", 0x0DA7535Eu, "sceFontGetFontInfo",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             constexpr std::uint32_t info_size = 0x108u;
             const std::uint32_t info_address = ctx.gpr[5];
             if (!rt.memory().contains(info_address, info_size)) {
                 ctx.set_gpr(2, kFontErrorInvalidParameter);
                 return;
             }
             FontLibraryState* library = nullptr;
             std::uint32_t slot = 0u;
             std::uint32_t font_index = 0u;
             if (!find_open_psp_font(ctx.gpr[4], library, slot, font_index)) {
                 ctx.set_gpr(2, kFontErrorInvalidParameter);
                 return;
             }
             fuc::port::psp::PgfFont* font = nullptr;
             const PgfLoadStatus load = load_psp_font(font_index, font);
             if (load != PgfLoadStatus::success) {
                 const char* reason = load == PgfLoadStatus::missing ? "falta" :
                     load == PgfLoadStatus::wrong_size ? "tamano no original" :
                     "header invalido";
                 rt.stop(std::string("sceFontGetFontInfo: ") + reason + " en " +
                         kPspFontRegistryStyles[font_index].file_name);
                 return;
             }
             const fuc::port::psp::PgfFontMetrics metrics =
                 fuc::port::psp::pgf_font_metrics(font->header());
             rt.memory().zero(info_address, info_size);
             for (std::uint32_t index = 0u; index < metrics.fixed_26_6.size(); ++index) {
                 rt.memory().store32(info_address + index * 4u,
                                     static_cast<std::uint32_t>(metrics.fixed_26_6[index]));
                 rt.memory().store32(info_address + 0x28u + index * 4u,
                                     std::bit_cast<std::uint32_t>(metrics.floating[index]));
             }
             rt.memory().store16(info_address + 0x50u, metrics.max_glyph_width);
             rt.memory().store16(info_address + 0x52u, metrics.max_glyph_height);
             rt.memory().store32(info_address + 0x54u, metrics.glyph_count);
             rt.memory().store32(info_address + 0x58u, metrics.shadow_map_length);
             write_psp_font_style(rt, info_address + 0x5Cu,
                                  kPspFontRegistryStyles[font_index]);
             rt.memory().store8(info_address + 0x104u, metrics.bits_per_pixel);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceLibFont", 0xDCC80C2Fu, "sceFontGetCharInfo",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             constexpr std::uint32_t info_size = 60u;
             const std::uint16_t character = static_cast<std::uint16_t>(ctx.gpr[5]);
             const std::uint32_t info_address = ctx.gpr[6];
             if (!rt.memory().contains(info_address, info_size)) {
                 ctx.set_gpr(2, kFontErrorInvalidParameter);
                 return;
             }
             FontLibraryState* library = nullptr;
             std::uint32_t slot = 0u;
             std::uint32_t font_index = 0u;
             if (!find_open_psp_font(ctx.gpr[4], library, slot, font_index)) {
                 ctx.set_gpr(2, kFontErrorInvalidParameter);
                 return;
             }
             fuc::port::psp::PgfFont* font = nullptr;
             const PgfLoadStatus load = load_psp_font(font_index, font);
             if (load != PgfLoadStatus::success) {
                 const char* reason = load == PgfLoadStatus::missing ? "falta" :
                     load == PgfLoadStatus::wrong_size ? "tamano no original" :
                     "PGF invalido";
                 rt.stop(std::string("sceFontGetCharInfo: ") + reason + " en " +
                         kPspFontRegistryStyles[font_index].file_name);
                 return;
             }

             fuc::port::psp::PgfCharInfo info{};
             (void)font->character_info(character, library->alt_character, info);
             rt.memory().zero(info_address, info_size);
             const std::array<std::int32_t, 14> words{{
                 static_cast<std::int32_t>(info.bitmap_width),
                 static_cast<std::int32_t>(info.bitmap_height), info.bitmap_left,
                 info.bitmap_top, info.width_26_6, info.height_26_6,
                 info.ascender_26_6, info.descender_26_6,
                 info.bearing_h_x_26_6, info.bearing_h_y_26_6,
                 info.bearing_v_x_26_6, info.bearing_v_y_26_6,
                 info.advance_h_26_6, info.advance_v_26_6}};
             for (std::uint32_t index = 0u; index < words.size(); ++index)
                 rt.memory().store32(info_address + index * 4u,
                                      static_cast<std::uint32_t>(words[index]));
             rt.memory().store16(info_address + 0x38u,
                                  static_cast<std::uint16_t>(info.shadow_flags));
             rt.memory().store16(info_address + 0x3Au,
                                  static_cast<std::uint16_t>(info.shadow_id));

             if ((info.advance_h_26_6 == 0 && info.advance_v_26_6 == 0) ||
                 info.bitmap_width == 0u || info.bitmap_height == 0u) {
                 ctx.set_gpr(2, 0u);
                 return;
             }
             if (g_font_char_info_pending.contains(g_table.current_uid)) {
                 ctx.set_gpr(2, kFontErrorInvalidParameter);
                 return;
             }
             FontCharInfoPending pending{};
             pending.resume = ctx;
             pending.resume.pc = ctx.gpr[31];
             pending.library = library->handle;
             pending.allocation_size = info.bitmap_width * info.bitmap_height;
             pending.stage = library->char_info_allocation != 0u
                 ? FontCharInfoCallbackStage::free_previous
                 : FontCharInfoCallbackStage::allocate;
             g_font_char_info_pending.emplace(g_table.current_uid, pending);
             ctx.set_gpr(4, library->params[0]);
             ctx.set_gpr(5, pending.stage == FontCharInfoCallbackStage::free_previous
                 ? library->char_info_allocation : pending.allocation_size);
             ctx.set_gpr(31, 20u);
             ctx.pc = pending.stage == FontCharInfoCallbackStage::free_previous
                 ? library->params[4] : library->params[3];
         });

    bind(runtime, "sceLibFont", 0xEE232411u, "sceFontSetAltCharacterCode",
         [](psprecomp::Runtime&, psprecomp::AllegrexContext& ctx) {
             const auto library = g_font_libraries.find(ctx.gpr[4]);
             if (library == g_font_libraries.end()) {
                 ctx.set_gpr(2, kFontErrorInvalidLibrary);
                 return;
             }
             library->second.alt_character = static_cast<std::uint16_t>(ctx.gpr[5]);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceLibFont", 0xCA1E6945u, "sceFontGetCharGlyphImage_Clip",
         [](psprecomp::Runtime& rt, psprecomp::AllegrexContext& ctx) {
             constexpr std::uint32_t image_size = 24u;
             const std::uint16_t character = static_cast<std::uint16_t>(ctx.gpr[5]);
             const std::uint32_t image_address = ctx.gpr[6];
             if (!rt.memory().contains(image_address, image_size)) {
                 ctx.set_gpr(2, kFontErrorInvalidParameter);
                 return;
             }
             FontLibraryState* library = nullptr;
             std::uint32_t slot = 0u;
             std::uint32_t font_index = 0u;
             if (!find_open_psp_font(ctx.gpr[4], library, slot, font_index)) {
                 ctx.set_gpr(2, kFontErrorInvalidParameter);
                 return;
             }
             fuc::port::psp::PgfFont* font = nullptr;
             const PgfLoadStatus load = load_psp_font(font_index, font);
             if (load != PgfLoadStatus::success) {
                 rt.stop(std::string("sceFontGetCharGlyphImage_Clip: PGF ausente o invalido en ") +
                         kPspFontRegistryStyles[font_index].file_name);
                 return;
             }

             const std::uint32_t pixel_format = rt.memory().load32(image_address);
             // PSP hardware accepts the other documented formats but leaves
             // their destination untouched.
             if (pixel_format != 0u && pixel_format != 2u) {
                 ctx.set_gpr(2, 0u);
                 return;
             }
             fuc::port::psp::PgfGlyphBitmap bitmap{};
             if (!font->glyph_bitmap(character, library->alt_character, bitmap)) {
                 ctx.set_gpr(2, 0u);
                 return;
             }
             const std::int32_t x_position =
                 static_cast<std::int32_t>(rt.memory().load32(image_address + 4u));
             const std::int32_t y_position =
                 static_cast<std::int32_t>(rt.memory().load32(image_address + 8u));
             const std::int32_t origin_x = x_position >> 6;
             const std::int32_t origin_y = y_position >> 6;
             const std::int32_t fraction_x = x_position & 0x3F;
             const std::uint16_t buffer_width = rt.memory().load16(image_address + 12u);
             const std::uint16_t buffer_height = rt.memory().load16(image_address + 14u);
             const std::uint16_t bytes_per_line = rt.memory().load16(image_address + 16u);
             const std::uint32_t buffer = rt.memory().load32(image_address + 20u);
             std::int32_t clip_x = static_cast<std::int32_t>(ctx.gpr[7]);
             std::int32_t clip_y = -1;
             std::int32_t clip_width = -1;
             std::int32_t clip_height = -1;
             const std::uint32_t stack = ctx.gpr[29];
             if (rt.memory().contains(stack + 16u, 12u)) {
                 clip_y = static_cast<std::int32_t>(rt.memory().load32(stack + 16u));
                 clip_width = static_cast<std::int32_t>(rt.memory().load32(stack + 20u));
                 clip_height = static_cast<std::int32_t>(rt.memory().load32(stack + 24u));
             }
             if (clip_x < 0) clip_x = 0;
             if (clip_y < 0) clip_y = 0;
             if (clip_width < 0) clip_width = 8192;
             if (clip_height < 0) clip_height = 8192;

             const std::int64_t clip_right = static_cast<std::int64_t>(clip_x) + clip_width;
             const std::int64_t clip_bottom = static_cast<std::int64_t>(clip_y) + clip_height;
             const std::int32_t first_x = std::max(clip_x, origin_x) - origin_x;
             const std::int32_t first_y = std::max(clip_y, origin_y) - origin_y;
             const std::int32_t last_x = static_cast<std::int32_t>(std::min<std::int64_t>(
                 clip_right - origin_x, bitmap.width + (fraction_x != 0 ? 1u : 0u)));
             const std::int32_t last_y = static_cast<std::int32_t>(std::min<std::int64_t>(
                 clip_bottom - origin_y, bitmap.height));
             const auto sample = [&](const std::int32_t x, const std::int32_t y) {
                 if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(bitmap.width) ||
                     y >= static_cast<std::int32_t>(bitmap.height)) return 0;
                 return static_cast<int>(bitmap.pixels[
                     static_cast<std::size_t>(y) * bitmap.width + x]);
             };
             const std::uint32_t line_pixel_capacity = pixel_format == 0u
                 ? static_cast<std::uint32_t>(bytes_per_line) * 2u
                 : bytes_per_line;
             for (std::int32_t y = first_y; y < last_y; ++y) {
                 for (std::int32_t x = first_x; x < last_x; ++x) {
                     const std::int32_t destination_x = origin_x + x;
                     const std::int32_t destination_y = origin_y + y;
                     if (destination_x < 0 || destination_y < 0 ||
                         destination_x >= buffer_width || destination_y >= buffer_height ||
                         static_cast<std::uint32_t>(destination_x) >= line_pixel_capacity)
                         continue;
                     const int scale = pixel_format == 0u ? 1 : 17;
                     const int left = sample(x - 1, y) * scale;
                     const int current = sample(x, y) * scale;
                     const int coverage = left * fraction_x / 64 +
                         (current * (64 - fraction_x) + 63) / 64;
                     const std::uint64_t byte_offset =
                         static_cast<std::uint64_t>(destination_y) * bytes_per_line +
                         (pixel_format == 0u ? destination_x / 2u : destination_x);
                     if (byte_offset > std::numeric_limits<std::uint32_t>::max() - buffer)
                         continue;
                     const std::uint32_t address = buffer + static_cast<std::uint32_t>(byte_offset);
                     if (!rt.memory().contains(address, 1u)) continue;
                     const std::uint8_t old = rt.memory().load8(address);
                     if (pixel_format == 0u) {
                         const std::uint32_t shift = (destination_x & 1) != 0 ? 4u : 0u;
                         const int old_pixel = (old >> shift) & 0x0F;
                         const std::uint8_t value = static_cast<std::uint8_t>(
                             (old & ~(0x0Fu << shift)) |
                             (std::min(old_pixel + coverage, 15) << shift));
                         rt.memory().store8(address, value);
                     } else {
                         rt.memory().store8(address, static_cast<std::uint8_t>(
                             std::min(static_cast<int>(old) + coverage, 255)));
                     }
                 }
             }
             ctx.set_gpr(2, 0u);
         });
}

void install_sas(psprecomp::Runtime &runtime) {
    bind(runtime, "sceSasCore", 0x42778A9Fu, "__sceSasInit",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t core = ctx.gpr[4];
             const std::uint32_t grain = ctx.gpr[5];
             const std::uint32_t voices = ctx.gpr[6];
             const std::uint32_t mode = ctx.gpr[7];
             const std::uint32_t rate = ctx.gpr[8];
             if ((core & 0x3Fu) != 0u || !rt.memory().contains(core, 64u)) {
                 ctx.set_gpr(2, kSasBadAddress); return;
             }
             if (grain < 64u || grain > 2048u || (grain & 63u) != 0u) {
                 ctx.set_gpr(2, kSasInvalidGrain); return;
             }
             if (voices == 0u || voices > 32u) {
                 ctx.set_gpr(2, kSasInvalidMaxVoices); return;
             }
             if (mode > 1u) { ctx.set_gpr(2, kSasInvalidOutputMode); return; }
             if (rate != 44100u) { ctx.set_gpr(2, kSasInvalidSampleRate); return; }
             g_sas = SasState{};
             g_sas.initialized = true;
             g_sas.core_address = core;
             g_sas.grain_size = grain;
             g_sas.max_voices = voices;
             g_sas.output_mode = mode;
             g_sas.sample_rate = rate;
             rt.memory().zero(core, 64u);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0x99944089u, "__sceSasSetVoice",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const auto voice_index = static_cast<std::int32_t>(ctx.gpr[5]);
             auto *voice = sas_voice(ctx.gpr[4], voice_index, ctx);
             if (!voice) return;
             const std::uint32_t address = ctx.gpr[6];
             const std::int32_t size = static_cast<std::int32_t>(ctx.gpr[7]);
             const std::int32_t loop = static_cast<std::int32_t>(ctx.gpr[8]);
             if (size <= 0 || (static_cast<std::uint32_t>(size) & 15u) != 0u) {
                 ctx.set_gpr(2, kSasInvalidParameter); return;
             }
             if (loop != 0 && loop != 1) { ctx.set_gpr(2, kSasInvalidLoop); return; }
             if (!rt.memory().contains(address, static_cast<std::size_t>(size))) {
                 ctx.set_gpr(2, kSasBadAddress); return;
             }
             voice->type = SasVoiceType::Vag;
             voice->data_address = address;
             voice->data_size = static_cast<std::uint32_t>(size);
              const auto* source = rt.memory().raw_pointer(address,
                  static_cast<std::size_t>(size));
              const auto decoded = fuc::port::psp::decode_vag_adpcm(
                  {source, static_cast<std::size_t>(size)});
             ++g_vag_flow.voices;
             g_vag_flow.source_blocks += static_cast<std::uint64_t>(size) / 16u;
             g_vag_flow.decoded_blocks += decoded.samples.size() / 28u;
             if (decoded.terminal_flag < g_vag_flow.terminal_flags.size())
                 ++g_vag_flow.terminal_flags[decoded.terminal_flag];
             if (decoded.terminal_flag != 0u) {
                 const std::uint64_t consumed_blocks =
                     decoded.terminal_block_offset / 16u + 1u;
                 const std::uint64_t source_blocks =
                     static_cast<std::uint64_t>(size) / 16u;
                 g_vag_flow.trailing_blocks_skipped +=
                     source_blocks > consumed_blocks ? source_blocks - consumed_blocks : 0u;
             }
             voice->pcm = decoded.samples;
             voice->loop_start_sample = decoded.loop_start_sample;
             voice->loop_start_valid = decoded.loop_start_valid;
             voice->sample_cursor_12 = 0u;
             voice->remaining_samples = voice->pcm.size();
             voice->loop = fuc::port::psp::vag_should_loop(decoded, loop != 0);
              sas_trace("set voice=%d addr=%08X size=%u loop=%d samples=%zu "
                        "terminal=%u loop-start=%zu valid=%u",
                        voice_index, address, static_cast<std::uint32_t>(size), loop,
                        voice->pcm.size(), static_cast<unsigned>(decoded.terminal_flag),
                       decoded.loop_start_sample,
                       decoded.loop_start_valid ? 1u : 0u);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0xB7660A23u, "__sceSasSetNoise",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
             if (!voice) return;
             const auto frequency = static_cast<std::int32_t>(ctx.gpr[6]);
             if (frequency < 0 || frequency > 63) { ctx.set_gpr(2, kSasInvalidNoise); return; }
             voice->type = SasVoiceType::Noise;
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0xAD84D37Fu, "__sceSasSetPitch",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
             if (!voice) return;
             const auto pitch = static_cast<std::int32_t>(ctx.gpr[6]);
             if (pitch < 1 || pitch > 0x4000) { ctx.set_gpr(2, kSasInvalidPitch); return; }
             voice->pitch = pitch;
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0x440CA7D8u, "__sceSasSetVolume",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
             if (!voice) return;
             const std::array<std::int32_t, 4> values{
                 static_cast<std::int32_t>(ctx.gpr[6]), static_cast<std::int32_t>(ctx.gpr[7]),
                 static_cast<std::int32_t>(ctx.gpr[8]), static_cast<std::int32_t>(ctx.gpr[9])};
             for (const auto value : values) {
                 if (value < -0x1000 || value > 0x1000) {
                     ctx.set_gpr(2, kSasInvalidVolume); return;
                 }
             }
             voice->volumes = values;
             sas_trace("time=%llu set-volume voice=%d dry=(%d,%d) wet=(%d,%d) "
                       "type=%u on=%u playing=%u",
                       static_cast<unsigned long long>(g_virtual_time_us),
                       static_cast<std::int32_t>(ctx.gpr[5]), values[0], values[1],
                       values[2], values[3], static_cast<unsigned>(voice->type),
                       voice->on ? 1u : 0u, voice->playing ? 1u : 0u);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0xCBCD4F79u, "__sceSasSetSimpleADSR",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
             if (!voice) return;
             if (((ctx.gpr[7] >> 13u) & 1u) != 0u) {
                 ctx.set_gpr(2, kSasInvalidAdsrMode); return;
             }
             voice->simple_adsr1 = ctx.gpr[6] & 0xFFFFu;
             voice->simple_adsr2 = ctx.gpr[7] & 0xFFFFu;
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0x76F01ACAu, "__sceSasSetKeyOn",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const auto voice_index = static_cast<std::int32_t>(ctx.gpr[5]);
             auto *voice = sas_voice(ctx.gpr[4], voice_index, ctx);
             if (!voice) return;
             if (voice->paused || voice->on) { ctx.set_gpr(2, kSasInvalidState); return; }
             if (voice->type == SasVoiceType::Vag)
             {
                 voice->sample_cursor_12 = 0u;
                 voice->remaining_samples = voice->pcm.size();
             }
             voice->on = true;
             voice->playing = voice->type != SasVoiceType::Off;
             voice->envelope_height = 0u;
             sas_trace("time=%llu key-on voice=%d loop=%u samples=%zu "
                       "dry=(%d,%d) wet=(%d,%d) adsr=(%04X,%04X) cursor=%llu",
                       static_cast<unsigned long long>(g_virtual_time_us), voice_index,
                       voice->loop ? 1u : 0u, voice->pcm.size(),
                       voice->volumes[0], voice->volumes[1],
                       voice->volumes[2], voice->volumes[3],
                       voice->simple_adsr1, voice->simple_adsr2,
                       static_cast<unsigned long long>(voice->sample_cursor_12));
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0xA0CF2FA4u, "__sceSasSetKeyOff",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const auto voice_index = static_cast<std::int32_t>(ctx.gpr[5]);
             auto *voice = sas_voice(ctx.gpr[4], voice_index, ctx);
             if (!voice) return;
             if (voice->paused || !voice->on) { ctx.set_gpr(2, kSasInvalidState); return; }
             voice->on = false;
             sas_trace("key-off voice=%d cursor=%llu remaining=%llu", voice_index,
                       static_cast<unsigned long long>(voice->sample_cursor_12),
                       static_cast<unsigned long long>(voice->remaining_samples));
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0x2C8E6AB3u, "__sceSasGetPauseFlag",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasNotInitialized); return; }
             std::uint32_t flags = 0u;
             for (std::size_t i = 0; i < g_sas.voices.size(); ++i)
                 if (g_sas.voices[i].paused) flags |= 1u << i;
             ctx.set_gpr(2, flags);
         });

    bind(runtime, "sceSasCore", 0x68A46B95u, "__sceSasGetEndFlag",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasNotInitialized); return; }
             std::uint32_t flags = 0u;
             for (std::size_t i = 0; i < g_sas.voices.size(); ++i)
                 if (!g_sas.voices[i].playing) flags |= 1u << i;
             ctx.set_gpr(2, flags);
         });

    bind(runtime, "sceSasCore", 0x07F58C24u, "__sceSasGetAllEnvelopeHeights",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasNotInitialized); return; }
             const std::uint32_t heights = ctx.gpr[5];
             if (!rt.memory().contains(heights, 32u * 4u)) {
                 ctx.set_gpr(2, kSasBadAddress); return;
             }
             for (std::size_t i = 0; i < g_sas.voices.size(); ++i)
                 rt.memory().store32(heights + static_cast<std::uint32_t>(i * 4u),
                                     g_sas.voices[i].envelope_height);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0xE175EF66u, "__sceSasGetOutputmode",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             ctx.set_gpr(2, sas_valid_core(ctx.gpr[4]) ? g_sas.output_mode
                                                       : kSasNotInitialized);
         });

    bind(runtime, "sceSasCore", 0x33D4AB37u, "__sceSasRevType",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasNotInitialized); return; }
             const auto type = static_cast<std::int32_t>(ctx.gpr[5]);
             if (type < -1 || type > 8) { ctx.set_gpr(2, kSasInvalidFxType); return; }
             g_sas.reverb_type = type;
             sas_trace("time=%llu reverb-type=%d",
                       static_cast<unsigned long long>(g_virtual_time_us), type);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0x267A6DD2u, "__sceSasRevParam",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasNotInitialized); return; }
             const auto delay = static_cast<std::int32_t>(ctx.gpr[5]);
             const auto feedback = static_cast<std::int32_t>(ctx.gpr[6]);
             if (delay < 0 || delay >= 128) { ctx.set_gpr(2, kSasInvalidFxDelay); return; }
             if (feedback < 0 || feedback >= 128) { ctx.set_gpr(2, kSasInvalidFxFeedback); return; }
             g_sas.reverb_delay = delay;
             g_sas.reverb_feedback = feedback;
             sas_trace("time=%llu reverb-param delay=%d feedback=%d",
                       static_cast<unsigned long long>(g_virtual_time_us), delay,
                       feedback);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0xD5A229C9u, "__sceSasRevEVOL",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasNotInitialized); return; }
             if (ctx.gpr[5] > 0x1000u || ctx.gpr[6] > 0x1000u) {
                 ctx.set_gpr(2, kSasInvalidFxVolume); return;
             }
             g_sas.reverb_left = ctx.gpr[5];
             g_sas.reverb_right = ctx.gpr[6];
             sas_trace("time=%llu reverb-volume=(%u,%u)",
                       static_cast<unsigned long long>(g_virtual_time_us),
                       g_sas.reverb_left, g_sas.reverb_right);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0xF983B186u, "__sceSasRevVON",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasNotInitialized); return; }
             g_sas.dry = ctx.gpr[5] != 0u;
             g_sas.wet = ctx.gpr[6] != 0u;
             sas_trace("time=%llu reverb-on dry=%u wet=%u",
                       static_cast<unsigned long long>(g_virtual_time_us),
                       g_sas.dry ? 1u : 0u, g_sas.wet ? 1u : 0u);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0xA3589D81u, "__sceSasCore",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasNotInitialized); return; }
             const std::size_t bytes = static_cast<std::size_t>(g_sas.grain_size) *
                 (g_sas.output_mode == 0u ? 4u : 8u);
             if (!rt.memory().contains(ctx.gpr[5], bytes)) {
                 ctx.set_gpr(2, kSasInvalidParameter); return;
             }
             sas_mix_grain(rt, ctx.gpr[5], false);
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceSasCore", 0x50A14DFCu, "__sceSasCoreWithMix",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasNotInitialized); return; }
             if (g_sas.output_mode != 0u) { ctx.set_gpr(2, kSasInvalidOutputMode); return; }
             const std::size_t bytes = static_cast<std::size_t>(g_sas.grain_size) * 4u;
             if (!rt.memory().contains(ctx.gpr[5], bytes)) {
                 ctx.set_gpr(2, kSasInvalidParameter); return;
             }
             if (ctx.gpr[6] > 0x1000u || ctx.gpr[7] > 0x1000u) {
                 ctx.set_gpr(2, kSasInvalidVolume); return;
             }
             sas_mix_grain(rt, ctx.gpr[5], true, ctx.gpr[6], ctx.gpr[7]);
             ctx.set_gpr(2, 0u);
         });
}

std::uint32_t audio_remaining_samples(const AudioChannelState &channel) {
    if (!channel.reserved || channel.busy_until_us <= g_virtual_time_us) return 0u;
    const std::uint64_t remaining_us = channel.busy_until_us - g_virtual_time_us;
    const std::uint64_t samples = (remaining_us * 44100u + 999999u) / 1000000u;
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(samples, channel.sample_count));
}

void install_audio(psprecomp::Runtime &runtime) {
    constexpr std::uint32_t kAudioInvalidChannel = 0x80260003u;
    constexpr std::uint32_t kAudioNoChannel = 0x80260005u;
    constexpr std::uint32_t kAudioInvalidSize = 0x80260006u;
    constexpr std::uint32_t kAudioInvalidFormat = 0x80260007u;
    constexpr std::uint32_t kAudioNotReserved = 0x80260008u;
    constexpr std::uint32_t kAudioBusy = 0x80260002u;
    constexpr std::uint32_t kAudioInvalidVolume = 0x8026000Bu;

    bind(runtime, "sceAudio", 0x5EC81C55u, "sceAudioChReserve",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             std::int32_t channel = static_cast<std::int32_t>(ctx.gpr[4]);
             const std::uint32_t sample_count = ctx.gpr[5];
             const std::uint32_t format = ctx.gpr[6];
             if (channel < 0) {
                 channel = -1;
                 for (std::int32_t candidate = 0; candidate < 8; ++candidate) {
                     if (!g_audio_channels[static_cast<std::size_t>(candidate)].reserved) {
                         channel = candidate;
                         break;
                     }
                 }
                 if (channel < 0) { ctx.set_gpr(2, kAudioNoChannel); return; }
             }
             if (channel >= 8) { ctx.set_gpr(2, kAudioInvalidChannel); return; }
             if (sample_count < 64u || (sample_count & 63u) != 0u ||
                 sample_count > 65472u) {
                 ctx.set_gpr(2, kAudioInvalidSize);
                 return;
             }
             if (format != 0u && format != 0x10u) {
                 ctx.set_gpr(2, kAudioInvalidFormat);
                 return;
             }
             auto &state = g_audio_channels[static_cast<std::size_t>(channel)];
             if (state.reserved) { ctx.set_gpr(2, kAudioBusy); return; }
             state = AudioChannelState{true, sample_count, format, 0u};
             ctx.set_gpr(2, static_cast<std::uint32_t>(channel));
         });

    bind(runtime, "sceAudio", 0x6FC46853u, "sceAudioChRelease",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t channel = ctx.gpr[4];
             if (channel >= g_audio_channels.size()) {
                 ctx.set_gpr(2, kAudioInvalidChannel);
                 return;
             }
             if (!g_audio_channels[channel].reserved) {
                 ctx.set_gpr(2, kAudioNotReserved);
                 return;
             }
             g_audio_channels[channel] = {};
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "sceAudio", 0xB011922Fu, "sceAudioGetChannelRestLen",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t channel = ctx.gpr[4];
             if (channel >= g_audio_channels.size()) {
                 ctx.set_gpr(2, kAudioInvalidChannel);
                 return;
             }
             if (!g_audio_channels[channel].reserved) {
                 ctx.set_gpr(2, kAudioNotReserved);
                 return;
             }
             ctx.set_gpr(2, audio_remaining_samples(g_audio_channels[channel]));
         });

    bind(runtime, "sceAudio", 0xCB2E439Eu, "sceAudioSetChannelDataLen",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t channel = ctx.gpr[4];
             const std::uint32_t sample_count = ctx.gpr[5];
             if (channel >= g_audio_channels.size()) {
                 ctx.set_gpr(2, kAudioInvalidChannel);
                 return;
             }
             if (!g_audio_channels[channel].reserved) {
                 ctx.set_gpr(2, kAudioNotReserved);
                 return;
             }
             if (sample_count < 64u || (sample_count & 63u) != 0u ||
                 sample_count > 65472u) {
                 ctx.set_gpr(2, kAudioInvalidSize);
                 return;
             }
             g_audio_channels[channel].sample_count = sample_count;
             ctx.set_gpr(2, 0u);
         });

    const auto output_panned = [](psprecomp::Runtime &rt,
                                  psprecomp::AllegrexContext &ctx) {
        const std::uint32_t channel = ctx.gpr[4];
        const std::uint32_t left_volume = ctx.gpr[5];
        const std::uint32_t right_volume = ctx.gpr[6];
        const std::uint32_t buffer = ctx.gpr[7];
        if (channel >= g_audio_channels.size()) {
            ctx.set_gpr(2, kAudioInvalidChannel);
            return;
        }
        auto &state = g_audio_channels[channel];
        if (!state.reserved) { ctx.set_gpr(2, kAudioNotReserved); return; }
        if (left_volume > 0xFFFFu || right_volume > 0xFFFFu) {
            ctx.set_gpr(2, kAudioInvalidVolume);
            return;
        }
        const std::uint32_t channels = state.format == 0x10u ? 1u : 2u;
        const std::size_t bytes = static_cast<std::size_t>(state.sample_count) * channels * 2u;
        if (buffer != 0u && !rt.memory().contains(buffer, bytes)) {
            ctx.set_gpr(2, 0x800200D3u);
            return;
        }

        const std::uint64_t start_us = std::max(g_virtual_time_us, state.busy_until_us);
        const std::uint64_t duration_us =
            (static_cast<std::uint64_t>(state.sample_count) * 1000000u + 44099u) / 44100u;
        if (buffer != 0u && g_audio_pcm_hook) {
            const auto* pcm = rt.memory().raw_pointer(buffer, bytes);
            if (pcm != nullptr) {
                bool atrac_buffer = false;
                const auto after = g_atrac_pcm_buffers.upper_bound(buffer);
                if (after != g_atrac_pcm_buffers.begin()) {
                    const auto range = std::prev(after);
                    const std::uint64_t output_end =
                        static_cast<std::uint64_t>(buffer) + bytes;
                    atrac_buffer = buffer >= range->first && output_end <= range->second;
                }
                if (std::getenv("PSPRECOMP_ATRAC_DIAG") != nullptr) {
                    static std::uint32_t diagnostic_outputs = 0u;
                    if (diagnostic_outputs < 48u && (channel == 1u || atrac_buffer)) {
                        std::printf("[audio-fate] channel=%u buffer=%08X samples=%u atrac=%u\n",
                                    channel, buffer, state.sample_count,
                                    atrac_buffer ? 1u : 0u);
                        ++diagnostic_outputs;
                    }
                }
                if (atrac_buffer) {
                    ++g_atrac_output_buffers;
                    const auto* decoded_pcm = reinterpret_cast<const std::int16_t*>(pcm);
                    g_atrac_output_nonzero += static_cast<std::uint64_t>(
                        std::count_if(decoded_pcm,
                                     decoded_pcm + static_cast<std::size_t>(
                                         state.sample_count) * channels,
                                     [](const std::int16_t sample) {
                                         return sample != 0;
                                     }));
                }
                ++g_audio_channel_buffers[channel];
                g_audio_channel_samples[channel] += state.sample_count;
                g_audio_pcm_hook({reinterpret_cast<const std::int16_t*>(pcm),
                                 static_cast<std::size_t>(state.sample_count) * channels},
                                 channels, left_volume, right_volume, channel, start_us);
            }
        }
        state.busy_until_us = start_us + duration_us;
        const std::uint64_t wait_us = start_us - g_virtual_time_us;
        if (wait_us == 0u) {
            ctx.set_gpr(2, state.sample_count);
            return;
        }
        psprecomp::AllegrexContext sleeping = ctx;
        sleeping.set_gpr(2, state.sample_count);
        sleeping.pc = ctx.gpr[31];
        g_table.ready.push_back({g_table.current_uid, sleeping,
                                 g_virtual_time_us + wait_us});
        (void)activate_next(rt, ctx, "audio-output");
    };

    bind(runtime, "sceAudio", 0x13F592BCu, "sceAudioOutputPannedBlocking",
         output_panned);
    bind(runtime, "sceAudio", 0x136CAF51u, "sceAudioOutputBlocking",
         [output_panned](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::uint32_t volume = ctx.gpr[5];
             const std::uint32_t buffer = ctx.gpr[6];
             ctx.set_gpr(6, volume);
             ctx.set_gpr(7, buffer);
             output_panned(rt, ctx);
         });
}

void install_module_manager(psprecomp::Runtime &runtime) {
    bind_success(runtime, "ModuleMgrForUser", 0xF0A26395u, "sceKernelGetModuleId", kSelfModuleId);

    bind(runtime, "ModuleMgrForUser", 0xD8B73127u, "sceKernelGetModuleIdByAddress",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             // Every executable address belongs to the one module that is loaded.
             ctx.set_gpr(2, rt.memory().contains(ctx.gpr[4], 4u) ? kSelfModuleId
                                                                 : kErrorUnknownModule);
         });

    bind(runtime, "ModuleMgrForUser", 0x977DE386u, "sceKernelLoadModule",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const std::string path = ctx.gpr[4] != 0u
                 ? rt.memory().read_c_string(ctx.gpr[4], 256u) : std::string{};
             if (!is_hle_backed_module(path)) {
                 ctx.set_gpr(2, kErrorUnknownModule);
                 return;
             }

             // A supported HLE module must still exist in the user's extracted
             // game data. This preserves the original dependency without
             // reading, decrypting or executing the commercial PRX.
             std::filesystem::path host_path;
             try { host_path = rt.translate_path(path); }
             catch (const std::exception &) { ctx.set_gpr(2, kErrorUnknownModule); return; }
             std::error_code error;
             if (!std::filesystem::is_regular_file(host_path, error) || error) {
                 ctx.set_gpr(2, kErrorUnknownModule);
                 return;
             }

             const std::int32_t uid = g_next_module_uid++;
             g_hle_modules.emplace(uid, HleModuleRecord{path, false});
             ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
         });

    bind(runtime, "ModuleMgrForUser", 0x50F0C1ECu, "sceKernelStartModule",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
             const auto found = g_hle_modules.find(uid);
             if (found == g_hle_modules.end()) {
                 ctx.set_gpr(2, kErrorUnknownModule);
                 return;
             }
             // a3 is the optional module_start status output. The fifth O32
             // argument (SceKernelSMOption*) is on the guest stack and is not
             // needed for an HLE-backed module.
             if (ctx.gpr[7] != 0u && rt.memory().contains(ctx.gpr[7], 4u))
                 rt.memory().store32(ctx.gpr[7], 0u);
             found->second.started = true;
             ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
         });

    bind(runtime, "ModuleMgrForUser", 0xD1FF982Au, "sceKernelStopModule",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
             const auto found = g_hle_modules.find(uid);
             if (found == g_hle_modules.end()) {
                 ctx.set_gpr(2, kErrorUnknownModule);
                 return;
             }
             if (ctx.gpr[7] != 0u && rt.memory().contains(ctx.gpr[7], 4u))
                 rt.memory().store32(ctx.gpr[7], 0u);
             found->second.started = false;
             ctx.set_gpr(2, 0u);
         });

    bind(runtime, "ModuleMgrForUser", 0x2E0911AAu, "sceKernelUnloadModule",
         [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
             const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
             ctx.set_gpr(2, g_hle_modules.erase(uid) == 1u ? 0u : kErrorUnknownModule);
         });

    // The game asking to stop and unload itself is a clean shutdown request.
    bind(runtime, "ModuleMgrForUser", 0x8F2DF740u, "sceKernelStopUnloadSelfModuleWithStatus",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             ctx.set_gpr(2, 0u);
             // This is where the run actually ends, so it is the place worth
             // dumping the PC ring from: the addresses just before it are the
             // path that decided to quit.
             dump_pc_ring("que el juego se descargase");
             rt.stop("el juego pidio descargarse a si mismo");
         });

    // Exit callback registration and the exit game request.
    bind_success(runtime, "LoadExecForUser", 0x4AC57943u, "sceKernelRegisterExitCallback", 0u);
    bind(runtime, "LoadExecForUser", 0x05572A5Fu, "sceKernelExitGame",
         [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
             ctx.set_gpr(2, 0u);
             rt.stop("el juego pidio salir");
         });
}

} // namespace

void dump_trace_pc_ring(const char *why) { dump_pc_ring(why); }

void trace_pc_heartbeat(std::uint32_t pc) { heartbeat_record_pc(0u, pc); }

void set_umd_image(const std::filesystem::path &path) { g_umd_image = path; }

void set_savedata_root(const std::filesystem::path &path) {
    g_savedata_root = path.empty() ? std::filesystem::path{}
                                   : std::filesystem::absolute(path).lexically_normal();
}

void set_game_data_root(const std::filesystem::path &path) {
    g_game_data_root = path.empty() ? std::filesystem::path{}
                                    : std::filesystem::absolute(path).lexically_normal();
}

void set_font_root(const std::filesystem::path &path) {
    g_font_root = path.empty() ? std::filesystem::path{}
                               : std::filesystem::absolute(path).lexically_normal();
}

std::uint64_t virtual_time_us() { return g_virtual_time_us; }

void set_pad_state(std::uint32_t buttons, std::uint8_t analog_x, std::uint8_t analog_y) {
    g_pad_buttons = buttons;
    g_pad_analog_x = analog_x;
    g_pad_analog_y = analog_y;
}

void set_vblank_hook(VblankHook hook) { g_vblank_hook = std::move(hook); }

void set_ge_draw_hook(GeDrawHook hook) { g_ge_draw_hook = std::move(hook); }

void set_audio_pcm_hook(AudioPcmHook hook) { g_audio_pcm_hook = std::move(hook); }

void set_trace_pc(bool enabled) { g_trace_pc = enabled; }

void install_profile(psprecomp::Runtime &runtime, std::uint32_t arena_start,
                     std::uint32_t arena_end, bool trace_scheduler, bool trace_hle) {
    g_table = ThreadTable{};
    g_table.arena_next = (arena_start + 0xFFu) & ~0xFFu;
    g_table.arena_end = arena_end;
    g_table.trace = trace_scheduler;
    if (g_trace_pc) psprecomp::set_runtime_heartbeat_hook(&heartbeat_record_pc, 1u);
    g_trace_hle = trace_hle;
    g_hle_calls.clear();
    g_virtual_time_us = 0u;
    g_compiled_sdk_version = 0u;
    g_semaphores.clear();
    g_event_flags.clear();
    g_callbacks.clear();
    g_lw_mutexes.clear();
    g_next_sync_uid = 0x2000;
    g_vcount = 0u;
    g_vblank_hook = {};
    g_ge_draw_hook = {};
    g_audio_pcm_hook = {};
    g_next_display_list_id = 0x100;
    g_ge_finish_func = 0u;
    g_ge_finish_arg = 0u;
    g_ge_callback_stack = 0u;
    g_ge_runtime_lists.clear();
    g_ge_consumer.reset();
    open_ge_trace_from_environment();
    g_files.clear();
    g_next_file_uid = 0x3000;
    g_framebuffer = 0u;
    g_interrupts_enabled = true;
    g_sub_interrupts.clear();
    g_sub_interrupt_returns.clear();
    g_next_sub_interrupt_us = 0u;
    g_blocks.clear();
    // g_pools was missing here while every other table was reset. One run per
    // process hides it, but a second install() would hand out blocks from an
    // arena the first run had already released.
    g_pools.clear();
    g_next_block_uid = 0x1000;
    g_hle_modules.clear();
    g_next_module_uid = 0x4000;
    g_font_libraries.clear();
    g_font_newlib_pending.clear();
    g_font_open_pending.clear();
    g_font_char_info_pending.clear();
    g_psp_fonts.clear();
    g_audio_channels = {};
    g_atrac_pcm_buffers.clear();
    g_atrac_decoded_samples = 0u;
    g_atrac_decoded_nonzero = 0u;
    g_atrac_output_buffers = 0u;
    g_atrac_output_nonzero = 0u;
    g_mpeg.clear();
    g_mpeg_callbacks.clear();
    g_next_mpeg_stream_id = 1u;
    g_mpeg_video_frames = 0u;
    g_mpeg_audio_bytes = 0u;
    g_mpeg_csc_crops = 0u;
    g_sas = {};
    g_savedata = {};
    g_ge_flow = {};

    // The two version setters the module calls before anything else. They record
    // build metadata the kernel never reports back, so returning success is the
    // whole behaviour.
    runtime.register_hle("SysMemUserForUser", 0x91DE343Cu,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            g_compiled_sdk_version = ctx.gpr[4];
            ctx.set_gpr(2, 0u);
        });
    runtime.register_hle("SysMemUserForUser", 0xF77D77CBu,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });

    install_thread_manager(runtime);
    install_memory_manager(runtime);
    install_benign(runtime);
    install_module_manager(runtime);
    install_sync(runtime);
    install_system_services(runtime);
    install_graphics(runtime);
    install_file_io(runtime);
    install_utility(runtime);
    install_audio(runtime);
    install_sas(runtime);
    install_atrac(runtime);
    install_mpeg(runtime);
    install_font(runtime);
    runtime.register_function(0u, &thread_return_trampoline, "thread_return");
    runtime.register_function(4u, &mpeg_callback_return, "mpeg_callback_return");
    runtime.register_function(8u, &sub_interrupt_return, "sub_interrupt_return");
    runtime.register_function(12u, &font_newlib_callback_return,
                              "font_newlib_callback_return");
    runtime.register_function(16u, &font_open_callback_return,
                              "font_open_callback_return");
    runtime.register_function(20u, &font_char_info_callback_return,
                              "font_char_info_callback_return");
}

void report_hle_calls() {
    if (g_hle_calls.empty()) return;
    std::vector<std::pair<std::string, std::uint64_t>> ranked(g_hle_calls.begin(),
                                                              g_hle_calls.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    std::uint64_t total = 0u;
    for (const auto &row : ranked) total += row.second;
    std::printf("\nimports implementados llamados: %zu, llamadas totales: %llu\n",
                ranked.size(), static_cast<unsigned long long>(total));
    for (const auto &row : ranked) {
        std::printf("   %-52s %llu\n", row.first.c_str(),
                    static_cast<unsigned long long>(row.second));
    }
}

void report_audio_flow() {
    std::printf("flujo audio ATRAC PSP: decoded=%llu nonzero=%llu output-buffers=%llu output-nonzero=%llu\n",
                static_cast<unsigned long long>(g_atrac_decoded_samples),
                static_cast<unsigned long long>(g_atrac_decoded_nonzero),
                static_cast<unsigned long long>(g_atrac_output_buffers),
                static_cast<unsigned long long>(g_atrac_output_nonzero));
    std::printf(
        "flujo VAG PSP: voices=%llu source-blocks=%llu decoded-blocks=%llu "
        "trailing-skipped=%llu terminal[1]=%llu terminal[3]=%llu terminal[7]=%llu\n",
        static_cast<unsigned long long>(g_vag_flow.voices),
        static_cast<unsigned long long>(g_vag_flow.source_blocks),
        static_cast<unsigned long long>(g_vag_flow.decoded_blocks),
        static_cast<unsigned long long>(g_vag_flow.trailing_blocks_skipped),
        static_cast<unsigned long long>(g_vag_flow.terminal_flags[1]),
        static_cast<unsigned long long>(g_vag_flow.terminal_flags[3]),
        static_cast<unsigned long long>(g_vag_flow.terminal_flags[7]));
    for (std::size_t channel = 0u; channel < g_audio_channel_buffers.size(); ++channel) {
        if (g_audio_channel_buffers[channel] == 0u) continue;
        std::printf("  sceAudio channel=%zu buffers=%llu frames=%llu\n", channel,
                    static_cast<unsigned long long>(g_audio_channel_buffers[channel]),
                    static_cast<unsigned long long>(g_audio_channel_samples[channel]));
    }
}

void report_mpeg_flow() {
    std::printf("flujo PMF PSP: frames=%llu audio-bytes=%llu csc-crops=%llu contexts=%zu\n",
                static_cast<unsigned long long>(g_mpeg_video_frames),
                static_cast<unsigned long long>(g_mpeg_audio_bytes),
                static_cast<unsigned long long>(g_mpeg_csc_crops), g_mpeg.size());
}

void sas_mix_grain(psprecomp::Runtime& runtime, const std::uint32_t output,
                   const bool mix_existing, const std::uint32_t mix_left,
                   const std::uint32_t mix_right) {
    if (g_sas.output_mode != 0u) {
        runtime.memory().zero(output, static_cast<std::size_t>(g_sas.grain_size) * 8u);
        sas_advance_grain();
        return;
    }
    auto* bytes = runtime.memory().raw_pointer(
        output, static_cast<std::size_t>(g_sas.grain_size) * 4u);
    if (bytes == nullptr) return;
    auto* stereo = reinterpret_cast<std::int16_t*>(bytes);
    for (std::uint32_t frame = 0u; frame < g_sas.grain_size; ++frame) {
        std::int32_t left = mix_existing
            ? (static_cast<std::int32_t>(stereo[frame * 2u]) *
               static_cast<std::int32_t>(mix_left)) / 0x1000 : 0;
        std::int32_t right = mix_existing
            ? (static_cast<std::int32_t>(stereo[frame * 2u + 1u]) *
               static_cast<std::int32_t>(mix_right)) / 0x1000 : 0;
        for (std::size_t voice_index = 0u; voice_index < g_sas.voices.size();
             ++voice_index) {
            auto& voice = g_sas.voices[voice_index];
            if (!voice.playing || voice.paused || !voice.on ||
                voice.type != SasVoiceType::Vag || voice.pcm.empty())
                continue;
            std::size_t sample_index = static_cast<std::size_t>(
                voice.sample_cursor_12 >> 12u);
            if (sample_index >= voice.pcm.size()) {
                if (voice.loop) {
                    sas_trace("loop voice=%zu cursor=%llu samples=%zu loop-start=%zu valid=%u",
                              voice_index,
                              static_cast<unsigned long long>(voice.sample_cursor_12),
                              voice.pcm.size(), voice.loop_start_sample,
                              voice.loop_start_valid ? 1u : 0u);
                    const std::uint64_t end =
                        static_cast<std::uint64_t>(voice.pcm.size()) << 12u;
                    const std::uint64_t start =
                        static_cast<std::uint64_t>(
                            voice.loop_start_valid ? voice.loop_start_sample : 0u)
                        << 12u;
                    const std::uint64_t length = end - start;
                    voice.sample_cursor_12 = start +
                        (length == 0u ? 0u : (voice.sample_cursor_12 - end) % length);
                    sample_index = static_cast<std::size_t>(
                        voice.sample_cursor_12 >> 12u);
                } else {
                    sas_trace("end voice=%zu cursor=%llu samples=%zu", voice_index,
                              static_cast<unsigned long long>(voice.sample_cursor_12),
                              voice.pcm.size());
                    voice.on = false;
                    voice.playing = false;
                    voice.remaining_samples = 0u;
                    voice.envelope_height = 0u;
                    continue;
                }
            }
            const std::int32_t sample = voice.pcm[sample_index];
            left += sample * voice.volumes[0] / 0x1000;
            right += sample * voice.volumes[1] / 0x1000;
            voice.sample_cursor_12 += static_cast<std::uint32_t>(voice.pitch);
            const std::size_t next = static_cast<std::size_t>(
                voice.sample_cursor_12 >> 12u);
            voice.remaining_samples = next < voice.pcm.size()
                ? voice.pcm.size() - next : 0u;
            voice.envelope_height = kSasEnvelopeMaximum;
        }
        stereo[frame * 2u] = static_cast<std::int16_t>(
            std::clamp(left, -32768, 32767));
        stereo[frame * 2u + 1u] = static_cast<std::int16_t>(
            std::clamp(right, -32768, 32767));
    }
}

void report_ge_flow() {
    std::printf(
        "flujo GE PSP: jump=%llu bjump=%llu call=%llu ret=%llu end=%llu "
        "end-en-call=%llu caps=%llu loops=%llu invalid=%llu ret-sin-call=%llu "
        "profundidad=%zu\n",
        static_cast<unsigned long long>(g_ge_flow.jumps),
        static_cast<unsigned long long>(g_ge_flow.bjumps),
        static_cast<unsigned long long>(g_ge_flow.calls),
        static_cast<unsigned long long>(g_ge_flow.returns),
        static_cast<unsigned long long>(g_ge_flow.ends),
        static_cast<unsigned long long>(g_ge_flow.ends_inside_call),
        static_cast<unsigned long long>(g_ge_flow.execution_caps),
        static_cast<unsigned long long>(g_ge_flow.execution_loops),
        static_cast<unsigned long long>(g_ge_flow.invalid_addresses),
        static_cast<unsigned long long>(g_ge_flow.returns_without_call),
        g_ge_flow.max_call_depth);
    std::vector<std::pair<std::uint32_t, std::uint64_t>> targets(
        g_ge_flow.call_targets.begin(), g_ge_flow.call_targets.end());
    std::sort(targets.begin(), targets.end(),
              [](const auto& left, const auto& right) {
                  return left.second > right.second;
              });
    for (std::size_t index = 0u; index < std::min<std::size_t>(targets.size(), 32u);
         ++index) {
        std::printf("  GE CALL %s count=%llu\n", hex32(targets[index].first).c_str(),
                    static_cast<unsigned long long>(targets[index].second));
    }
    for (const auto& [start, count] : g_ge_flow.execution_cap_starts) {
        std::printf("  GE CAP start=%s count=%llu\n", hex32(start).c_str(),
                    static_cast<unsigned long long>(count));
    }
    for (const auto& [behavior, count] : g_ge_flow.signal_behaviors) {
        std::printf("  GE SIGNAL behavior=0x%02X count=%llu\n",
                    static_cast<unsigned>(behavior),
                    static_cast<unsigned long long>(count));
    }
}

void reset_ge_flow_stats() { g_ge_flow = {}; }

std::size_t thread_count() { return g_table.threads.size(); }

std::uint32_t display_framebuffer() { return g_framebuffer; }

std::uint32_t arena_used() { return g_table.arena_next; }

} // namespace fate
