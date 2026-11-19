// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Host test of AtlasScaler (atlas_scale.h): sizes, flat planes stay flat and
 * U, V and Y never mix, a 2:1 scale is the exact 2x2 average, non-integer
 * ratios keep a gradient's mean, rounding to level bits, refusals, a crop
 * (exact 1:1, scaled as the part cut out first), the edges' fade and the
 * speed patch, and the time a 1088x480 frame takes to scale to the HUD's
 * sizes. */
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
        /* the edges' fade: a white 800x476 picture (1088x480 1:1 but for the
         * crop), its edges black, its middle untouched, the ramp rising
         * smoothly from each edge over the band; U and V over half the band */
        std::vector<uint8_t> white(1088 * 720, 235);
        for (size_t i = 1088 * 480; i < white.size(); i++) white[i] = 200;
        AtlasScaler fs;
        fs.configure(1088, 480, 800, 476, 0, 142, 0, 804, 480);
        fs.set_edge_fade(30);
        fs.scale(white.data(), &out);
        const uint8_t *y = out.data(), *u = y + 800 * 476;
        ok("fade: each edge's pixel black (Y 16, U and V 128)",
           y[238 * 800] == 16 && y[238 * 800 + 799] == 16 && y[400] == 16 && y[475 * 800 + 400] == 16 && u[119 * 800] == 128 &&
               u[119 * 800 + 399] == 128 && u[200] == 128 && u[237 * 800 + 200] == 128);
        ok("fade: the middle and the band's inner edge untouched",
           y[238 * 800 + 400] == 235 && y[238 * 800 + 30] == 235 && y[30 * 800 + 400] == 235 && u[119 * 800 + 15] == 200);
        bool mono = true, ramp = false;
        for (uint32_t x = 1; x < 31; x++) {
            mono = mono && y[238 * 800 + x] >= y[238 * 800 + x - 1] && y[238 * 800 + 799 - x] >= y[238 * 800 + 800 - x];
            ramp = ramp || (y[238 * 800 + x] > 16 && y[238 * 800 + x] < 235);
        }
        ok("fade: the ramp rises from each edge, never back", mono && ramp);
        ok("fade: smooth at both ends (smoothstep): slow off the edge, slow into the middle",
           y[238 * 800 + 3] - 16 < 10 && 235 - y[238 * 800 + 27] < 10 && y[238 * 800 + 15] > 110 && y[238 * 800 + 15] < 140);
        ok("fade: a corner darker than either edge's middle at that depth", y[10 * 800 + 10] < y[10 * 800 + 400]);
        fs.set_edge_fade(0);
        fs.scale(white.data(), &out);
        ok("fade 0: off", out[238 * 800] == 235);
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
        /* with the fade: multiplied */
        ps.set_clear(0, 300, 200, 100);
        ps.set_edge_fade(30);
        ps.scale(white.data(), &out);
        ok("patch and fade together: black where either is, the rest as the fade alone", out[350 * 800 + 100] == 16 && out[350 * 800] == 16 &&
                                                                                           out[100 * 800 + 400] == 235);
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
        /* 135 %: 804x480 of the picture to 800x476, the edges' fade and the speed patch */
        AtlasScaler t;
        t.configure(1088, 480, 800, 476, 2, 142, 0, 804, 480);
        t.set_edge_fade(30);
        t.set_clear(110, 320, 220, 90);
        t.scale(ramp.data(), &out);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; i++) t.scale(ramp.data(), &out);
        const double each = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
        std::printf("     1088x480 (804x480 of it) -> 800x476, fade and patch: %.2f ms a frame on this machine\n", each);
    }

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
