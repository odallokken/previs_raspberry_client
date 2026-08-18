/* =============================================================================
 *  armpl_compat.c — libamath.so replacement for the arm64 Pulse SDK
 * =============================================================================
 *
 *  The arm64 build of the Pexip Pulse SDK is compiled with the Arm Compiler
 *  for Linux, which redirects vectorised math calls to the Arm Performance
 *  Libraries.  The shipped libraries therefore carry
 *
 *      NEEDED  libamath.so
 *      NEEDED  libastring.so
 *
 *  and reference symbols such as 'armpl_vsinq_f32'.  Neither library is part
 *  of the pexninja package, nor of Ubuntu, so linking against libpexpulse.so
 *  fails with:
 *
 *      libamath.so, needed by /opt/pexninja/lib/libpexpulse.so, not found
 *      undefined reference to `armpl_vsinq_f32'
 *
 *  This file implements the referenced entry points on top of the C standard
 *  math library, one lane at a time.  The results are the ordinary libm
 *  results — slower than the Arm Performance Libraries, but correct.
 *
 *  scripts/install.sh compiles this into <sdk-prefix>/lib/libamath.so, plus an
 *  empty libastring.so (the routines in the real libastring are drop-in
 *  replacements for plain C string functions and are satisfied by glibc).  If
 *  the genuine Arm Performance Libraries are installed on the machine, the
 *  installer uses those instead and never builds this file.
 *
 *  Only built for aarch64.
 * ---------------------------------------------------------------------------
 */

#include <arm_neon.h>
#include <math.h>
#include <stdint.h>

/* --- helpers ------------------------------------------------------------- */

#define VEC_F32_UNARY(name, fn)                         \
    float32x4_t armpl_##name(float32x4_t x);            \
    float32x4_t armpl_##name(float32x4_t x)             \
    {                                                   \
        float v[4];                                     \
        vst1q_f32(v, x);                                \
        for (int i = 0; i < 4; ++i) v[i] = fn(v[i]);    \
        return vld1q_f32(v);                            \
    }

#define VEC_F32_BINARY(name, fn)                                    \
    float32x4_t armpl_##name(float32x4_t x, float32x4_t y);         \
    float32x4_t armpl_##name(float32x4_t x, float32x4_t y)          \
    {                                                               \
        float a[4], b[4];                                           \
        vst1q_f32(a, x);                                            \
        vst1q_f32(b, y);                                            \
        for (int i = 0; i < 4; ++i) a[i] = fn(a[i], b[i]);          \
        return vld1q_f32(a);                                        \
    }

#define VEC_F64_UNARY(name, fn)                         \
    float64x2_t armpl_##name(float64x2_t x);            \
    float64x2_t armpl_##name(float64x2_t x)             \
    {                                                   \
        double v[2];                                    \
        vst1q_f64(v, x);                                \
        for (int i = 0; i < 2; ++i) v[i] = fn(v[i]);    \
        return vld1q_f64(v);                            \
    }

#define VEC_F64_BINARY(name, fn)                                    \
    float64x2_t armpl_##name(float64x2_t x, float64x2_t y);         \
    float64x2_t armpl_##name(float64x2_t x, float64x2_t y)          \
    {                                                               \
        double a[2], b[2];                                          \
        vst1q_f64(a, x);                                            \
        vst1q_f64(b, y);                                            \
        for (int i = 0; i < 2; ++i) a[i] = fn(a[i], b[i]);          \
        return vld1q_f64(a);                                        \
    }

/* --- single precision ---------------------------------------------------- */

