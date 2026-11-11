#pragma once
#include <cmath>
#include <cstring>
#include <cstddef>
namespace stereo {
// (docs/VR_EYE_PROJECTION.md) an eye's own field of view within the symmetric one the game's camera draws, as a scale
// and an offset of the projection's x and y rows: the eye's picture is the symmetric one's normalized device
// coordinates mapped by x' = sx * x - ox, y' = sy * y - oy (only its own part, at its own pixels). For tangents l, r, u,
// d of the eye's edges in units of the symmetric half-field: sx = 2 / (r - l), ox = (r + l) / (r - l), and so for y.
// The default leaves every value exactly as it was (the eyes drawn with the symmetric field)
struct EyeFrustum {
    float sx = 1, ox = 0, sy = 1, oy = 0;
    bool identity() const { return sx == 1 && ox == 0 && sy == 1 && oy == 0; }
};
// The game's sun-shadow pass reconstructs negative-Z view-space positions.
// Its VS remapped[2].z is subtracted from all three reconstruction rays.
// Its PS remapped[6..17] contains three view-to-shadow matrices. Reconstruct
// relative to the displaced eye, then translate back into the centre camera's
// space before the original light matrices. This also keeps gather normals
// consistent with the off-axis projection used to draw the scene depth.
// (docs/VR_EYE_PROJECTION.md) Its rays are view-space directions at the corners: ray.x = x_ndc * -[1].z - [2].z (y: .w).
// For an eye drawn with its own frustum (x' = sx * x - ox), the ray at x' is the symmetric picture's at (x' + ox) / sx:
// [1].z / sx and [2].z + [1].z * ox / sx (y the same with sy, oy).
inline bool shadow_uniforms(unsigned char* bytes, size_t size, bool vertex,
                            float eye, float convergence, const EyeFrustum& f = {}) {
    if (size < (vertex ? 48u : 288u)) return false;
    if (vertex) {
        if (!f.identity()) {
            float scale[2], offset[2];
            std::memcpy(scale,bytes+24,8);std::memcpy(offset,bytes+40,8);
            offset[0]+=scale[0]*f.ox/f.sx;offset[1]+=scale[1]*f.oy/f.sy;
            scale[0]/=f.sx;scale[1]/=f.sy;
            std::memcpy(bytes+24,scale,8);std::memcpy(bytes+40,offset,8);
        }
        float value;std::memcpy(&value,bytes+40,4);
        value-=eye/convergence;std::memcpy(bytes+40,&value,4);
    } else {
        for (size_t row=6;row<18;++row) {
            float value[4];std::memcpy(value,bytes+row*16,16);
            value[3]+=value[0]*eye;std::memcpy(bytes+row*16,value,16);
        }
    }
    return true;
}
// The point-light passes (gfx::ProgramKind::PointLight: agl's light pre-pass, the torches' and the searchlights' light;
// VS 44390500 / PS 44390C00, VS 44461600 / PS 44461F00) rebuild each pixel's view-space position from the depth copy
// as the sun shadow does: P = ray * -linear depth. Their light volumes are drawn with the camera's projection (shifted
// for the eye like any other), but the ray and the light's place were the centre camera's, so each eye lit its own
// wrong spot: a searchlight's pool apart from its beam, a torch's light gone from one eye (docs/VR_EYE_PROJECTION.md,
// "Point lights").
// VS remapped[5]: ray.x = x_ndc * -[5].x - [5].z, ray.y = -y_ndc * -[5].y - [5].w (y flipped, unlike the sun's). For an
// eye drawn with its own frustum (x' = sx * x - ox): [5].x / sx and [5].z + [5].x * ox / sx; y, flipped: [5].y / sy and
// [5].w - [5].y * oy / sy. The eye's displacement as the sun's ray: [5].z - eye / convergence.
// PS remapped[1].xyz: the light's view-space place; the rebuilt position is the displaced eye's (as the sun's), so the
// light is moved into that eye's space: [1].x - eye (the centre camera's x is the eye's + eye, project_eye).
// (2026-11-05, a Quest 3 player: both torches lit in both eyes, but "shining in different directions with each eye")
// The PS shift is left out by default (shiftLight false): if remapped[1] is the light's place it moves it by the eye's
// offset only (3 game units, a few centimetres against a light hundreds of units across: unseen), but the round
// lights' layout was taken for the spotlights' program too (PS 44461F00, another layout), and were [1] a direction
// there, taking 3 units off its x turns the light one way in the left eye and the other in the right. The VS ray, what
// puts each eye's light where it falls, is kept. WWHD_VK_POINT_LIGHT_EYES (draw.cpp) chooses
inline bool point_light_uniforms(unsigned char* bytes, size_t size, bool vertex,
                                 float eye, float convergence, const EyeFrustum& f = {}, bool shiftLight = false) {
    if (size < (vertex ? 96u : 32u)) return false;
    if (vertex) {
        float ray[4];
        std::memcpy(ray,bytes+80,16);
        for (float v : ray) if (!std::isfinite(v)) return false;
        // (2026-11-07, PC VR on a PS VR2: "light going in opposite directions" with the ray alone) The light volume
        // itself: VS 44390500's GLSL (WWHD_VK_DUMP_GLSL) puts the volume with the combined matrix's rows in
        // remapped[1..4] (w = [1], y = [2], x = [3], z = [4]): not a camera projection, so nothing moved it for the eye,
        // and both eyes drew it where the centre camera's symmetric picture has it (logged: the same rows in both eyes).
        // With each eye's own field (ox 0.32 one way in one eye, the other way in the other) the lit shape was off to
        // opposite sides of where each eye's ray put the light. Moved as project_eye moves a projection: the x row
        // x' = sx * x - ox * w, and the eye's displacement -eye * sx * p0 (p0 / convergence: none at infinity);
        // y' = sy * y - oy * w. p0 and p5 are the rays' 1 / [5].x and 1 / -[5].y; the rows are taken for the layout
        // only where their lengths agree with those (a rotation keeps them: within 2%), else left as they were.
        float w[4], y[4], x[4];
        std::memcpy(w,bytes+16,16);std::memcpy(y,bytes+32,16);std::memcpy(x,bytes+48,16);
        const float p0 = ray[0] != 0 ? 1.0f/ray[0] : 0, p5 = ray[1] != 0 ? -1.0f/ray[1] : 0;
        const float xLen = std::sqrt(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]), yLen = std::sqrt(y[0]*y[0]+y[1]*y[1]+y[2]*y[2]);
        const bool rows = p0 > 0 && p5 > 0 && std::fabs(xLen-p0) <= 0.02f*p0 && std::fabs(yLen-p5) <= 0.02f*p5;
        if (rows) {
            for (int i=0;i<4;++i) x[i] = f.sx*x[i] - f.ox*w[i];
            x[3] -= eye*f.sx*p0;
            if (std::isfinite(convergence) && convergence != 0)
                for (int i=0;i<4;++i) x[i] -= eye*f.sx*p0/convergence*(-w[i]);
            for (int i=0;i<4;++i) y[i] = f.sy*y[i] - f.oy*w[i];
            std::memcpy(bytes+32,y,16);std::memcpy(bytes+48,x,16);
        }
        if (!f.identity()) {
            ray[2]+=ray[0]*f.ox/f.sx;ray[3]-=ray[1]*f.oy/f.sy;
            ray[0]/=f.sx;ray[1]/=f.sy;
        }
        ray[2]-=eye/convergence;
        std::memcpy(bytes+80,ray,16);
    } else {
        if (!shiftLight) return true;
        float light[4];
        std::memcpy(light,bytes+16,16);
        if (!std::isfinite(light[0])) return false;
        light[0]-=eye;
        std::memcpy(bytes+16,light,16);
    }
    return true;
}
// GX2's row-major perspective matrix. Validate all four rows so orthographic
// layouts, world transforms, and light-space texture matrices are not shifted.
inline bool camera_projection(const float* p) {
    // Scanned at every 16-byte offset of every vertex uniform block, per draw and eye: the w row
    // rejects almost all of them before the remaining tests.
    if (p[14]!=-1 || p[15]!=0 || p[12]!=0 || p[13]!=0) return false;
    if (!(p[0]>0 && p[5]>0 && p[1]==0 && p[2]==0 && p[3]==0 &&
          p[4]==0 && p[6]==0 && p[7]==0 && p[8]==0 && p[9]==0 && p[10]<=-1 && p[11]<0)) return false;
    for (int i=0;i<16;++i) if (!std::isfinite(p[i])) return false;
    return true;
}
inline bool project_eye(float* p, float eye, float convergence, const EyeFrustum& f = {}) {
    if (!camera_projection(p)) return false;
    if (!f.identity()) {
        p[0] *= f.sx;
        p[2] += f.ox;
        p[5] *= f.sy;
        p[6] += f.oy;
    }
    const float projectedEye = eye*p[0];
    p[2] -= projectedEye/convergence;
    p[3] -= projectedEye;
    return true;
}
// The x rows project_eye shifted in a packed buffer, with their unshifted values: the other eye's
// bytes are the same buffer with these rows shifted for its eye (apply_eye_rows), exactly as
// project_eye computes them.
struct EyeRows {
    // offset: the x row's place; yOffset: the y row's (16 after it in a whole matrix; a remapped layout copies rows on
    // their own: kNoRow where the shader has no y row there), for the eye's own field of view (EyeFrustum)
    static constexpr unsigned kNoRow = ~0u;
    struct Row { unsigned offset; float p0, p2, p3, p5; unsigned yOffset; };
    Row rows[16];
    int count = 0;
    bool overflow = false;
    // (docs/VR_EYE_PROJECTION.md) a layout's rows (HUD, menus, text) in an eye's picture: not shifted for the eye, but the
    // eye replaying them takes its own frustum (marked here, not by the replaying thread's draw: eye 1's copy may be
    // replayed on another thread, which then left eye 1's menus where eye 0's were and showed them twice)
    bool layout = false;
    // the same rows in each of `repeat` copies of the layout, `stride` bytes apart (a batched run's pieces:
    // gfx/draw_batch.h)
    unsigned repeat = 1, stride = 0;
    // (unshifted: the matrix's first row, p0..p3; p5 its y scale)
    void add(size_t offset, const float* unshifted, float p5 = 0, unsigned yOffset = kNoRow) {
        if (count < 16) rows[count++] = {unsigned(offset), unshifted[0], unshifted[2], unshifted[3], p5, yOffset};
        else overflow = true;
    }
};
inline void apply_eye_rows(unsigned char* bytes, const EyeRows& rows, float eye, float convergence,
                           const EyeFrustum& f = {}) {
    for (unsigned copy=0;copy<rows.repeat;++copy)
    for (int i=0;i<rows.count;++i) {
        const auto& row = rows.rows[i];
        unsigned char* at = bytes+size_t(copy)*rows.stride+row.offset;
        if (!f.identity()) {
            // (as project_eye: the eye's own field of view; the unshifted y offset is 0, camera_projection)
            const float p0 = row.p0*f.sx, projectedEye = eye*p0;
            const float x[4] = {p0, 0.0f, row.p2 + f.ox - projectedEye/convergence, row.p3 - projectedEye};
            const float y[2] = {row.p5*f.sy, f.oy};
            std::memcpy(at, x, sizeof x);
            if (row.yOffset != EyeRows::kNoRow)
                std::memcpy(bytes+size_t(copy)*rows.stride+row.yOffset+4, y, sizeof y);
            continue;
        }
        const float projectedEye = eye*row.p0;
        const float shifted[2] = {row.p2 - projectedEye/convergence, row.p3 - projectedEye};
        std::memcpy(at+8, shifted, sizeof shifted);
    }
}
// The first camera projection in a block (copied into out); false if none (docs/VR_FRAME_SYNTHESIS.md: its planes).
inline bool find_camera_projection(const unsigned char* bytes, size_t size, float* out) {
    for (size_t at=0;at+64<=size;at+=16) {
        std::memcpy(out,bytes+at,64);
        if (camera_projection(out)) return true;
    }
    return false;
}
// Whether project_uniform_block would change the block (read in place: no copy without a camera).
inline bool has_camera_projection(const unsigned char* bytes, size_t size) {
    for (size_t at=0;at+64<=size;at+=16) {
        float p[16];std::memcpy(p,bytes+at,64);
        if (camera_projection(p)) return true;
    }
    return false;
}
inline void project_uniform_block(unsigned char* bytes, size_t size, float eye, float convergence,
                                  const void* excludedProjection = nullptr,
                                  EyeRows* rows = nullptr, size_t rowBase = 0, const EyeFrustum& f = {}) {
    for (size_t at=0;at+64<=size;at+=16) {
        if (excludedProjection && !std::memcmp(bytes+at,excludedProjection,64)) continue;
        float p[16];std::memcpy(p,bytes+at,64);
        const float unshifted[4] = {p[0],p[1],p[2],p[3]};
        const float unshiftedY = p[5];
        if (project_eye(p,eye,convergence,f)) {
            if (rows) rows->add(rowBase+at,unshifted,unshiftedY,unsigned(rowBase+at+16));
            std::memcpy(bytes+at,p,f.identity() ? 16 : 32);  // (the eye's own field of view: the y row too)
            at+=48;
        }
    }
}
}
