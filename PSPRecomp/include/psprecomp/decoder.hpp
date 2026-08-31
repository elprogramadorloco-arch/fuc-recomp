#pragma once

#include <cstdint>
#include <string>

namespace psprecomp {

enum class OpcodeKind {
    Nop,
    Addiu, Slti, Sltiu, Andi, Ori, Xori, Lui,
    Add, Addu, Sub, Subu, And, Or, Xor, Nor, Slt, Sltu, Max, Min, Movz, Movn,
    Sll, Srl, Sra, Rotr, Sllv, Srlv, Srav, Rotrv, Clz, Clo, Ext, Ins, Seb, Seh, Bitrev, Wsbh, Wsbw, Sync,
    Lw, Lwl, Lwr, Sw, Swl, Swr, Lh, Lhu, Sh, Lb, Lbu, Sb, Lwc1, Swc1, Cache,
    Beq, Bne, Beql, Bnel, Blez, Bgtz, Blezl, Bgtzl, Bltz, Bgez, Bltzl, Bgezl, Bltzal, Bgezal, Bltzall, Bgezall, J, Jal, Jr, Jalr,
    Mfhi, Mflo, Mthi, Mtlo,
    Mult, Multu, Div, Divu,
    // Multiply-accumulate into the HI:LO pair, and the trap instruction.
    // Allegrex keeps all of these in SPECIAL (opcode 0), not in MIPS32's
    // SPECIAL2. Observed funct codes on real code: 0x1C madd, 0x2E msub,
    // 0x0D break. 0x1D maddu and 0x2F msubu follow the standard pairing and
    // did not occur in the module measured, so they are unverified here.
    Madd, Maddu, Msub, Msubu, Break,
    Mfc1, Mtc1, Cfc1, Ctc1,
    AddS, SubS, MulS, DivS, SqrtS, AbsS, MovS, NegS,
    RoundWS, TruncWS, CeilWS, FloorWS, CvtWS, CvtSW, FpuCompare,
    Bc1f, Bc1t, Bc1fl, Bc1tl, Bvf, Bvt, Bvfl, Bvtl,
    Mfv, Mtv, Vpfx, Viim, Vfim, Vh2f, Vf2h, Vf2i, Vi2f, Vx2i, VmidT, Vmmov, VfpuMatrixInit, Vidt, Vcst, Vocp, VfpuHorizontal, Vrot, Vtfm, VfpuVectorInit, VfpuVec3, Vscl, Vdot, Vhdp, Vcmp, Vminmax, VfpuCompare3, Vcmov, VfpuUnary, Vmmul, Vmscl, VcrossQuat, Lvs, Svs, Lvq, Svq,
    // Per-component sign: VFPU4 group 2, operation 10. It gets its own kind
    // instead of joining VfpuUnary because that lowering switches on the
    // operation field alone, and an operation number only means the same thing
    // within one group -- 10 in group 0 is not this.
    Vsgn,
    Syscall,
    Vflush,
    Vfpu,
    Unsupported,
};

struct DecodedInstruction {
    OpcodeKind kind{OpcodeKind::Unsupported};
    std::uint32_t word{};
    std::uint32_t rs{};
    std::uint32_t rt{};
    std::uint32_t rd{};
    std::uint32_t sa{};
    std::int16_t immediate{};
    std::uint32_t target{};
    std::string mnemonic;

    [[nodiscard]] bool is_control_flow() const noexcept;
    [[nodiscard]] bool has_delay_slot() const noexcept;
};

[[nodiscard]] DecodedInstruction decode_allegrex(std::uint32_t word);

} // namespace psprecomp
