#ifndef SYSTOLIC_MESH_H
#define SYSTOLIC_MESH_H

#include <riscv-pk/encoding.h>

/**
 * Systolic Mesh Control API
 * Controls the global stall and enables the systolic mesh logic.
 */

#define SYSTOLIC_CTRL_CSR 0x800

/**
 * @brief Stall all slave cores in the mesh.
 * Only effective when called from the Head Node (Core 0).
 */
static inline void systolic_stall_slaves() {
    write_csr(SYSTOLIC_CTRL_CSR, 1);
}

/**
 * @brief Release all slave cores in the mesh.
 * Only effective when called from the Head Node (Core 0).
 */
static inline void systolic_release_slaves() {
    write_csr(SYSTOLIC_CTRL_CSR, 0);
}

/**
 * @brief Check if the current core is the Head Node.
 */
static inline int systolic_is_head_node() {
    return read_csr(mhartid) == 0;
}

/**
 * @brief Simple busy-wait delay.
 */
static inline void delay(int count) {
    for (volatile int i = 0; i < count; i++);
}

/**
 * @brief Wait for a specific core to reach a barrier.
 */
static inline void systolic_barrier(int n_cores) {
    uint64_t mhartid = read_csr(mhartid);
    // Simple busy-wait serialized print/synchronization logic
    for (int i = 0; i < n_cores; i++) {
        if (mhartid == (uint64_t)i) {
            // My turn
        }
        delay(100000);
    }
}
/**
 * Systolic ALU-to-ALU Instructions
 * SYSTOLIC_MUL rd, rs1: rd = mesh_west * rs1
 *   - mesh_west is injected as MulDiv in2 (replaces rs2)
 *   - Auto-forwards: mesh_west → east_out, rd result → south_out
 *   - Encoding: funct7=0000000, rs2=x0, rs1, funct3=000, rd, opcode=1011011
 *   - Opcode 0x5b = CUSTOM2
 */
#define SYSTOLIC_MUL(rd, rs1)  \
    ".insn r 0x5b, 0x0, 0x00, " rd ", " rs1 ", x0\n\t"

/**
 * SYSTOLIC_FMUL_S fd, fs1: fd = mesh_west * fs1 (single-precision FP)
 *   - Encoding: funct7=0000100, rs2=f0, fs1, funct3=000, fd, opcode=1011011
 */
#define SYSTOLIC_FMUL_S(fd, fs1)  \
    ".insn r 0x5b, 0x0, 0x04, " fd ", " fs1 ", f0\n\t"

/**
 * SYSTOLIC_FMAC_S fd, fs1: fd = mesh_west * fs1 + mesh_north (single-precision FP FMA)
 *   - Encoding: funct7=0001000, rs2=f0, fs1, funct3=000, fd, opcode=1011011
 *   - funct7=0001000 ensures inst[26:25]=01 (single-precision format) for FPU compatibility
 */
#define SYSTOLIC_FMAC_S(fd, fs1)  \
    ".insn r 0x5b, 0x0, 0x08, " fd ", " fs1 ", f0\n\t"

#endif // SYSTOLIC_MESH_H
