// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 lukher-dev
/* The HUD map's zoom: the part of the decoded picture the HUD shows, scaled
 * to the HUD's box (MapFeed). A zoom of z % shows the picture's middle
 * 100/z of each side, centred on a point given in per mille of the picture
 * (hud_crop_cx_pm, hud_crop_cy_pm: 500, 500 is the middle) and held inside
 * it; its width a multiple of 4 and its height, x and y even, as the atlas
 * and the scaler need. The scaler only scales down, so a box takes at most
 * hud_zoom_cap(): the part never narrower than the box.
 *
 * A map wider than the HUD (hud_width_pct above 100) is shown in a box the
 * scene's width: hud_window() is the part of it the box shows. */
#ifndef WOC_HUD_CROP_H
#define WOC_HUD_CROP_H

#include <stdint.h>

#include "map_rect.h"

namespace woc {

const unsigned kHudZoomMin = 100, kHudZoomMax = 250;

/* The widest zoom a box box_w wide takes of a picture fw wide (100 without a box). */
inline unsigned hud_zoom_cap(uint32_t fw, uint32_t box_w)
{
    if (!box_w)
        return kHudZoomMin;
    const uint64_t z = (uint64_t)fw * 100 / box_w;
    return z < kHudZoomMin ? kHudZoomMin : z > kHudZoomMax ? kHudZoomMax : (unsigned)z;
}

/* The part of an fw x fh picture zoom_pct shows (held to 100-250). */
inline MapRect hud_crop(uint32_t fw, uint32_t fh, unsigned zoom_pct, unsigned cx_pm, unsigned cy_pm)
{
    const unsigned z = zoom_pct < kHudZoomMin ? kHudZoomMin : zoom_pct > kHudZoomMax ? kHudZoomMax : zoom_pct;
    MapRect r;
    r.w = fw;
    r.h = fh;
    if (z == kHudZoomMin)
        return r;
    r.w = (uint32_t)((uint64_t)fw * 100 / z) & ~3u;
    r.h = (uint32_t)((uint64_t)fh * 100 / z) & ~1u;
    const int64_t cx = (int64_t)fw * (cx_pm > 1000 ? 1000 : cx_pm) / 1000;
    const int64_t cy = (int64_t)fh * (cy_pm > 1000 ? 1000 : cy_pm) / 1000;
    int64_t x = cx - r.w / 2, y = cy - r.h / 2;
    x = x < 0 ? 0 : x > (int64_t)(fw - r.w) ? (int64_t)(fw - r.w) : x;
    y = y < 0 ? 0 : y > (int64_t)(fh - r.h) ? (int64_t)(fh - r.h) : y;
    r.x = (uint32_t)x & ~1u;
    r.y = (uint32_t)y & ~1u;
    return r;
}

/* The window a box box_w wide shows of part, a map virt_w wide (its
 * "virtual" width, wider than the box above 100 %), in a picture fw wide:
 * the part's middle box_w/virt_w of its width (a multiple of 4, never
 * narrower than the box), its whole height, moved by pan output pixels (the
 * map moved right by n shows n more of its left side) and held inside the
 * picture. *pan_used: the pan it is at, as far as it goes. A map no wider
 * than its box: all of part, no pan. */
inline MapRect hud_window(const MapRect &part, uint32_t fw, uint32_t box_w, uint32_t virt_w, int pan, int *pan_used)
{
    *pan_used = 0;
    if (!virt_w || virt_w <= box_w || !part.w || part.x + part.w > fw)
        return part;
    uint32_t cw = (uint32_t)((uint64_t)part.w * box_w / virt_w) & ~3u;
    cw = cw < (box_w & ~3u) ? box_w & ~3u : cw;
    cw = cw > part.w ? part.w : cw;
    const int64_t centre = (int64_t)part.x + (part.w - cw) / 2, right = (int64_t)fw - cw - centre;
    /* the pan in output pixels each way before the window meets the picture's edge */
    const int64_t pmax = centre * virt_w / part.w, pmin = -(right * virt_w / part.w);
    const int64_t p = pan > pmax ? pmax : pan < pmin ? pmin : pan;
    int64_t x = p == pmax ? 0 : p == pmin ? (int64_t)fw - cw : centre - (p * part.w + (p < 0 ? -1 : 1) * (int64_t)virt_w / 2) / virt_w;
    x = x < 0 ? 0 : x > (int64_t)(fw - cw) ? (int64_t)(fw - cw) : x;
    *pan_used = (int)p;
    MapRect r = part;
    r.x = (uint32_t)x & ~1u;
    r.w = cw;
    return r;
}

}  // namespace woc

#endif
