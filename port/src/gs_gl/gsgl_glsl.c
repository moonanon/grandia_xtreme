/* The GPU renderer's shaders (port/src/gs_gl/, port/docs/gpu.md).

   Conventions shared with gsgl.c:
   - The local memory mirror (u_vram) is a 1024 x 1024 R32UI texture: word
     a of local memory at (a & 1023, a >> 10), so page p is rows 2p, 2p+1.
   - u_sw holds the swizzle tables of gs_mem.c (R16UI, 128 x 512): pg32
     rows 0-63 (Z layout from 32), pg16 rows 64-319 (CT16, CT16S, Z16,
     Z16S), pg8 rows 320-383, pg4 rows 384-511. u_inv the inverse ones
     (R16UI, 4096 x 6, x | y << 8): row 0-1 a 32-bit page's words (CT, Z),
     rows 2-5 a 16-bit page's halfwords.
   - Layout and kind numbers are gs_internal.h's GS_L* and GS_K*.
   - Render targets keep GS pixels as they are in memory: RGBA8 with the
     colour bytes and the alpha byte as stored (0x80 = 1.0), 16-bit ones
     expanded (5 bits << 3, alpha 0x80 or 0); depth targets the Z value
     over 2^32 in a 32-bit float. Texel row 0 is the buffer's row 0. */

const char gsgl_vram_glsl[] =
    "uniform usampler2D u_vram;\n"
    "uniform usampler2D u_sw;\n"
    "uint vword(uint a) { return texelFetch(u_vram, ivec2(int(a & 1023u), int(a >> 10u)), 0).r; }\n"
    "uint swz(uint x, uint y) { return texelFetch(u_sw, ivec2(int(x), int(y)), 0).r; }\n"
    "uint bw8(uint bw) { return bw > 1u ? bw >> 1u : 1u; }\n"
    /* the address of a pixel in its layout's units (gs_addr32/16/8/4) */
    "uint pix_addr(int lay, uint bp, uint bw, uint x, uint y) {\n"
    "    x &= 2047u; y &= 2047u;\n"
    "    if (lay <= 1) return (((bp + ((y >> 5u) * bw + (x >> 6u)) * 32u) << 6u) + swz(x & 63u, (y & 31u) + uint(lay) * 32u)) & 0xFFFFFu;\n"
    "    if (lay <= 5) return (((bp + ((y >> 6u) * bw + (x >> 6u)) * 32u) << 7u) + swz(x & 63u, 64u + uint(lay - 2) * 64u + (y & 63u))) & 0x1FFFFFu;\n"
    "    if (lay == 6) return (((bp + ((y >> 6u) * bw8(bw) + (x >> 7u)) * 32u) << 8u) + swz(x & 127u, 320u + (y & 63u))) & 0x3FFFFFu;\n"
    "    return (((bp + ((y >> 7u) * bw8(bw) + (x >> 7u)) * 32u) << 9u) + swz(x & 127u, 384u + (y & 127u))) & 0x7FFFFFu;\n"
    "}\n"
    /* a pixel's raw value (gs_read_pixel; CT24 keeps the top byte) */
    "uint read_raw(int lay, int kind, uint bp, uint bw, uint x, uint y) {\n"
    "    uint a = pix_addr(lay, bp, bw, x, y);\n"
    "    if (lay <= 1) {\n"
    "        uint w = vword(a);\n"
    "        if (kind == 5) return w >> 24u;\n"
    "        if (kind == 6) return (w >> 24u) & 15u;\n"
    "        if (kind == 7) return w >> 28u;\n"
    "        return w;\n"
    "    }\n"
    "    if (lay <= 5) return (vword(a >> 1u) >> ((a & 1u) * 16u)) & 0xFFFFu;\n"
    "    if (lay == 6) return (vword(a >> 2u) >> ((a & 3u) * 8u)) & 0xFFu;\n"
    "    return (vword(a >> 3u) >> ((a & 7u) * 4u)) & 15u;\n"
    "}\n"
    "vec4 unpack32(uint w) {\n"
    "    return vec4(float(w & 255u), float((w >> 8u) & 255u), float((w >> 16u) & 255u), float(w >> 24u)) / 255.0;\n"
    "}\n"
    /* a 16-bit pixel in a render target (alpha 0x80 or 0) */
    "vec4 unpack16(uint c) {\n"
    "    return vec4(float((c & 31u) << 3u), float(((c >> 5u) & 31u) << 3u), float(((c >> 10u) & 31u) << 3u),\n"
    "                (c & 0x8000u) != 0u ? 128.0 : 0.0) / 255.0;\n"
    "}\n";

