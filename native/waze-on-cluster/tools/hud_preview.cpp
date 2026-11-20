// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 lukher-dev
/* hud-preview: what the HUD shows of a map picture, on the host, with the
 * HUD feed's own code: the HUD box (MapFeed's quad, the size setting and the
 * offset), the zoom's part (hud_crop.h, at the map's cap) and, above 100 %,
 * its window (hud_window, panned by the x offset), the scaler (atlas_scale.h,
 * rounded corners, the night map's background made black, the speed patch),
 * drawn on the 800x480 HUD scene's black. For choosing
 * hud_crop_cx_pm / hud_crop_cy_pm and the HUD settings from a picture of the
 * phone's cluster map.
 *
 *   hud-preview --grid out.bmp
 *       writes a 1088x480 test picture: a grid every 64 px on dark grey,
 *       a red marker at the middle (544, 240)
 *   hud-preview in.bmp out.bmp [--zoom 100..250] [--cx-pm N] [--cy-pm N]
 *               [--width-pct 0|20..135] [--x N] [--y N]
 *               [--clear X,Y,W,H | --no-clear] [--no-round] [--no-black]
 *       in: a 24-bit 1088x480 BMP (macOS: sips -s format bmp in.png --out
 *       in.bmp); out: the 800x480 HUD scene. Prints the box, the zoom used,
 *       the part shown and where the red marker landed (its centroid).
 *       The defaults are the shipped ones but the size (0) and the offset
 *       (0,0): the speed patch 220x90 at 110,320 of the scene. */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "atlas_scale.h"
#include "hud_crop.h"
#include "map_geometry.h"

using namespace woc;

namespace {

const uint32_t kW = 1088, kH = 480, kHudW = 800, kHudH = 480;

struct Rgb {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;   /* r, g, b rows, top first */
};

uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
void put32(std::string *s, uint32_t v) { for (int i = 0; i < 4; i++) s->push_back((char)(v >> (8 * i))); }
void put16(std::string *s, uint32_t v) { for (int i = 0; i < 2; i++) s->push_back((char)(v >> (8 * i))); }

bool read_bmp(const char *path, Rgb *out)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M')
        return false;
    const uint32_t off = le32(&b[10]), w = le32(&b[18]);
    const int32_t hh = (int32_t)le32(&b[22]);
    const uint32_t h = (uint32_t)std::abs(hh), bpp = b[28] | b[29] << 8, comp = le32(&b[30]);
    if ((bpp != 24 && bpp != 32) || (comp != 0 && comp != 3))
        return false;
    const uint32_t bytes = bpp / 8, stride = (w * bytes + 3) & ~3u;
    if (b.size() < off + (size_t)stride * h)
        return false;
    out->w = w;
    out->h = h;
    out->px.assign((size_t)w * h * 3, 0);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = &b[off + (size_t)stride * (hh > 0 ? h - 1 - y : y)];
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *o = &out->px[((size_t)y * w + x) * 3];
            o[0] = row[x * bytes + 2];
            o[1] = row[x * bytes + 1];
            o[2] = row[x * bytes];
        }
    }
    return true;
}

bool write_bmp(const char *path, const Rgb &img)
{
    const uint32_t stride = (img.w * 3 + 3) & ~3u;
    std::string s = "BM";
    put32(&s, 54 + stride * img.h);
    put32(&s, 0);
    put32(&s, 54);
    put32(&s, 40);
    put32(&s, img.w);
    put32(&s, img.h);
    put16(&s, 1);
    put16(&s, 24);
    for (int i = 0; i < 6; i++)
        put32(&s, 0);
    for (uint32_t y = 0; y < img.h; y++) {
        const uint8_t *row = &img.px[(size_t)(img.h - 1 - y) * img.w * 3];
        for (uint32_t x = 0; x < img.w; x++) {
            s.push_back((char)row[x * 3 + 2]);
            s.push_back((char)row[x * 3 + 1]);
            s.push_back((char)row[x * 3]);
        }
        s.append(stride - img.w * 3, '\0');
    }
    std::ofstream f(path, std::ios::binary);
    f.write(s.data(), (std::streamsize)s.size());
    return (bool)f;
}

uint8_t clamp8(double v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : std::lround(v)); }

