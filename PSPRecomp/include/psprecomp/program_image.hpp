#pragma once

#include <cstdint>
#include <vector>

namespace psprecomp {

// Input abstraction for program analysis.
//
// `analyze_program` used to take an `Elf32Image` directly, but it only ever read
// four things out of it: which PT_LOAD segments exist and whether each one is
// executable, where each lands at runtime, the entry point, and the R_MIPS_32
// relocation sites. Everything else -- instruction decoding, module info, the
// import tables -- is read from `GuestMemory` once the image has been loaded.
//
// Naming those four facts explicitly lets a second kind of input produce them.
// The case that motivated this is a program image that an emulator has already
// loaded and relocated, where there is no ELF file to parse but the loaded
// layout is fully known.
//
// This is deliberately the minimum measured set and not a general "loaded
// module" description: imports, exports, module info and relocation state are
// absent because analysis does not need them.
struct ImageRegion {
    // Runtime address. Already relocated: no load base is added.
    std::uint32_t start{};
    // Bytes backed by the image. This is the ELF `file_size`, not `memory_size`,
    // because analysis must not walk into BSS.
    std::uint32_t size{};
    bool executable{};
};

struct ProgramImageView {
    std::vector<ImageRegion> regions;
    // Runtime entry address, used as the first analysis seed.
    std::uint32_t entry{};
    // Addresses patched by R_MIPS_32 relocations. These are the authoritative
    // source for function pointers held in read-only tables, so they sharpen
    // seed discovery -- but they are optional. An already-relocated image may
    // not be able to recover them, and analysis still works without them
    // because data words are also scanned heuristically.
    std::vector<std::uint32_t> r_mips32_patch_addresses;

    // Function entry points already known from outside the image, used directly
    // as analysis seeds.
    //
    // This exists because dropping the relocations is not free. Measured on a
    // 3.4 MB module analysed without them: 45 of the 3852 functions an emulator
    // had found were never reached, and disassembling them showed why -- they
    // are trivial `jr ra` stubs and thin wrappers around imports, called only
    // through pointer tables, which is precisely what R_MIPS_32 sites would have
    // pointed at. Somewhere that already knows those entry points can hand them
    // over here and recover the shortfall.
    //
    // Note for whoever scores a run: seeding from a list and then measuring
    // coverage against that same list is circular. The point of this field is
    // that the analysis reaches the code, not that the number looks good.
    std::vector<std::uint32_t> extra_seed_addresses;
};

} // namespace psprecomp
