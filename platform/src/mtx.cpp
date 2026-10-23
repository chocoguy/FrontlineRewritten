// MTX: the SDK matrix functions the game calls (in C; the console versions are
// paired-single assembly with the same results up to rounding).

#include <cmath>

#include "dolphin/gx.h"

extern "C" {

void PSMTXIdentity(f32 m[3][4]) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            m[r][c] = r == c ? 1.0f : 0.0f;
        }
    }
}

// The inverse transpose of the upper 3x3 (for normals); 0 if it's singular.
u32 PSMTXInvXpose(const f32 src[3][4], f32 invX[3][4]) {
    f32 det = src[0][0] * src[1][1] * src[2][2] + src[0][1] * src[1][2] * src[2][0] +
              src[0][2] * src[1][0] * src[2][1] - src[2][0] * src[1][1] * src[0][2] -
              src[1][0] * src[0][1] * src[2][2] - src[0][0] * src[2][1] * src[1][2];
    if (det == 0.0f) {
        return 0;
    }
    det = 1.0f / det;
    f32 m[3][4];
    m[0][0] = (src[1][1] * src[2][2] - src[2][1] * src[1][2]) * det;
    m[0][1] = -(src[1][0] * src[2][2] - src[2][0] * src[1][2]) * det;
    m[0][2] = (src[1][0] * src[2][1] - src[2][0] * src[1][1]) * det;
    m[1][0] = -(src[0][1] * src[2][2] - src[2][1] * src[0][2]) * det;
    m[1][1] = (src[0][0] * src[2][2] - src[2][0] * src[0][2]) * det;
    m[1][2] = -(src[0][0] * src[2][1] - src[2][0] * src[0][1]) * det;
    m[2][0] = (src[0][1] * src[1][2] - src[1][1] * src[0][2]) * det;
    m[2][1] = -(src[0][0] * src[1][2] - src[1][0] * src[0][2]) * det;
    m[2][2] = (src[0][0] * src[1][1] - src[1][0] * src[0][1]) * det;
    for (int r = 0; r < 3; r++) {
        m[r][3] = 0.0f;
        for (int c = 0; c < 4; c++) {
            invX[r][c] = m[r][c];
        }
    }
    return 1;
}

void PSMTXMultVec(const f32 m[3][4], const f32* src, f32* dst) {
    f32 x = src[0], y = src[1], z = src[2];
    dst[0] = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
    dst[1] = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
    dst[2] = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
}

void C_MTXFrustum(f32 m[4][4], f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = 2 * n * tmp;
    m[0][1] = 0.0f;
    m[0][2] = (r + l) * tmp;
    m[0][3] = 0.0f;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = 2 * n * tmp;
    m[1][2] = (t + b) * tmp;
    m[1][3] = 0.0f;
    tmp = 1.0f / (f - n);
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -n * tmp;
    m[2][3] = -(f * n) * tmp;
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = -1.0f;
    m[3][3] = 0.0f;
}

void C_MTXOrtho(f32 m[4][4], f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = -(r + l) * tmp;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = 2.0f * tmp;
    m[1][2] = 0.0f;
    m[1][3] = -(t + b) * tmp;
    tmp = 1.0f / (f - n);
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -tmp;
    m[2][3] = -f * tmp;
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = 0.0f;
    m[3][3] = 1.0f;
}

}  // extern "C"