/* A quad per instance: gl_VertexID 0-5 over (0,0)-(1,1) */
const char gsgl_quad_vs[] =
    "#version 330 core\n"
    "uniform ivec4 u_items[128];\n" /* four items an entry */
    "uniform vec4 u_geo;\n"        /* item size in output pixels, 2 / output size */
    "uniform ivec4 u_lay;\n"       /* how items map to positions: x: 0 mirror page rows, 1 cells of a buffer; y: bw; z: cell w; w: cell h (native) */
    "uniform ivec2 u_scale;\n"
    "flat out int v_item;\n"
    "void main() {\n"
    "    int it = u_items[gl_InstanceID >> 2][gl_InstanceID & 3];\n"
    "    int k = gl_VertexID;\n"
    "    vec2 c = vec2((k == 1 || k == 2 || k == 4) ? 1.0 : 0.0, (k == 2 || k == 4 || k == 5) ? 1.0 : 0.0);\n"
    "    vec2 o;\n"
    "    if (u_lay.x == 0) { o = vec2(0.0, float(it * 2)); c *= vec2(1024.0, 2.0); }\n"
    "    else { o = vec2(float((it % u_lay.y) * u_lay.z), float((it / u_lay.y) * u_lay.w)) * vec2(u_scale);\n"
    "           c *= vec2(float(u_lay.z), float(u_lay.w)) * vec2(u_scale); }\n"
    "    gl_Position = vec4((o + c) * u_geo.zw - 1.0, 0.0, 1.0);\n"
    "    v_item = it;\n"
    "}\n";

/* Texture decode: memory (as a texture's PSM) to RGBA8, CLUT and TEXA applied */
const char gsgl_decode_fs[] =
    "uniform ivec4 u_fmt;\n" /* layout, kind, bp, bw */
    "uniform ivec4 u_texa;\n" /* ta0, ta1, aem */
    "uniform sampler2D u_clut;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "    uvec2 p = uvec2(gl_FragCoord.xy);\n"
    "    int k = u_fmt.y;\n"
    "    uint v = read_raw(u_fmt.x, k, uint(u_fmt.z), uint(u_fmt.w), p.x, p.y);\n"
    "    if (k == 0) { o = unpack32(v); }\n"
    "    else if (k == 1) { uint c = v & 0xFFFFFFu; o = unpack32(c | (((u_texa.z != 0 && c == 0u) ? 0u : uint(u_texa.x)) << 24u)); }\n"
    "    else if (k == 2) {\n"
    "        uint a = (v & 0x8000u) != 0u ? uint(u_texa.y) : ((u_texa.z == 0 || (v & 0x7FFFu) != 0u) ? uint(u_texa.x) : 0u);\n"
    "        o = vec4(float((v & 31u) << 3u), float(((v >> 5u) & 31u) << 3u), float(((v >> 10u) & 31u) << 3u), float(a)) / 255.0;\n"
    "    }\n"
    "    else { o = texelFetch(u_clut, ivec2(int(v), 0), 0); }\n"
    "}\n";

