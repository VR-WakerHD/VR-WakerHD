// The Picto Box at other aspect ratios (issue #303): pure math shared by the guest hooks
// (aspect.cpp) and their test (runtime/tools/aspect_picto_test.cpp).
//
// At another aspect ratio the TV picture is the 16:9 picture widened (Hor+) or made taller (Vert+):
// its 16:9 part is 1/kx of its width and 1/ky of its height (kx, ky as aspect.cpp's factors: one
// of them is 1), centred unless the projection is off-centre (crop_to_16x9). The Picto Box's finder is a TV layout, which stays at its 16:9 place
// in the centre. The game takes the photo from the TV picture through a source rectangle in texture
// coordinates (dSnap_packet::draw: the finder's frame within the 1280x720 screen), and finds its
// subjects by drawing them into the photo's alpha channel with the TV projection narrowed to that
// frame (dSnap_packet::DebugDraw). Both assume a 16:9 TV picture. The hooks give the photo the same
// part of the scene as on a Wii U: the rectangle is moved into the 16:9 part of the wider picture,
// and the subject projection gets the 16:9 picture's scale back.
#pragma once

namespace aspect::picto {

// A source rectangle in texture coordinates of the 16:9 picture, as the game passes it to its
// texture copy (the copy reads at position + t * scale, t running over the destination, in each
// axis): the same rectangle in the k times wider (taller) picture that holds the 16:9 picture.
// shift: the projection's off-centre term along that axis, as texture coordinates grow (x: -m[0][2],
// y: +m[1][2] for a GX projection, v pointing down). The port widens (or heightens) the view by
// scaling m[0][0] (m[1][1]) only, which scales the picture about the projection's centre, not about
// the screen centre: with a shift (the Picto Box's camera has m[1][2] = 0.35, its finder sits above
// the screen centre) the 16:9 part is centred at 0.5 + shift / 2 * (1 - 1/k), not at 0.5.
// k = 1 leaves the values bit for bit.
inline void crop_to_16x9(float& position, float& scale, float k, float shift = 0.0f) {
    if (k == 1.0f) return;
    position = 0.5f + (position - 0.5f) / k + 0.5f * shift * (1.0f - 1.0f / k);
    scale /= k;
}

// The TV projection (row-major 4x4, rows x y z w) of a picture kx times wider and ky times taller
// than 16:9, narrowed to its 16:9 part: the 16:9 projection's x and y scale. Hor+ widened the
// picture by dividing m[0][0] by kx; Vert+ made it taller by dividing m[1][1] by ky (and m[0][0] with
// it, which the narrower aspect undid). The rest of the matrix (offsets, depth) is the same.
inline void narrow_to_16x9(float m[16], float kx, float ky) {
    if (kx != 1.0f) m[0] *= kx;
    if (ky != 1.0f) m[5] *= ky;
}

// (VR fork, docs/VR_PICTO_BOX.md) The same narrowing about the screen's centre instead of the projection's: the 16:9
// part in the middle of the k times wider (taller) picture. A projection narrowed to the finder's frame maps the TV
// picture's coordinates n to a * n + b; the TV picture's own are n = s * p + o (s its scale, o its off-centre term),
// so the frame's matrix holds a * s (`scale`) and a * o + b (`offset`). The middle part is k * n: there its matrix
// holds a * k * s (narrow_to_16x9) and a * k * o + b, the offset plus a * (k - 1) * o, a = scale / s at the
// narrowing. tvScale 0 (no TV projection seen): the offset as it is.
// (VR fork) A source rectangle of the 16:9 screen (position, scale along one axis, as crop_to_16x9) moved into an eye's
// picture that covers its own field, off-centre against the field the 16:9 layout space is centred in (the UI
// layer's, the game camera's). Along that axis: `share` of the field's extent is the 16:9 screen's (its viewfinder
// size included), `half` the field's half extent as a tangent, and the eye's picture runs from tangent `start` to
// `end` (x: left to right; y: up to down, the tangents then negated by the caller). Linear in the tangents, as both are.
inline void to_eye_picture(float& position, float& scale, float share, float half, float start, float end) {
    const float w = end - start;
    if (!(w != 0.0f)) return;
    const float k = 2.0f * share * half;
    position = ((position - 0.5f) * k - start) / w;
    scale = scale * k / w;
}

inline float centred_offset(float offset, float scale, float tvScale, float tvOffset, float k) {
    if (k == 1.0f || tvScale == 0.0f) return offset;
    return offset + scale / tvScale * (k - 1.0f) * tvOffset;
}

}  // namespace aspect::picto
