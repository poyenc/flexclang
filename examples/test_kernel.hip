// examples/test_kernel.hip
// Minimal kernel for flexclang validation -- no HIP runtime headers needed.
// Uses __attribute__((amdgpu_kernel)) and MFMA builtins directly so this
// compiles with cc1-level invocation (no driver layer).
//
// Exercises: global_load, v_mfma_f32_32x32x8f16, s_nop, v_mov, s_waitcnt

using half4 = __attribute__((ext_vector_type(4))) _Float16;
using float16 = __attribute__((ext_vector_type(16))) float;

extern "C" __attribute__((amdgpu_kernel)) void mfma_gemm_tile(
    const half4* __restrict__ A,
    const half4* __restrict__ B,
    float16* __restrict__ C,
    int K_tiles) {
  // Accumulator (16 floats per thread for 32x32 output tile)
  float16 acc = {0};

  for (int k = 0; k < K_tiles; ++k) {
    half4 a = A[k * 64];
    half4 b = B[k * 64];
    // MFMA: F32 += F16 * F16 (32x32x8)
    acc = __builtin_amdgcn_mfma_f32_32x32x8f16(a, b, acc, 0, 0, 0);
  }

  C[0] = acc;
}