/* Memory to a colour target, a cell (page) at a time */
const char gsgl_load_fs[] =
    "uniform ivec4 u_fmt;\n" /* layout, 0: 32-bit / 1: 16-bit / 2: Z24, bp, bw */
    "uniform ivec2 u_scale;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "    uvec2 p = uvec2(ivec2(gl_FragCoord.xy) / u_scale);\n"
    "    uint v = read_raw(u_fmt.x, u_fmt.y == 1 ? 2 : 0, uint(u_fmt.z), uint(u_fmt.w), p.x, p.y);\n"
    "    o = u_fmt.y == 1 ? unpack16(v) : unpack32(v);\n"
    "}\n";

/* Memory to a depth target */
const char gsgl_loadz_fs[] =
    "uniform ivec4 u_fmt;\n"
    "uniform ivec2 u_scale;\n"
    "void main() {\n"
    "    uvec2 p = uvec2(ivec2(gl_FragCoord.xy) / u_scale);\n"
    "    uint v = read_raw(u_fmt.x, u_fmt.y == 1 ? 2 : 0, uint(u_fmt.z), uint(u_fmt.w), p.x, p.y);\n"
    "    if (u_fmt.y == 2) v &= 0xFFFFFFu;\n"
    "    gl_FragDepth = float(v) * (1.0 / 4294967296.0);\n"
    "}\n";

/* A target's pages back to memory (the mirror): one word per fragment */
const char gsgl_resolve_fs[] =
    "#version 330 core\n"
    "uniform sampler2D u_src;\n"
    "uniform usampler2D u_inv;\n"
    "uniform ivec4 u_fmt;\n" /* layout, depth, bp, bw */
    "uniform ivec2 u_scale;\n"
    "flat in int v_item;\n"
    "out uint o;\n"
    "uint pk32(vec4 t) { uvec4 c = uvec4(t * 255.0 + 0.5); return c.r | (c.g << 8u) | (c.b << 16u) | (c.a << 24u); }\n"
    "uint pk16(vec4 t) { uvec4 c = uvec4(t * 255.0 + 0.5); return (c.r >> 3u) | ((c.g >> 3u) << 5u) | ((c.b >> 3u) << 10u) | ((c.a & 0x80u) << 8u); }\n"
    "uint zval(float d) { return d >= 1.0 ? 0xFFFFFFFFu : uint(d * 4294967296.0); }\n"
    "ivec2 at(int cx, int cy, uint xy, int ch) { return ivec2(cx * 64 + int(xy & 255u), cy * ch + int(xy >> 8u)) * u_scale; }\n"
    "void main() {\n"
    "    int w = (int(gl_FragCoord.y) - v_item * 2) * 1024 + int(gl_FragCoord.x);\n"
    "    int rel = (v_item - (u_fmt.z >> 5)) & 511;\n"
    "    int cx = rel % u_fmt.w, cy = rel / u_fmt.w;\n"
    "    if (u_fmt.x <= 1) {\n"
    "        ivec2 s = at(cx, cy, texelFetch(u_inv, ivec2(w, u_fmt.x), 0).r, 32);\n"
    "        o = u_fmt.y != 0 ? zval(texelFetch(u_src, s, 0).r) : pk32(texelFetch(u_src, s, 0));\n"
    "    } else {\n"
    "        int r = u_fmt.x;\n"
    "        ivec2 s0 = at(cx, cy, texelFetch(u_inv, ivec2(w * 2, r), 0).r, 64);\n"
    "        ivec2 s1 = at(cx, cy, texelFetch(u_inv, ivec2(w * 2 + 1, r), 0).r, 64);\n"
    "        uint a = u_fmt.y != 0 ? (zval(texelFetch(u_src, s0, 0).r) & 0xFFFFu) : pk16(texelFetch(u_src, s0, 0));\n"
    "        uint b = u_fmt.y != 0 ? (zval(texelFetch(u_src, s1, 0).r) & 0xFFFFu) : pk16(texelFetch(u_src, s1, 0));\n"
    "        o = a | (b << 16u);\n"
    "    }\n"
    "}\n";

