// examples/test_kernel_driver.hip
// Full HIP kernel for driver-mode validation -- requires HIP runtime headers.
// Uses __global__, __shared__, __syncthreads(), and MFMA builtins.
//
// Exercises: global_load, ds_read/write, v_mfma_f32_32x32x8f16, s_waitcnt,
//            __syncthreads (s_barrier), shared memory

#include <hip/hip_runtime.h>

using half4 = __attribute__((ext_vector_type(4))) _Float16;
using float16 = __attribute__((ext_vector_type(16))) float;

// Minimal kernel that guarantees MFMA instructions in the output.
// Uses v_mfma_f32_32x32x8f16: 32x32 output tile with K=8,
// each thread in wave64 holds 1024/64 = 16 floats (float16).
// Exercises: global_load_dwordx4, ds_read, v_mfma_f32_32x32x8f16,
//            v_mov, s_waitcnt
__global__ void mfma_gemm_tile(const half4* __restrict__ A,
                               const half4* __restrict__ B,
                               float16* __restrict__ C,
                               int K_tiles) {
  int tx = threadIdx.x;

  // Shared memory for A and B tiles
  __shared__ half4 tileA[64];
  __shared__ half4 tileB[64];

  // Accumulator (16 floats per thread for 32x32 output tile)
  float16 acc = {0};

  for (int k = 0; k < K_tiles; ++k) {
    // Global load -> LDS
    tileA[tx] = A[blockIdx.y * K_tiles * 64 + k * 64 + tx];
    tileB[tx] = B[blockIdx.x * K_tiles * 64 + k * 64 + tx];
    __syncthreads();

    // MFMA: F32 += F16 * F16 (32x32x8)
    // Each thread contributes to a 32x32 output tile
    for (int i = 0; i < 4; ++i) {
      half4 a = tileA[i * 16 + tx % 16];
      half4 b = tileB[i * 16 + tx / 16];
      acc = __builtin_amdgcn_mfma_f32_32x32x8f16(a, b, acc, 0, 0, 0);
    }
    __syncthreads();
  }

  C[blockIdx.y * gridDim.x * 64 + blockIdx.x * 64 + tx] = acc;
}
