// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 lukher-dev
/* Host tests for the HUD map's zoom (hud_crop.h): the part of the frame a
 * zoom shows, centred on a point given in per-mille of the frame and held
 * inside it, its width a multiple of 4 and its height and corner even; and
 * the widest zoom a HUD box takes before its picture would be scaled up. */
#include "../src/hud_crop.h"

#include <cstdio>

using namespace woc;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static bool is(const MapRect &r, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

int main()
{
    ok("100 %: the whole frame", is(hud_crop(1088, 480, 100, 500, 500), 0, 0, 1088, 480));
    ok("200 % at the centre: the middle 544x240", is(hud_crop(1088, 480, 200, 500, 500), 272, 120, 544, 240));
    ok("160 %: 680x300", is(hud_crop(1088, 480, 160, 500, 500), 204, 90, 680, 300));
    ok("250 % at the bottom right corner: held inside, x and y even",
       is(hud_crop(1088, 480, 250, 1000, 1000), 656, 288, 432, 192));
    ok("250 % at the top left corner: held inside", is(hud_crop(1088, 480, 250, 0, 0), 0, 0, 432, 192));
    ok("an odd centre: x and y even", hud_crop(1088, 480, 200, 501, 503).x % 2 == 0 && hud_crop(1088, 480, 200, 501, 503).y % 2 == 0);
    bool all = true;
    for (unsigned z = 100; z <= 250; z++)
        for (unsigned cx = 0; cx <= 1000; cx += 50)
            for (unsigned cy = 0; cy <= 1000; cy += 50) {
                const MapRect r = hud_crop(1088, 480, z, cx, cy);
                all = all && r.w % 4 == 0 && r.h % 2 == 0 && r.x % 2 == 0 && r.y % 2 == 0 && r.x + r.w <= 1088 &&
                      r.y + r.h <= 480 && r.w >= 1088 * 100 / z - 3 && r.h >= 480 * 100 / z - 1;
            }
    ok("every zoom 100-250 at any centre: inside the frame, aligned, the zoom's size", all);
    ok("another frame (Android Auto's 1280x564 crop as decoded): the middle at 200 %",
       is(hud_crop(1088, 480, 200, 500, 500), 272, 120, 544, 240) && is(hud_crop(1280, 564, 200, 500, 500), 320, 140, 640, 282));
    ok("out of range: below 100 the whole frame, above 250 as 250",
       is(hud_crop(1088, 480, 50, 500, 500), 0, 0, 1088, 480) && is(hud_crop(1088, 480, 400, 500, 500), 328, 144, 432, 192));
    ok("the cap: 240 % for the standard 452 px box, 160 % at 85 % (680 px)",
       hud_zoom_cap(1088, 452) == 240 && hud_zoom_cap(1088, 680) == 160);
    ok("the cap: never below 100 or above 250, 100 without a box",
       hud_zoom_cap(1088, 1088) == 100 && hud_zoom_cap(1088, 200) == 250 && hud_zoom_cap(1088, 0) == 100);
    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