/* Drawing: a GS primitive's vertices, in GS window coordinates */
const char gsgl_draw_vs[] =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in uint a_z;\n"
    "layout(location = 2) in vec4 a_col;\n"
    "layout(location = 3) in vec3 a_stq;\n"
    "layout(location = 4) in float a_fog;\n"
    "layout(location = 5) in vec4 a_cl;\n"
    "uniform vec4 u_vp;\n" /* (1), 2 / width, 2 / height (in target texels), 1: [0,1] depth (clip control) */
    "uniform vec2 u_vps;\n" /* the scale across and down */
    "uniform uint u_zmax;\n"
    "noperspective out vec4 v_col;\n"
    "flat out vec4 v_fcol;\n"
    "noperspective out vec3 v_stq;\n"
    "noperspective out float v_fog;\n"
    "flat out vec4 v_cl;\n"
    "void main() {\n"
    /* GS pixel centres are at integer coordinates, GL's at + 0.5; and a
       hair to the upper left, so samples exactly on an edge fall on the
       GS's side of it (left and top edges in, right and bottom out) */
    "    vec2 w = a_pos * u_vps + vec2(0.5 - 1.0 / 128.0);\n"
    "    float z = float(min(a_z, u_zmax)) * (1.0 / 4294967296.0);\n"
    "    gl_Position = vec4(w * u_vp.yz - 1.0, u_vp.w != 0.0 ? z : z * 2.0 - 1.0, 1.0);\n"
    "    v_col = a_col;\n"
    "    v_fcol = a_col;\n"
    "    v_stq = a_stq;\n"
    "    v_fog = a_fog;\n"
    "    v_cl = a_cl;\n"
    "}\n";