VEC_F32_UNARY(vacosq_f32,  acosf)
VEC_F32_UNARY(vacoshq_f32, acoshf)
VEC_F32_UNARY(vasinq_f32,  asinf)
VEC_F32_UNARY(vasinhq_f32, asinhf)
VEC_F32_UNARY(vatanq_f32,  atanf)
VEC_F32_UNARY(vatanhq_f32, atanhf)
VEC_F32_UNARY(vcbrtq_f32,  cbrtf)
VEC_F32_UNARY(vcosq_f32,   cosf)
VEC_F32_UNARY(vcoshq_f32,  coshf)
VEC_F32_UNARY(vexpq_f32,   expf)
VEC_F32_UNARY(vexp2q_f32,  exp2f)
VEC_F32_UNARY(vexpm1q_f32, expm1f)
VEC_F32_UNARY(vlogq_f32,   logf)
VEC_F32_UNARY(vlog10q_f32, log10f)
VEC_F32_UNARY(vlog1pq_f32, log1pf)
VEC_F32_UNARY(vlog2q_f32,  log2f)
VEC_F32_UNARY(vsinq_f32,   sinf)
VEC_F32_UNARY(vsinhq_f32,  sinhf)
VEC_F32_UNARY(vsqrtq_f32,  sqrtf)
VEC_F32_UNARY(vtanq_f32,   tanf)
VEC_F32_UNARY(vtanhq_f32,  tanhf)

VEC_F32_BINARY(vatan2q_f32, atan2f)
VEC_F32_BINARY(vfmodq_f32,  fmodf)
VEC_F32_BINARY(vhypotq_f32, hypotf)
VEC_F32_BINARY(vpowq_f32,   powf)

/* --- double precision ---------------------------------------------------- */

VEC_F64_UNARY(vacosq_f64,  acos)
VEC_F64_UNARY(vacoshq_f64, acosh)
VEC_F64_UNARY(vasinq_f64,  asin)
VEC_F64_UNARY(vasinhq_f64, asinh)
VEC_F64_UNARY(vatanq_f64,  atan)
VEC_F64_UNARY(vatanhq_f64, atanh)
VEC_F64_UNARY(vcbrtq_f64,  cbrt)
VEC_F64_UNARY(vcosq_f64,   cos)
VEC_F64_UNARY(vcoshq_f64,  cosh)
VEC_F64_UNARY(vexpq_f64,   exp)
VEC_F64_UNARY(vexp2q_f64,  exp2)
VEC_F64_UNARY(vexpm1q_f64, expm1)
VEC_F64_UNARY(vlogq_f64,   log)
VEC_F64_UNARY(vlog10q_f64, log10)
VEC_F64_UNARY(vlog1pq_f64, log1p)
VEC_F64_UNARY(vlog2q_f64,  log2)
VEC_F64_UNARY(vsinq_f64,   sin)
VEC_F64_UNARY(vsinhq_f64,  sinh)
VEC_F64_UNARY(vsqrtq_f64,  sqrt)
VEC_F64_UNARY(vtanq_f64,   tan)
VEC_F64_UNARY(vtanhq_f64,  tanh)

VEC_F64_BINARY(vatan2q_f64, atan2)
VEC_F64_BINARY(vfmodq_f64,  fmod)
VEC_F64_BINARY(vhypotq_f64, hypot)
VEC_F64_BINARY(vpowq_f64,   pow)

/* ldexp takes an integer exponent vector, so it does not fit the macros. */

float32x4_t armpl_vldexpq_f32(float32x4_t x, int32x4_t n);
float32x4_t armpl_vldexpq_f32(float32x4_t x, int32x4_t n)
{
    float   v[4];
    int32_t e[4];
    vst1q_f32(v, x);
    vst1q_s32(e, n);
    for (int i = 0; i < 4; ++i) v[i] = ldexpf(v[i], (int) e[i]);
    return vld1q_f32(v);
}

float64x2_t armpl_vldexpq_f64(float64x2_t x, int64x2_t n);
float64x2_t armpl_vldexpq_f64(float64x2_t x, int64x2_t n)
{
    double  v[2];
    int64_t e[2];
    vst1q_f64(v, x);
    vst1q_s64(e, n);
    for (int i = 0; i < 2; ++i) v[i] = ldexp(v[i], (int) e[i]);
    return vld1q_f64(v);
}