/* BT.601 limited range, 4:2:0: Y (w x h), then U and V side by side (w/2 each) in h/2 rows. */
std::vector<uint8_t> to_atlas(const Rgb &img)
{
    const uint32_t w = img.w, h = img.h;
    std::vector<uint8_t> a((size_t)w * (h + h / 2));
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = &img.px[((size_t)y * w + x) * 3];
            a[(size_t)y * w + x] = clamp8(16 + 0.257 * p[0] + 0.504 * p[1] + 0.098 * p[2]);
        }
    for (uint32_t y = 0; y < h / 2; y++)
        for (uint32_t x = 0; x < w / 2; x++) {
            double r = 0, g = 0, b = 0;
            for (uint32_t dy = 0; dy < 2; dy++)
                for (uint32_t dx = 0; dx < 2; dx++) {
                    const uint8_t *p = &img.px[((size_t)(2 * y + dy) * w + 2 * x + dx) * 3];
                    r += p[0] / 4.0;
                    g += p[1] / 4.0;
                    b += p[2] / 4.0;
                }
            a[(size_t)(h + y) * w + x] = clamp8(128 - 0.148 * r - 0.291 * g + 0.439 * b);
            a[(size_t)(h + y) * w + w / 2 + x] = clamp8(128 + 0.439 * r - 0.368 * g - 0.071 * b);
        }
    return a;
}

void atlas_rgb(const std::vector<uint8_t> &a, uint32_t w, uint32_t h, Rgb *out, uint32_t x0, uint32_t y0)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const double Y = 1.164 * (a[(size_t)y * w + x] - 16);
            const double U = a[(size_t)(h + y / 2) * w + x / 2] - 128.0, V = a[(size_t)(h + y / 2) * w + w / 2 + x / 2] - 128.0;
            uint8_t *o = &out->px[((size_t)(y0 + y) * out->w + x0 + x) * 3];
            o[0] = clamp8(Y + 1.596 * V);
            o[1] = clamp8(Y - 0.392 * U - 0.813 * V);
            o[2] = clamp8(Y + 2.017 * U);
        }
}

/* MapFeed::quad with the scaler: the HUD box for a size setting, moved by the
 * offset and held inside the scene; *virt_w: the map's width (wider than
 * the box above 100 %, whose x offset pans the picture instead). */
crk::QuadRect hud_box(unsigned pct, int dx, int dy, uint32_t *virt_w)
{
    crk::QuadRect q;
    if (!pct) {
        q = crk::map_quad(kHudW, kHudH, kW, kH);
        *virt_w = q.w;
    } else {
        uint64_t w = (uint64_t)kHudW * (pct > 135 ? 135 : pct) / 100;
        uint64_t h = (w * kH + kW / 2) / kW;
        if (h > kHudH) {
            h = kHudH;
            w = (h * kW + kH / 2) / kH;
        }
        *virt_w = (uint32_t)w;
        w = w > kHudW ? kHudW : w;
        q = crk::QuadRect{(uint32_t)((kHudW - w) / 2), (uint32_t)((kHudH - h) / 2), (uint32_t)w, (uint32_t)h};
    }
    const long x = (long)q.x + (*virt_w > q.w ? 0 : dx), y = (long)q.y + dy;
    const long xmax = (long)kHudW - (long)q.w, ymax = (long)kHudH - (long)q.h;
    q.x = (uint32_t)(x < 0 ? 0 : x > xmax ? xmax : x);
    q.y = (uint32_t)(y < 0 ? 0 : y > ymax ? ymax : y);
    return q;
}