const char gsgl_draw_fs[] =
    "#version 330 core\n"
    "noperspective in vec4 v_col;\n"
    "flat in vec4 v_fcol;\n"
    "noperspective in vec3 v_stq;\n"
    "noperspective in float v_fog;\n"
    "flat in vec4 v_cl;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_dst;\n"
    "uniform ivec4 u_tf;\n"   /* tme, tfx, tcc, fst */
    "uniform ivec4 u_tw;\n"   /* wms, wmt, bilinear, iip */
    "uniform ivec4 u_reg;\n"  /* minu, maxu, minv, maxv */
    "uniform vec4 u_tsz;\n"   /* texture width, height (texels), 2: a target (texels to GL texels by u_scale) else 1, RT (1) */
    "uniform vec4 u_toff;\n"  /* texel offset in the target (x, y), 1 / GL texture width, height */
    "uniform ivec4 u_at;\n"   /* atst, aref, 0: discard failing / 1: discard passing / 2: no test, fba */
    "uniform vec4 u_fog;\n"   /* fge, FOGCOL r g b */
    "uniform vec4 u_bk;\n"    /* blend: Cs's coefficient (times C, plus), Cd's (times C, plus) */
    "uniform ivec4 u_bm;\n"   /* blend on, C: 0 As 2 FIX, equation (0 add, 1 subtract, 2 reverse), pabe */
    "uniform float u_fix;\n"
    "uniform ivec4 u_misc;\n" /* date, datm, 16-bit frame, dither */
    "uniform ivec4 u_dimx[4];\n"
    "uniform ivec2 u_scale;\n"
    "layout(location = 0, index = 0) out vec4 o_col;\n"
    "layout(location = 0, index = 1) out vec4 o_fac;\n"
    "int wrapc(int c, int mode, int mask, int mn, int mx) {\n"
    "    if (mode == 0) return c & mask;\n"
    "    if (mode == 1) return clamp(c, 0, mask);\n"
    "    if (mode == 2) return clamp(c, mn, mx);\n"
    "    return (c & mn) | mx;\n"
    "}\n"
    "ivec2 wrap2(ivec2 t) {\n"
    "    ivec2 sz = ivec2(u_tsz.xy);\n"
    "    return ivec2(wrapc(t.x, u_tw.x, sz.x - 1, u_reg.x, u_reg.y), wrapc(t.y, u_tw.y, sz.y - 1, u_reg.z, u_reg.w));\n"
    "}\n"
    "vec4 fetch(ivec2 t) { return texelFetch(u_tex, ivec2((vec2(t) + u_toff.xy) * (u_tsz.z > 1.5 ? vec2(u_scale) : vec2(1.0))), 0); }\n"
    /* A replacement (u_tsz.z < 0): any size, a multiple of the texture's.
       The coordinates are in the original's texels; the wrapping is done on
       them, the sampler then reads in normalised ones. Its alpha is the GS's
       (128 opaque), as in the PCSX2 packs of its time: no correction. */
    "vec4 sample_rep(vec2 uv) {\n"
    "    vec2 c = uv;\n"
    "    if (u_tw.x == 0) c.x = mod(c.x, u_tsz.x);\n"
    "    else if (u_tw.x == 1) c.x = clamp(c.x, 0.0, u_tsz.x);\n"
    "    else if (u_tw.x == 2) c.x = clamp(c.x, float(u_reg.x), float(u_reg.y) + 1.0);\n"
    "    else c.x = float((int(floor(c.x)) & u_reg.x) | u_reg.y) + fract(c.x);\n"
    "    if (u_tw.y == 0) c.y = mod(c.y, u_tsz.y);\n"
    "    else if (u_tw.y == 1) c.y = clamp(c.y, 0.0, u_tsz.y);\n"
    "    else if (u_tw.y == 2) c.y = clamp(c.y, float(u_reg.z), float(u_reg.w) + 1.0);\n"
    "    else c.y = float((int(floor(c.y)) & u_reg.z) | u_reg.w) + fract(c.y);\n"
    "    return texture(u_tex, c * u_toff.zw);\n"
    "}\n"
    "vec4 sample_tex(vec2 uv) {\n"
    "    if (u_tsz.z < 0.0) return sample_rep(uv);\n"
    "    if (u_tw.z == 0) return fetch(wrap2(ivec2(floor(uv))));\n"
    "    if (u_tw.x == 3 || u_tw.y == 3 || (u_tsz.w != 0.0 && (u_tw.x == 0 || u_tw.y == 0))) {\n"
    /* region repeat (and repeat on a target): four texels, wrapped one by one */
    "        vec2 b = uv - 0.5;\n"
    "        ivec2 i = ivec2(floor(b));\n"
    "        vec2 f = b - vec2(i);\n"
    "        if (u_tsz.w != 0.0) {\n"
    /* a target sampled at its own resolution: the GS's texel steps are
       finer than that, the filter works on the target's texels */
    "            vec2 c = uv;\n"
    "            if (u_tw.x == 0) c.x = mod(c.x, u_tsz.x);\n"
    "            if (u_tw.y == 0) c.y = mod(c.y, u_tsz.y);\n"
    "            return texture(u_tex, (c + u_toff.xy) * (u_tsz.z > 1.5 ? vec2(u_scale) : vec2(1.0)) * u_toff.zw);\n"
    "        }\n"
    "        vec4 a = mix(fetch(wrap2(i)), fetch(wrap2(i + ivec2(1, 0))), f.x);\n"
    "        vec4 c = mix(fetch(wrap2(i + ivec2(0, 1))), fetch(wrap2(i + ivec2(1, 1))), f.x);\n"
    "        return mix(a, c, f.y);\n"
    "    }\n"
    "    vec2 c = uv;\n"
    "    if (u_tw.x == 2) c.x = clamp(c.x, float(u_reg.x) + 0.5, float(u_reg.y) + 0.5);\n"
    "    else if (u_tw.x == 1) c.x = clamp(c.x, 0.5, u_tsz.x - 0.5);\n"
    "    if (u_tw.y == 2) c.y = clamp(c.y, float(u_reg.z) + 0.5, float(u_reg.w) + 0.5);\n"
    "    else if (u_tw.y == 1) c.y = clamp(c.y, 0.5, u_tsz.y - 0.5);\n"
    "    return texture(u_tex, (c + u_toff.xy) * (u_tsz.z > 1.5 ? vec2(u_scale) : vec2(1.0)) * u_toff.zw);\n"
    "}\n"
    "void main() {\n"
    "    vec4 c = clamp(floor((u_tw.w != 0 ? v_col : v_fcol) + 0.001), 0.0, 255.0);\n"
    "    if (u_tf.x != 0) {\n"
    "        vec2 uv = u_tf.w != 0 ? v_stq.xy : v_stq.xy / v_stq.z * u_tsz.xy;\n"
    /* v_cl is the uv at the first and last pixel centres; a replacement
       (finer than a texel) is sampled up to the sprite's edges, else each
       strip of a strip-drawn sprite shows a flat band at its edges */
    "        vec2 h = u_tsz.z < 0.0 ? 0.5 * fwidth(uv) : vec2(0.0);\n"
    "        uv = clamp(clamp(uv, v_cl.xy - h, v_cl.zw + h), vec2(-1.0e6), vec2(1.0e6));\n"
    /* (the GS's bilinear filter truncates; whole texels are exact either way) */
    "        vec4 t = floor(sample_tex(uv) * 255.0 + (u_tw.z != 0 ? 0.004 : 0.5));\n"
    "        vec3 m = floor(t.rgb * c.rgb / 128.0);\n"
    "        if (u_tf.y == 0) { c.rgb = min(m, 255.0); if (u_tf.z != 0) c.a = min(floor(t.a * c.a / 128.0), 255.0); }\n"
    "        else if (u_tf.y == 1) { c.rgb = t.rgb; if (u_tf.z != 0) c.a = t.a; }\n"
    "        else if (u_tf.y == 2) { c.rgb = min(m + c.a, 255.0); if (u_tf.z != 0) c.a = min(t.a + c.a, 255.0); }\n"
    "        else { c.rgb = min(m + c.a, 255.0); if (u_tf.z != 0) c.a = t.a; }\n"
    "    }\n"
    "    if (u_fog.x != 0.0) {\n"
    "        float f = clamp(floor(v_fog + 0.001), 0.0, 255.0);\n"
    "        c.rgb = floor((f * c.rgb + (255.0 - f) * u_fog.yzw) / 256.0);\n"
    "    }\n"
    "    if (u_at.z != 2) {\n"
    "        int a = int(c.a), r = u_at.y;\n"
    "        bool pass;\n"
    "        if (u_at.x == 0) pass = false;\n"
    "        else if (u_at.x == 1) pass = true;\n"
    "        else if (u_at.x == 2) pass = a < r;\n"
    "        else if (u_at.x == 3) pass = a <= r;\n"
    "        else if (u_at.x == 4) pass = a == r;\n"
    "        else if (u_at.x == 5) pass = a >= r;\n"
    "        else if (u_at.x == 6) pass = a > r;\n"
    "        else pass = a != r;\n"
    "        if (pass == (u_at.z == 1)) discard;\n"
    "    }\n"
    "    if (u_misc.x != 0) {\n"
    "        float da = texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0).a;\n"
    "        if ((da > 0.5) != (u_misc.y != 0)) discard;\n"
    "    }\n"
    "    if (u_misc.w != 0) {\n"
    "        ivec2 q = ivec2(gl_FragCoord.xy) / u_scale;\n"
    "        c.rgb = clamp(c.rgb + float(u_dimx[q.y & 3][q.x & 3]), 0.0, 255.0);\n"
    "    }\n"
    "    if (u_misc.z != 0) c.rgb = floor(c.rgb / 8.0) * 8.0;\n"
    "    float a = c.a;\n"
    "    float oa = (u_at.w != 0 && a < 128.0) ? a + 128.0 : a;\n"
    "    if (u_misc.z != 0) oa = oa >= 128.0 ? 128.0 : 0.0;\n"
    "    if (u_bm.x != 0) {\n"
    "        float cc = u_bm.y == 0 ? a / 128.0 : u_fix;\n"
    "        float ks = cc * u_bk.x + u_bk.y, kd = cc * u_bk.z + u_bk.w;\n"
    "        if (u_bm.w != 0 && a < 128.0) { ks = 1.0; kd = 0.0; }\n"
    /* The GS truncates (A - B) * C >> 7; the blender rounds: a bias
       just under half a step turns its rounding into the truncation */
    "        if (u_bm.z == 2) { o_col.rgb = (c.rgb * -ks + 0.496) / 255.0; o_fac = vec4(kd); }\n"
    "        else if (u_bm.z == 1) { o_col.rgb = (c.rgb * ks - 0.496) / 255.0; o_fac = vec4(-kd); }\n"
    "        else { o_col.rgb = (c.rgb * ks - 0.496) / 255.0; o_fac = vec4(kd); }\n"
    "    } else {\n"
    "        o_col.rgb = c.rgb / 255.0;\n"
    "        o_fac = vec4(0.0);\n"
    "    }\n"
    "    o_col.a = oa / 255.0;\n"
    "}\n";

