// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Host test of AtlasScaler (atlas_scale.h): sizes, flat planes stay flat and
 * U, V and Y never mix, a 2:1 scale is the exact 2x2 average, non-integer
 * ratios keep a gradient's mean, rounding to level bits, refusals, a crop
 * (exact 1:1, scaled as the part cut out first), rounded corners, the
 * speed patch, the vignette (fixed to the scene: its shape, the same scene
 * pixel alike whatever the map's size and place, level 0 nothing, with the
 * corners and the patch), and the time a 1088x480 frame takes to scale to
 * the HUD's sizes. */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "atlas_scale.h"
#include "yuv_atlas.h"

using namespace woc;

static int failures;
static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static std::vector<uint8_t> atlas(uint32_t w, uint32_t h, uint8_t y, uint8_t u, uint8_t v)
{
    std::vector<uint8_t> a((size_t)w * crk::atlas_height(h));
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t c = 0; c < w; c++) a[(size_t)r * w + c] = y;
    for (uint32_t r = 0; r < h / 2; r++)
        for (uint32_t c = 0; c < w / 2; c++) {
            a[(size_t)(h + r) * w + c] = u;
            a[(size_t)(h + r) * w + w / 2 + c] = v;
        }
    return a;
}

int main()
{
    AtlasScaler s;
    std::vector<uint8_t> out;

    ok("refuses odd, upscaled or empty sizes",
       !s.configure(1088, 480, 453, 200, 0) && !s.configure(1088, 480, 1200, 200, 0) &&
           !s.configure(1088, 480, 452, 482, 0) && !s.configure(0, 480, 452, 200, 0));
    ok("1088x480 -> 452x200", s.configure(1088, 480, 452, 200, 0) && s.dst_bytes() == 452u * 300);

    const std::vector<uint8_t> flat = atlas(1088, 480, 200, 60, 180);
    s.scale(flat.data(), &out);
    bool flat_ok = out.size() == 452u * 300;
    for (uint32_t r = 0; r < 200 && flat_ok; r++)
        for (uint32_t c = 0; c < 452; c++) flat_ok = flat_ok && out[(size_t)r * 452 + c] == 200;
    for (uint32_t r = 0; r < 100 && flat_ok; r++)
        for (uint32_t c = 0; c < 226; c++)
            flat_ok = flat_ok && out[(size_t)(200 + r) * 452 + c] == 60 && out[(size_t)(200 + r) * 452 + 226 + c] == 180;
    ok("flat Y, U and V stay exact: the planes never mix", flat_ok);

    /* 2:1 is the 2x2 average */
    std::vector<uint8_t> g((size_t)8 * crk::atlas_height(8), 128);
    for (uint32_t r = 0; r < 8; r++)
        for (uint32_t c = 0; c < 8; c++) g[r * 8 + c] = (uint8_t)(r * 30 + c * 3);
    AtlasScaler h;
    ok("8x8 -> 4x4", h.configure(8, 8, 4, 4, 0));
    h.scale(g.data(), &out);
    bool avg = true;
    for (uint32_t r = 0; r < 4; r++)
        for (uint32_t c = 0; c < 4; c++) {
            const int want = (int)std::lround((g[(2 * r) * 8 + 2 * c] + g[(2 * r) * 8 + 2 * c + 1] + g[(2 * r + 1) * 8 + 2 * c] +
                                               g[(2 * r + 1) * 8 + 2 * c + 1]) / 4.0);
            avg = avg && std::abs((int)out[r * 4 + c] - want) <= 1;
        }
    ok("2:1 is the 2x2 average (within rounding)", avg);

    /* a horizontal ramp at 1088 -> 452 keeps its mean and stays monotonic */
    std::vector<uint8_t> ramp = atlas(1088, 480, 0, 128, 128);
    for (uint32_t r = 0; r < 480; r++)
        for (uint32_t c = 0; c < 1088; c++) ramp[(size_t)r * 1088 + c] = (uint8_t)(c * 255 / 1087);
    s.scale(ramp.data(), &out);
    double ms = 0, md = 0;
    bool mono = true;
    for (uint32_t c = 0; c < 1088; c++) ms += ramp[c];
    for (uint32_t c = 0; c < 452; c++) {
        md += out[c];
        if (c) mono = mono && out[c] >= out[c - 1];
    }
    ok("a ramp keeps its mean and order at 2.41:1", std::fabs(ms / 1088 - md / 452) < 0.6 && mono);

    AtlasScaler rb;
    rb.configure(1088, 480, 452, 200, 2);
    rb.scale(ramp.data(), &out);
    bool rounded = true;
    for (uint8_t b : out) rounded = rounded && (b % 4 == 0 || b == 255);
    ok("level bits: values rounded to multiples of 4 (255 kept)", rounded);

    ok("same size is allowed (a plain copy)", h.configure(8, 8, 8, 8, 0));
    h.scale(g.data(), &out);
    ok("and copies exactly", out == g);

    {
        /* rounded corners: a white frame, its corners black (Y 16, U and V
         * 128), the edges' middles and the inside untouched */
        AtlasScaler rc;
        rc.configure(1088, 480, 452, 200, 0);
        rc.set_corner_radius(16);
        std::vector<uint8_t> white(1088 * 720, 235);
        for (size_t i = 1088 * 480; i < white.size(); i++) white[i] = 200;
        rc.scale(white.data(), &out);
        const uint8_t *y = out.data(), *u = y + 452 * 200;
        ok("corners: the four corner pixels black", y[0] == 16 && y[451] == 16 && y[199 * 452] == 16 && y[199 * 452 + 451] == 16);
        ok("corners: the chroma corner neutral", u[0] == 128 && u[99 * 452 + 225] == 128);
        ok("corners: the edges' middles and the inside untouched", y[226] == 235 && y[100 * 452] == 235 && y[100 * 452 + 226] == 235 &&
                                                                       y[16 * 452 + 16] == 235);
        bool aa = false;
        for (uint32_t x = 0; x < 16; x++) aa = aa || (y[x] > 16 && y[x] < 235);
        ok("corners: anti-aliased along the curve", aa);
        rc.set_corner_radius(0);
        rc.scale(white.data(), &out);
        ok("radius 0: square again", out[0] == 235);
    }

    {
        /* black background: a night map (navy field Y 60, a road Y 120, a
         * label Y 220): the field goes black with neutral chroma, the road
         * and label stay; a day map (field Y 200) is left alone */
        std::vector<uint8_t> night(1088 * 720, 60);
        for (size_t i = 1088 * 480; i < night.size(); i++) night[i] = (i % 2) ? 150 : 110;   /* bluish U, V */
        for (uint32_t r = 200; r < 240; r++)
            for (uint32_t c = 0; c < 1088; c++) night[r * 1088 + c] = 120;
        for (uint32_t r = 300; r < 320; r++)
            for (uint32_t c = 500; c < 600; c++) night[r * 1088 + c] = 220;
        AtlasScaler kb;
        kb.configure(1088, 480, 452, 200, 0);
        kb.set_black_background(true);
        kb.scale(night.data(), &out);
        const uint8_t *y = out.data(), *u = y + 452 * 200;
        ok("black background: the navy field black", y[20 * 452 + 20] == 16 && kb.background() / 16 == 60);
        ok("black background: its chroma neutral", u[5 * 452 + 5] == 128);
        ok("black background: the road kept", y[91 * 452 + 100] == 120);
        ok("black background: the label kept", y[129 * 452 + 228] == 220);
        /* a park: a dark green block (Y about the field's, U a little below
         * neutral, V well below) on the night field - kept, chroma and all;
         * and a frame mostly park doesn't make the park the background */
        std::vector<uint8_t> park = night;
        for (uint32_t r = 40; r < 120; r++)
            for (uint32_t c = 100; c < 400; c++) park[r * 1088 + c] = 62;
        for (uint32_t r = 20; r < 60; r++)
            for (uint32_t c = 50; c < 200; c++) { park[1088 * 480 + r * 1088 + c] = 124; park[1088 * 480 + r * 1088 + 544 + c] = 116; }
        AtlasScaler kp;
        kp.configure(1088, 480, 452, 200, 0);
        kp.set_black_background(true);
        kp.scale(park.data(), &out);
        const uint8_t *py = out.data(), *pu = py + 452 * 200, *pv = pu + 226;
        ok("black background: a green park kept (luma and chroma)", py[33 * 452 + 100] == 62 && pu[16 * 452 + 50] < 128 && pv[16 * 452 + 50] < 122);
        ok("black background: the navy field still black beside the park", py[150 * 452 + 300] == 16);
        std::vector<uint8_t> parks(1088 * 720, 62);
        for (size_t r = 0; r < 240; r++)
            for (size_t c = 0; c < 544; c++) { parks[1088 * 480 + r * 1088 + c] = 124; parks[1088 * 480 + r * 1088 + 544 + c] = 116; }
        for (uint32_t r = 400; r < 480; r++)
            for (uint32_t c = 0; c < 1088; c++) parks[r * 1088 + c] = 40;
        for (size_t r = 200; r < 240; r++)
            for (size_t c = 0; c < 544; c++) { parks[1088 * 480 + r * 1088 + c] = 140; parks[1088 * 480 + r * 1088 + 544 + c] = 124; }
        AtlasScaler km;
        km.configure(1088, 480, 452, 200, 0);
        km.set_black_background(true);
        km.scale(parks.data(), &out);
        ok("black background: a frame mostly park keeps the park (not taken for the field)", out[50 * 452 + 200] == 62 && km.background() / 16 == 40);
        std::vector<uint8_t> day(1088 * 720, 200);
        for (size_t i = 1088 * 480; i < day.size(); i++) day[i] = 128;
        AtlasScaler kd;
        kd.configure(1088, 480, 452, 200, 0);
        kd.set_black_background(true);
        kd.scale(day.data(), &out);
        ok("black background: a day map left alone", out[20 * 452 + 20] == 200);
    }

    {
        /* a crop (a host's part of the map): 1:1 it is exactly that part,
         * Y and both chroma planes; scaled, it is the scale of that part
         * cut out first */
        std::vector<uint8_t> f(1088 * 720);
        for (size_t i = 0; i < f.size(); i++)
            f[i] = (uint8_t)((i * 7 + i / 1088 * 3) & 0xff);
        AtlasScaler cs;
        ok("crop: refused odd, outside the frame, or smaller than the output",
           !cs.configure(1088, 480, 560, 400, 0, 263, 40, 560, 400) &&
               !cs.configure(1088, 480, 560, 400, 0, 600, 40, 560, 400) &&
               !cs.configure(1088, 480, 564, 400, 0, 264, 40, 560, 400));
        ok("crop: 560x400 at 264,40, 1:1", cs.configure(1088, 480, 560, 400, 0, 264, 40, 560, 400) &&
                                               cs.dst_bytes() == 560u * 600);
        cs.scale(f.data(), &out);
        std::vector<uint8_t> cut(560 * 600);
        for (uint32_t r = 0; r < 400; r++)
            for (uint32_t c = 0; c < 560; c++) cut[r * 560 + c] = f[(r + 40) * 1088 + 264 + c];
        for (uint32_t r = 0; r < 200; r++)
            for (uint32_t c = 0; c < 280; c++) {
                cut[(400 + r) * 560 + c] = f[(480 + 20 + r) * 1088 + 132 + c];
                cut[(400 + r) * 560 + 280 + c] = f[(480 + 20 + r) * 1088 + 544 + 132 + c];
            }
        ok("crop: 1:1 is exactly that part of Y, U and V", out == cut);
        AtlasScaler half, whole;
        half.configure(1088, 480, 280, 200, 0, 264, 40, 560, 400);
        whole.configure(560, 400, 280, 200, 0);
        std::vector<uint8_t> a, b;
        half.scale(f.data(), &a);
        whole.scale(cut.data(), &b);
        ok("crop: scaled is the scale of the part cut out", a == b);
    }

    {
        /* the speed patch: a rectangle black inside, a 12 px feather in from
         * its edges, clipped to the picture; the rest untouched */
        std::vector<uint8_t> white(1088 * 720, 235);
        for (size_t i = 1088 * 480; i < white.size(); i++) white[i] = 200;
        AtlasScaler ps;
        ps.configure(1088, 480, 800, 476, 0, 142, 0, 804, 480);
        ps.set_clear(110, 320, 220, 90);
        ps.scale(white.data(), &out);
        const uint8_t *y = out.data(), *u = y + 800 * 476;
        ok("patch: its middle black, chroma neutral", y[365 * 800 + 220] == 16 && y[333 * 800 + 123] == 16 &&
                                                       u[182 * 800 + 110] == 128);
        ok("patch: outside it untouched", y[319 * 800 + 220] == 235 && y[365 * 800 + 109] == 235 && y[365 * 800 + 330] == 235 &&
                                           y[410 * 800 + 220] == 235 && y[100 * 800 + 400] == 235);
        bool feather = true;
        for (uint32_t x = 111; x < 122; x++) feather = feather && y[365 * 800 + x] <= y[365 * 800 + x - 1];
        ok("patch: a feather 12 px in from its edge, darker going in", feather && y[365 * 800 + 110] > 200 && y[365 * 800 + 116] > 16 &&
                                                                       y[365 * 800 + 116] < 235);
        ps.set_clear(700, 400, 220, 90);   /* past the right and bottom edges */
        ps.scale(white.data(), &out);
        ok("patch: clipped to the picture (its part inside black, its feather where its edges are)",
           out[460 * 800 + 790] == 16 && out[475 * 800 + 799] == 16 && out[460 * 800 + 700] > 200 && out[399 * 800 + 790] == 235);
        ps.set_clear(-100, -50, 120, 70);   /* past the left and top edges */
        ps.scale(white.data(), &out);
        ok("patch: clipped at the top left", out[0] == 16 && out[19 * 800 + 19] > 16 && out[30 * 800 + 5] == 235);
        ps.set_clear(0, 0, 0, 0);
        ps.scale(white.data(), &out);
        ok("patch: none (hud_clear 0)", out[365 * 800 + 220] == 235 && out[0] == 235);
        ps.set_clear(110, 320, 20, 40);   /* small: the feather a quarter of its smaller side */
        ps.scale(white.data(), &out);
        ok("patch: a small one's feather at most a quarter of its smaller side (5 px), black in its middle",
           out[340 * 800 + 120] == 16 && out[340 * 800 + 115] == 16 && out[340 * 800 + 111] > 16);
        /* with the rounded corners: the patch applied after them, both black */
        ps.set_clear(0, 300, 200, 100);
        ps.set_corner_radius(39);
        ps.scale(white.data(), &out);
        ok("patch and corners together: black where either is, the rest untouched", out[350 * 800 + 100] == 16 && out[0] == 16 &&
                                                                                      out[475 * 800 + 799] == 16 && out[100 * 800 + 400] == 235 &&
                                                                                      out[475 * 800 + 400] == 235);
    }

    {
        /* the vignette: fixed to the 800x480 scene, not to the map. A white
         * frame; at 135 % the map is 800x476 at 0,2 of the scene, so the
         * scene's top left is at 0,-2 of the map. */
        std::vector<uint8_t> white(1088 * 720, 235);
        for (size_t i = 1088 * 480; i < white.size(); i++) white[i] = 200;
        const auto wide = [&](unsigned level, int32_t sx, int32_t sy, std::vector<uint8_t> *o) {
            AtlasScaler v;
            v.configure(1088, 480, 800, 476, 0, 142, 0, 804, 480);
            v.set_vignette(level, sx, sy, 800, 480);
            v.scale(white.data(), o);
        };
        std::vector<uint8_t> plain, a, b;
        AtlasScaler none;
        none.configure(1088, 480, 800, 476, 0, 142, 0, 804, 480);
        none.scale(white.data(), &plain);
        wide(5, 0, -2, &a);
        const uint8_t *y = a.data(), *u = y + 800 * 476;
        ok("vignette: the scene's centre (400,240) kept whole, chroma too, at the strongest level",
           y[238 * 800 + 400] == 235 && y[237 * 800 + 399] == 235 && u[119 * 800 + 200] == 200);
        wide(1, 0, -2, &a);
        y = a.data();
        u = y + 800 * 476;
        ok("vignette: the scene's left and right edges black at the weakest level, chroma neutral",
           y[238 * 800] == 16 && y[238 * 800 + 799] == 16 && u[119 * 800] == 128 && u[119 * 800 + 399] == 128);
        ok("vignette: the weakest level keeps the scene's middle whole (x 40-759, y 28-452)",
           y[238 * 800 + 40] == 235 && y[238 * 800 + 759] == 235 && y[26 * 800 + 400] == 235 && y[450 * 800 + 400] == 235);
        wide(1, 0, 10, &b);   /* the map 10 px above the scene's top: its rows 0-9 are off the scene */
        ok("vignette: the map's part off the scene black", b[0 * 800 + 400] == 16 && b[9 * 800 + 400] == 16 &&
                                                              b[250 * 800 + 400] == 235);
        wide(0, 0, -2, &a);
        ok("vignette: level 0 changes nothing", a == plain);
        AtlasScaler off;
        off.configure(1088, 480, 800, 476, 0, 142, 0, 804, 480);
        off.set_vignette(3, 0, -2, 800, 480);
        off.scale(white.data(), &a);
        const bool was_on = a != plain;
        off.set_vignette(0, 0, -2, 800, 480);
        off.scale(white.data(), &a);
        ok("vignette: set back to 0, none again", was_on && a == plain);

        /* the shape: a scene pixel keeps 1 - smoothstep(r0, 1, d) of itself,
         * d = (u^4 + v^4)^(1/4) from the scene's centre, r0 = 1 - 0.07 level */
        const auto keep = [](unsigned level, int sx, int sy) {
            const double u = std::fabs(sx + 0.5 - 400) / 400, v = std::fabs(sy + 0.5 - 240) / 240;
            const double d = std::pow(u * u * u * u + v * v * v * v, 0.25), r0 = 1 - 0.07 * level;
            double t = (d - r0) / (1 - r0);
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            return 1 - t * t * (3 - 2 * t);
        };
        const auto want = [&](unsigned level, int sx, int sy) {
            return (int)(16 + 219 * std::lround(256 * keep(level, sx, sy)) / 256);
        };
        const int pts[][2] = {{100, 200}, {700, 60}, {400, 470}, {50, 50}, {760, 240}, {400, 10}, {200, 420}, {600, 100}};
        bool exact = true, ramp = false;
        for (unsigned level = 1; level <= 5; level++) {
            wide(level, 0, -2, &a);
            for (const auto &p : pts) {
                const int got = a[(size_t)(p[1] - 2) * 800 + p[0]];
                exact = exact && got == want(level, p[0], p[1]);
                ramp = ramp || (got > 16 && got < 235);
            }
        }
        ok("vignette: each level's shape the superellipse's smoothstep, from r0 = 1 - 0.07 level to the scene's edge", exact && ramp);

        /* the same scene pixel, the same factor, whatever the map's size and place */
        AtlasScaler m85, m85b;
        std::vector<uint8_t> c, e;
        m85.configure(1088, 480, 680, 300, 0);   /* 85 %, 40 down: 680x300 at 60,130 */
        m85.set_vignette(5, -60, -130, 800, 480);
        m85.scale(white.data(), &c);
        m85b.configure(1088, 480, 680, 300, 0);   /* 85 %, moved 50 left and 80 up: at 10,10 */
        m85b.set_vignette(5, -10, -10, 800, 480);
        m85b.scale(white.data(), &e);
        wide(5, 0, -2, &a);   /* 135 %: 800x476 at 0,2 */
        const int same_pts[][2] = {{65, 135}, {70, 300}, {100, 240}, {685, 200}, {650, 150}, {300, 305}};
        bool same = true;
        ramp = false;
        for (const auto &p : same_pts) {
            const int sx = p[0], sy = p[1];
            const uint8_t at85 = c[(size_t)(sy - 130) * 680 + sx - 60], at85b = e[(size_t)(sy - 10) * 680 + sx - 10],
                          at135 = a[(size_t)(sy - 2) * 800 + sx];
            same = same && at85 == at135 && at85b == at135 && at85 == want(5, sx, sy);
            ramp = ramp || (at135 > 16 && at135 < 235);
        }
        ok("vignette: a scene pixel the same at 85 % (two places) and 135 %", same && ramp);

        /* with the rounded corners and the speed patch: all three multiply */
        const auto combined = [&](bool corners, bool patch, bool vignette, std::vector<uint8_t> *o) {
            AtlasScaler v;
            v.configure(1088, 480, 680, 300, 0);   /* 680x300 at 60,130 */
            v.set_corner_radius(corners ? 25 : 0);
            if (patch)
                v.set_clear(110 - 60, 320 - 130, 220, 90);
            v.set_vignette(vignette ? 5 : 0, -60, -130, 800, 480);
            v.scale(white.data(), o);
        };
        std::vector<uint8_t> co, pa, vi, all;
        combined(true, false, false, &co);
        combined(false, true, false, &pa);
        combined(false, false, true, &vi);
        combined(true, true, true, &all);
        const auto product = [](int p, int q) { return 16 + (p - 16) * (q - 16) / 219; };
        const auto near = [](int got, int want) { return got >= want - 2 && got <= want + 2; };
        /* a corner's anti-aliased edge (its top row) and a patch's feather, each in the vignette's ramp */
        int cx = -1;
        for (int x = 0; x < 25 && cx < 0; x++)
            if (co[x] > 40 && co[x] < 200 && vi[x] > 40 && vi[x] < 200)
                cx = x;
        const size_t fp = (size_t)(365 - 130) * 680 + (115 - 60);   /* the scene's 115,365: 5 px into the patch's feather */
        ok("vignette with corners and patch: the corner pixel and the patch's middle black, the box's middle whole",
           all[0] == 16 && all[299 * 680 + 679] == 16 && all[(size_t)(365 - 130) * 680 + 220 - 60] == 16 &&
               all[150 * 680 + 340] == 235);
        ok("vignette with corners and patch: a corner's edge pixel is the corner's times the vignette's",
           cx >= 0 && near(all[cx], product(co[cx], vi[cx])) && all[cx] < co[cx] && all[cx] < vi[cx]);
        ok("vignette with corners and patch: the patch's feather is the patch's times the vignette's",
           pa[fp] > 16 && pa[fp] < 235 && vi[fp] > 16 && vi[fp] < 235 && near(all[fp], product(pa[fp], vi[fp])) &&
               all[fp] < pa[fp] && all[fp] < vi[fp]);
    }

    const struct { uint32_t w, h; } sizes[] = {{452, 200}, {560, 244}, {680, 300}, {800, 352}, {320, 140}};
    for (const auto &z : sizes) {
        AtlasScaler t;
        t.configure(1088, 480, z.w, z.h, 2);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; i++) t.scale(ramp.data(), &out);
        const double each = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
        std::printf("     1088x480 -> %ux%u: %.2f ms a frame on this machine, %zu B (%.1fx less than 783,360)\n", z.w, z.h,
                    each, t.dst_bytes(), 783360.0 / t.dst_bytes());
    }
    {
        /* 135 %: 804x480 of the picture to 800x476, rounded corners and the speed patch */
        AtlasScaler t;
        t.configure(1088, 480, 800, 476, 2, 142, 0, 804, 480);
        t.set_corner_radius(39);
        t.set_clear(110, 320, 220, 90);
        t.scale(ramp.data(), &out);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; i++) t.scale(ramp.data(), &out);
        const double each = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
        std::printf("     1088x480 (804x480 of it) -> 800x476, corners and patch: %.2f ms a frame on this machine\n", each);
        t.set_vignette(2, 0, -2, 800, 480);
        const auto t1 = std::chrono::steady_clock::now();
        t.scale(ramp.data(), &out);   /* the mask made again */
        const double first = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        const auto t2 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; i++) t.scale(ramp.data(), &out);
        const double with = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count() / 20;
        std::printf("     and the vignette (2): %.2f ms a frame, %.2f ms the frame after the change on this machine\n", with, first);
    }

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