int grid(const char *path)
{
    Rgb img;
    img.w = kW;
    img.h = kH;
    img.px.assign((size_t)kW * kH * 3, 40);
    for (uint32_t y = 0; y < kH; y++)
        for (uint32_t x = 0; x < kW; x++)
            if (x % 64 == 0 || y % 64 == 0)
                std::memset(&img.px[((size_t)y * kW + x) * 3], 230, 3);
    for (uint32_t y = kH / 2 - 6; y < kH / 2 + 6; y++)
        for (uint32_t x = kW / 2 - 6; x < kW / 2 + 6; x++) {
            uint8_t *p = &img.px[((size_t)y * kW + x) * 3];
            p[0] = 255;
            p[1] = p[2] = 0;
        }
    return write_bmp(path, img) ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && !std::strcmp(argv[1], "--grid"))
        return grid(argv[2]);
    if (argc < 3) {
        std::fprintf(stderr, "usage: hud-preview --grid out.bmp | in.bmp out.bmp [--zoom N] [--cx-pm N] [--cy-pm N] "
                             "[--width-pct N] [--x N] [--y N] [--clear X,Y,W,H | --no-clear] [--no-round] "
                             "[--no-black]\n");
        return 2;
    }
    unsigned zoom = 100, cx = 500, cy = 500, pct = 0;
    unsigned clear[4] = {110, 320, 220, 90};
    int dx = 0, dy = 0;
    bool round = true, black = true, clear_on = true;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        const auto num = [&]() { return i + 1 < argc ? (unsigned)std::strtoul(argv[++i], nullptr, 10) : 0u; };
        if (a == "--zoom") zoom = num();
        else if (a == "--cx-pm") cx = num();
        else if (a == "--cy-pm") cy = num();
        else if (a == "--width-pct") pct = num();
        else if (a == "--x") dx = i + 1 < argc ? std::atoi(argv[++i]) : 0;
        else if (a == "--y") dy = i + 1 < argc ? std::atoi(argv[++i]) : 0;
        else if (a == "--no-clear") clear_on = false;
        else if (a == "--clear" && i + 1 < argc &&
                 std::sscanf(argv[++i], "%u,%u,%u,%u", &clear[0], &clear[1], &clear[2], &clear[3]) == 4) clear_on = true;
        else if (a == "--no-round") round = false;
        else if (a == "--no-black") black = false;
        else { std::fprintf(stderr, "hud-preview: unknown option %s\n", a.c_str()); return 2; }
    }
    Rgb in;
    if (!read_bmp(argv[1], &in) || in.w != kW || in.h != kH) {
        std::fprintf(stderr, "hud-preview: %s is not a 24-bit %ux%u BMP\n", argv[1], kW, kH);
        return 1;
    }
    /* as MapFeed's HUD branch (hud_shape) */
    uint32_t virt = 0;
    const crk::QuadRect q = hud_box(pct, dx, dy, &virt);
    const uint32_t sw = q.w & ~3u, sh = q.h & ~3u;
    const bool wide = virt > q.w;
    const unsigned used = std::min(std::max(zoom, kHudZoomMin), hud_zoom_cap(kW, wide ? virt : sw));
    const MapRect part = hud_crop(kW, kH, used, cx, cy);
    int pan = 0;
    const MapRect crop = wide ? hud_window(part, kW, sw, virt, dx, &pan) : part;
    const uint32_t dw = std::min(sw, crop.w) & ~3u, dh = std::min(sh, crop.h) & ~3u;
    AtlasScaler scaler;
    const bool cropped = used > kHudZoomMin || wide;
    if (!scaler.configure(kW, kH, dw, dh, 2, cropped ? crop.x : 0, cropped ? crop.y : 0, cropped ? crop.w : 0,
                          cropped ? crop.h : 0)) {
        std::fprintf(stderr, "hud-preview: the scaler refuses %ux%u from %ux%u\n", dw, dh, crop.w, crop.h);
        return 1;
    }
    const uint32_t x0 = q.x + (q.w - dw) / 2, y0 = q.y + (q.h - dh) / 2;
    scaler.set_corner_radius(round ? (dh / 12 > 6 ? dh / 12 : 6) : 0);
    scaler.set_black_background(black);
    if (clear_on)
        scaler.set_clear((int32_t)clear[0] - (int32_t)x0, (int32_t)clear[1] - (int32_t)y0, clear[2], clear[3]);
    std::vector<uint8_t> out;
    scaler.scale(to_atlas(in).data(), &out);
    Rgb hud;
    hud.w = kHudW;
    hud.h = kHudH;
    hud.px.assign((size_t)kHudW * kHudH * 3, 0);
    atlas_rgb(out, dw, dh, &hud, x0, y0);
    if (!write_bmp(argv[2], hud))
        return 1;
    double mx = 0, my = 0, n = 0;
    for (uint32_t y = 0; y < kHudH; y++)
        for (uint32_t x = 0; x < kHudW; x++) {
            const uint8_t *p = &hud.px[((size_t)y * kHudW + x) * 3];
            if (p[0] > 150 && p[1] < 100 && p[2] < 100) {
                mx += x;
                my += y;
                n++;
            }
        }
    std::printf("box %ux%u at %u,%u (map %u wide%s); zoom %u%% (asked %u%%); part %ux%u at %u,%u; shown %ux%u at %u,%u, its "
                "middle %u,%u\n", q.w, q.h, q.x, q.y, virt, wide ? ", wider than the HUD" : "", used, zoom, crop.w, crop.h, crop.x, crop.y,
                dw, dh, x0, y0, x0 + dw / 2, y0 + dh / 2);
    if (wide)
        std::printf("pan %+d (asked %+d)\n", pan, dx);
    if (n)
        std::printf("marker %.1f,%.1f\n", mx / n, my / n);
    else
        std::printf("marker none\n");
    return 0;
}
