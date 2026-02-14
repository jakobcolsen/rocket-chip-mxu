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

#endif // SYSTOLIC_MESH_H
