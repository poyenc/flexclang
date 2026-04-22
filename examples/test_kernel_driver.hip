// examples/test_kernel_driver.hip
// HIP kernel for driver-mode validation.
// Uses __global__ (requires HIP language mode) but avoids #include <hip/hip_runtime.h>
// to work across LLVM/ROCm version mismatches. Compiled with --cuda-device-only
// -nogpulib to skip host compilation and device library linking.
//
// Exercises: global_load, v_mfma_f32_32x32x8f16, s_waitcnt, v_mov

using half4 = __attribute__((ext_vector_type(4))) _Float16;
using float16 = __attribute__((ext_vector_type(16))) float;

extern "C" __global__ void mfma_gemm_tile(const half4* __restrict__ A,
                                           const half4* __restrict__ B,
                                           float16* __restrict__ C,
                                           int K_tiles) {
  float16 acc = {0};

  for (int k = 0; k < K_tiles; ++k) {
    half4 a = A[k * 64];
    half4 b = B[k * 64];
    // MFMA: F32 += F16 * F16 (32x32x8)
    acc = __builtin_amdgcn_mfma_f32_32x32x8f16(a, b, acc, 0, 0, 0);
  }

  C[0] = acc;
}