/* The display: the two read circuits merged over the background colour
   (gs_display.c), into a picture at the internal resolution */
const char gsgl_compose_vs[] =
    "#version 330 core\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
const char gsgl_compose_fs[] =
    "#version 330 core\n"
    "uniform sampler2D u_c1;\n"
    "uniform sampler2D u_c2;\n"
    "uniform ivec4 u_r1;\n"   /* circuit 1's rectangle in output pixels (x0, y0, x1, y1) */
    "uniform ivec4 u_r2;\n"
    "uniform ivec4 u_o1;\n"   /* its first texel in the target (x, y), 24-bit, on */
    "uniform ivec4 u_o2;\n"
    "uniform ivec4 u_pm;\n"   /* mmod, alp, background rgb packed */
    "out vec4 o;\n"
    "bool inside(ivec2 p, ivec4 r) { return p.x >= r.x && p.y >= r.y && p.x < r.z && p.y < r.w; }\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    vec3 bg = vec3(float(u_pm.z & 255), float((u_pm.z >> 8) & 255), float((u_pm.z >> 16) & 255));\n"
    "    vec3 back = bg;\n"
    "    if (u_o2.w != 0 && inside(p, u_r2)) back = floor(texelFetch(u_c2, p - u_r2.xy + u_o2.xy, 0).rgb * 255.0 + 0.5);\n"
    "    vec3 outc = back;\n"
    "    if (u_o1.w != 0 && inside(p, u_r1)) {\n"
    "        vec4 f = floor(texelFetch(u_c1, p - u_r1.xy + u_o1.xy, 0) * 255.0 + 0.5);\n"
    "        if (u_o1.z != 0) f.a = 255.0;\n"
    "        float a = u_pm.x != 0 ? float(min(u_pm.y, 255)) : (f.a >= 128.0 ? 255.0 : f.a * 2.0);\n"
    "        outc = floor((f.rgb * a + back * (255.0 - a) + 127.0) / 255.0);\n"
    "    }\n"
    "    o = vec4(outc / 255.0, 1.0);\n"
    "}\n";

/* A rectangle of one texture copied into another (target growth, self-copies) */
const char gsgl_copy_fs[] =
    "#version 330 core\n"
    "uniform sampler2D u_src;\n"
    "uniform ivec4 u_off;\n" /* source texel = destination pixel + (x, y); z: depth */
    "out vec4 o;\n"
    "void main() {\n"
    "    vec4 t = texelFetch(u_src, ivec2(gl_FragCoord.xy) + u_off.xy, 0);\n"
    "    if (u_off.z != 0) gl_FragDepth = t.r; else o = t;\n"
    "}\n";
