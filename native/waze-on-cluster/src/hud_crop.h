// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 lukher-dev
/* The HUD map's zoom: the part of the decoded picture the HUD shows, scaled
 * to the HUD's box (MapFeed). A zoom of z % shows the picture's middle
 * 100/z of each side, centred on a point given in per mille of the picture
 * (hud_crop_cx_pm, hud_crop_cy_pm: 500, 500 is the middle) and held inside
 * it; its width a multiple of 4 and its height, x and y even, as the atlas
 * and the scaler need. The scaler only scales down, so a box takes at most
 * hud_zoom_cap(): the part never narrower than the box. */
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

}  // namespace woc

#endif
