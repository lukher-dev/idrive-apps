// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Host test of the map feed (map_feed.h) against the shared cluster renderer
 * (cluster-render, the CR tree) on fake display backends, over its socket:
 * - the map is a contending content layer on the map target, covering the
 *   1088x480 slot; before a frame the app sees idle, with the first frames
 *   the slot is asked for (starting) and the map shown with the frames as
 *   they were decoded;
 * - the stock map taking the slot shows as starting with that reason; the
 *   contending map asks again, parks behind it, takes it over and is shown,
 *   the stock map parked behind it;
 * - frames stopping: idle after stall_ms, the slot released;
 * - the renderer restarting: starting while it is gone, an error once it
 *   stays gone 15 s, then the map again with the next frames;
 * - a lowered shared upload cap shows on the status line, with its reason in
 *   the status, and leaves the line once it is back at the ceiling;
 * - frames offered faster than they can be written are newest-wins;
 * - the stream not live: the layer hidden, idle, the slot released and the
 *   stock map back; live again: shown with the frames; frozen: hidden, the
 *   status says so; no longer frozen: shown again;
 * - before the stream is live the layer is made hidden;
 * - the stream's pictures through the change sampler and freeze watch, as
 *   the service runs them: a stream blank from its start (the car's burst,
 *   then a pause) never asks for the slot, the stock map stays and the app
 *   is told it waits for the phone's map; the first non-blank frame, with no frame
 *   after it, takes the slot over and shows that frame; blank spells
 *   shorter than blank_ms between pictures keep the map up without asking
 *   for the slot again; a picture blank for blank_ms gives the slot back to
 *   the stock map, and the next picture takes it again; a stream blank at
 *   full rate from its start is frozen, the stock map showing; live
 *   through a recent frame (no connection to read), a static map's frame
 *   every 10 s takes the slot over each time it makes the stream live;
 *   a frame of the ended stream decoded after it ended, seconds before the
 *   next stream is live, does not take the slot for it, while one decoded
 *   just before it is live does; the stream ending is logged once, whatever
 *   frames still come;
 * - the status is logged when its state, reason or target changes, not for
 *   the frame rate, the page or the upload cap;
 * - BMW's turn box (the layer's guide_hold): hidden by default, the hold of
 *   its slot following the map; the switch off releases it and the box
 *   shows, the map still showing; on again, held; off from the start,
 *   never held; the status never says the hold is disabled;
 * - only what the renderer needs goes: a picture repeated (as the producer
 *   hands over a picture the same as the last) keeps a static map showing
 *   past stall_ms; nothing reaches the renderer while the layer is hidden,
 *   and shown again it draws the newest picture first; after the renderer
 *   restarts, and on another target, repeats alone bring the picture;
 * - MapFeeds: the HUD off, on beside the cluster (scaled, at most hud_fps),
 *   beside the cluster's map on panel, its size and place, kept shown over
 *   BMW's pop-ups or giving way to them, alone, neither; a still map's repeats keep
 *   both layers, or the HUD alone, shown past stall_ms with no frame whole
 *   and none scaled, and moving again the HUD scales at most hud_fps while
 *   the cluster keeps its rate; a feed made beside the other starts from its
 *   picture;
 * - Off (neither), from the cluster alone and from both: the map's and the
 *   HUD's slots and the turn box's hold released, the stock map and box
 *   shown again, nothing asked of the renderer while off; the cluster, or
 *   both, again: the next frame shown, the hold back;
 * - hosted by another app's scene (hosting): the map in its scene at the
 *   host's rect with the host's part of the picture, its own slot released;
 *   hidden by the host, no layer and nothing sent; shown again; the host
 *   gone, back on its own target; no part asked of the decoder (by default);
 * - hosted with hosted_vpp_scale (vpp_hosting): the host's part asked of the
 *   decoder at the size it is shown at, a whole frame meanwhile scaled by
 *   the worker, a VPP-made one sent as it is, one of another part dropped,
 *   the host's move asking for its new part, the host gone, none;
 * - always_packed (always_packed_map): 0, the layer without always_packed and a still
 *   map exact; 1, every update packed, a still map's too, never exact; with
 *   hosted_vpp_scale the VPP-made part packed; a host's part not a multiple
 *   of 4 high as with 0, shown; cluster-render upgraded or downgraded under
 *   the feed, the layer made again for it (renderer_changed). */
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

/* cluster-render's service and fakes, from its tree (-I$(CR)) */
#include "src/service.h"
#include "src/video_layer.h"
#include "block_pack.h"
#include "cluster_render/protocol.h"
#include "test/fake_graph.h"
#include "test/fake_port.h"
#include "../src/map_feed.h"
#include "src/log.h"
#include "../src/map_feeds.h"
#include "../src/atlas_scale.h"

using namespace woc;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static int rigs;
static const uint64_t kBox = 0x01000310, kGuide = 0x010004f1;

struct Rig {
    std::string path = "/tmp/woc-feed-test-" + std::to_string(getpid()) + "-" + std::to_string(++rigs) + ".sock";
    crs::FakeGpu gpu;
    crs::FakePort port;
    crs::ServiceConfig sc;
    std::unique_ptr<crs::Service> svc;
    std::unique_ptr<MapFeed> feed;
    int64_t now = 200000;
    std::vector<uint8_t> spare;   /* the buffer handed to the feed, as the producer's */
    /* watched: the frames' pictures drive the stream state, as in the
     * service's loop; otherwise the stream is live with a picture */
    bool watched;
    /* watched and > 0: live while a frame came within this long, as the
     * service decides without the stream's connection; 0: live while live
     * is set, as the stream's connection says */
    int64_t live_stall_ms = 0;
    bool live = true;
    /* frames the same as the last offered go as repeats, as the producer
     * hands them over */
    bool repeats = false;
    /* set: the frames are this part, made by VPP (hosted_vpp_scale) */
    OutputPart part;
    std::vector<uint8_t> offered;
    int64_t last_frame_ms = -1;
    FreezeWatch watch{FreezeConfig()};
    ChangeSampler sampler{1088, 480};
    cr::Client *other = nullptr;   /* another app of the renderer, polled with the feed */

    /* w: watched (above); hold: with the guide hold of the turn box's slot
     * following the map, as cluster-render.conf ships it; guide_hold: the
     * feed's setting; hud: the HUD target too. */
    explicit Rig(bool w = false, unsigned stall_ms = 6000, bool hold = false, bool guide_hold = true, bool hud = false)
        : watched(w)
    {
        crs::TargetSpec m, p, g, u;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 1500;
        m.take_over_stock = true;   /* as cluster-render.conf ships it */
        p.name = "panel"; p.slot = 0x203; p.scene_id = 0x010004f8; p.width = 1920; p.height = 720; p.linger_ms = 3000;
        g.name = "guide_0x105"; g.slot = 0x105; g.scene_id = kGuide; g.width = g.height = 16; g.linger_ms = 0;
        g.take_over_stock = true; g.follows = "map"; g.take_over_contents = {kBox};
        u.name = "hud"; u.slot = 0x323; u.scene_id = 0x01000400; u.width = 800; u.height = 480; u.linger_ms = 3000;
        u.yield_slots = {0x322};   /* as cluster-render.conf ships it */
        u.yield_ms = 1500;
        sc.targets = {m, p};
        if (hud)
            sc.targets.push_back(u);
        if (hold)
            sc.targets.push_back(g);
        sc.socket_path = path;
        sc.backend = "fake";
        start();
        FeedConfig fc;
        fc.socket = path;
        fc.video.level_bits = 0;
        fc.video.stall_ms = stall_ms;
        fc.guide_hold = guide_hold;
        feed.reset(new MapFeed(fc));
        watch.live(true, now);
        feed->set_stream(true, false, watched ? watch.picture(now) : Picture::Shown);
    }
    void start()
    {
        svc.reset(new crs::Service(&gpu, &port, sc));
        port.m = &svc->slots();
        port.pending.clear();
        std::string err;
        if (!svc->start(&err))
            std::printf("start: %s\n", err.c_str());
    }
    /* Simulated time; each step runs the socket exchange until idle. */
    void pump(int64_t ms, const std::vector<uint8_t> *frame = nullptr, int64_t frame_every = 65)
    {
        for (int64_t t = 0; t < ms; t += 5) {
            now += 5;
            if (frame && t % frame_every == 0) {
                if (repeats && offered == *frame) {
                    feed->repeat((uint64_t)now, now);
                } else {
                    spare.assign(frame->begin(), frame->end());
                    if (part.set())
                        feed->offer_part(&spare, (uint64_t)now, part, now);
                    else
                        feed->offer(&spare, (uint64_t)now, now);
                    offered = *frame;
                }
                if (watched) {
                    const bool changed = sampler.changed(frame->data());
                    watch.frame(now, changed, sampler.blank());
                    last_frame_ms = now;
                }
            }
            if (watched) {
                const bool on = live_stall_ms ? last_frame_ms >= 0 && now - last_frame_ms <= live_stall_ms : live;
                watch.live(on, now);
                feed->set_stream(on, watch.frozen(now), watch.picture(now));
            }
            for (int round = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                size_t first = 0;
                if (svc)
                    first = svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                if (svc) {
                    port.tick(now);
                    svc->after_poll(fds, first, now);
                }
                const bool pending = feed->wants_write() || (other && other->wants_write());
                feed->poll(now);
                if (other)
                    other->poll(now);
                if (round > 0 && n <= 0 && !pending)
                    break;
            }
        }
    }
    FeedStatus st() { return feed->status("decode"); }
    /* The map status of content in the turn box's slot; -1: not mapped. */
    int in_box_slot(uint64_t content) const
    {
        const crs::SlotInfo *s = port.row(0x105, content);
        return s ? (int)s->map_status : -1;
    }
    const crs::Layer *map_layer()
    {
        for (const crs::Layer *l : svc->compositor().layers())
            return l;
        return nullptr;
    }
    /* The map layer's frames the renderer has counted: written, unchanged,
     * within the threshold, superseded. */
    unsigned long long received()
    {
        unsigned long long n[4] = {};
        if (const crs::Layer *l = map_layer())
            std::sscanf(l->stats().c_str(), "%llu sent, %llu unchanged, %llu within threshold, %llu superseded", &n[0],
                        &n[1], &n[2], &n[3]);
        return n[0] + n[1] + n[2] + n[3];
    }
    /* Whether the target's scene draws the map, and with this atlas. */
    bool drawing(const char *target, const std::vector<uint8_t> &f, bool *with)
    {
        crs::FakeGraph *g = gpu.graph(target);
        *with = false;
        if (!g || g->drawn().empty())
            return false;
        for (const auto &kv : g->shown.buffers)
            *with = *with || (kv.second.f == crs::TexFormat::R8 && kv.second.px == f);
        return true;
    }
};

static std::vector<uint8_t> atlas(int seed)
{
    std::vector<uint8_t> f(1088 * 720);
    for (size_t i = 0; i < f.size(); i++)
        f[i] = (uint8_t)(i < 1088 * 480 ? (i / 5 + seed * 17) & 0xff : 128);
    return f;
}

/* The app's switch for BMW's turn box: on by default, the hold following
 * the map and the box taken over; off, the hold released and the box
 * shown, the map still showing; on again, held. */
static void guide_hold()
{
    const std::vector<uint8_t> f = atlas(7);
    Rig h(false, 6000, true);
    h.port.stock_map(0x105, kBox);
    h.pump(1500, &f);
    ok("turn box: hidden by default, the hold shown in its slot with the box parked behind it",
       h.st().state == "showing" && h.st().guide_hold && h.in_box_slot(kGuide) == 1 && h.in_box_slot(kBox) == 3);
    h.feed->set_guide_hold(false);
    h.pump(500, &f);
    const size_t taken = h.port.taken_over.size();
    ok("turn box: the switch off releases the hold, the box shown, the map still showing, the status says so",
       h.in_box_slot(kGuide) == -1 && h.in_box_slot(kBox) == 1 && h.st().state == "showing" && !h.st().guide_hold);
    h.pump(5000, &f);
    ok("turn box: off, no take-over and no hold", h.port.taken_over.size() == taken && h.in_box_slot(kGuide) == -1);
    h.feed->set_guide_hold(true);
    h.pump(500, &f);
    ok("turn box: on again, held, the box taken over",
       h.in_box_slot(kGuide) == 1 && h.in_box_slot(kBox) == 3 && h.port.taken_over.back() == kBox);

    Rig off(false, 6000, true, false);
    off.port.stock_map(0x105, kBox);
    off.pump(3000, &f);
    ok("turn box: set off before the map is made, never held",
       off.st().state == "showing" && off.in_box_slot(kGuide) == -1 && off.in_box_slot(kBox) == 1);
    ok("turn box: the hold not disabled, the status says so", !h.st().guide_disabled);

}

/* The HUD's worker is still scaling a frame: the feed then asks to be woken
 * in kScaleWaitMs however much later it is asked, while a frame due in
 * kScaleWaitMs asks for a millisecond less a millisecond later. */
static bool hud_scaling(const MapFeeds &feeds, int64_t now)
{
    return feeds.wake_in_ms(now) == MapFeed::kScaleWaitMs && feeds.wake_in_ms(now + 1) == MapFeed::kScaleWaitMs;
}

/* The app's Off (MapFeeds on neither), as the app sends it: from the cluster
 * alone the cluster off; from both each off. Off releases the map's slot and
 * the turn box's hold, so the stock map and box show again, and the HUD's
 * slot; nothing is asked of the renderer while off. A display chosen again
 * shows the next frame, the hold back with it. */
static void show_off()
{
    const std::vector<uint8_t> f1 = atlas(11), f2 = atlas(12), f3 = atlas(13);
    const uint64_t kStock = 0x01000101, kMap = 0x010004f0, kHud = 0x01000400;
    Rig b(false, 6000, true, true, true);
    b.feed.reset();
    b.start();
    b.port.stock_map(0x100, kStock);
    b.port.stock_map(0x105, kBox);
    FeedConfig fc;
    fc.socket = b.path;
    fc.video.level_bits = 0;
    fc.guide_hold = true;
    MapFeeds feeds(fc, true, false, 10);
    feeds.set_stream(true, false, Picture::Shown);
    std::vector<uint8_t> spare;
    bool asked = false;
    const auto pump = [&](int64_t ms, const std::vector<uint8_t> *frame) {
        for (int64_t t = 0; t < ms; t += 5) {
            b.now += 5;
            if (frame && t % 65 == 0 && feeds.wants_frames()) {   /* the service offers only then */
                spare.assign(frame->begin(), frame->end());
                spare[(size_t)(t / 65) % 1088] ^= 0x80;
                feeds.offer(&spare, (uint64_t)b.now, b.now);
            }
            for (int round = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                const size_t first = b.svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                b.port.tick(b.now);
                b.svc->after_poll(fds, first, b.now);
                const bool pending = (feeds.cluster() && feeds.cluster()->wants_write()) || (feeds.hud_feed() && feeds.hud_feed()->wants_write());
                const bool scaling = hud_scaling(feeds, b.now);
                if (scaling)
                    usleep(1000);
                feeds.poll(b.now);
                for (int i = 0; i < 4; i++)
                    asked = asked || b.svc->slots().requested(i);
                if (round > 0 && n <= 0 && !pending && !scaling)
                    break;
            }
        }
    };
    const auto status_of = [&](uint64_t slot, uint64_t content) -> int {
        const crs::SlotInfo *r = b.port.row(slot, content);
        return r ? (int)r->map_status : -1;
    };
    const auto layers = [&]() { return b.svc->compositor().layers().size(); };
    const auto shown = [&](const char *target, const std::vector<uint8_t> &like) {
        crs::FakeGraph *g = b.gpu.graph(target);
        bool on = false;
        if (g)
            for (const auto &kv : g->shown.buffers)
                on = on || (kv.second.f == crs::TexFormat::R8 && kv.second.px.size() == like.size() &&
                            std::equal(kv.second.px.begin() + 1088, kv.second.px.end(), like.begin() + 1088));
        return on;
    };
    const auto map_up = [&]() {
        return status_of(0x100, kMap) == 1 && status_of(0x100, kStock) == 3 && status_of(0x105, kGuide) == 1 &&
               status_of(0x105, kBox) == 3;
    };
    const auto stock_back = [&]() {
        return status_of(0x100, kMap) == -1 && status_of(0x100, kStock) == 1 && status_of(0x105, kGuide) == -1 &&
               status_of(0x105, kBox) == 1 && status_of(0x323, kHud) == -1;
    };

    pump(2000, &f1);
    ok("Off: the cluster's map up first, the stock map and box parked behind it and the hold",
       map_up() && shown("map", f1) && feeds.status("decode").state == "showing");

    /* Off from the cluster alone: the app sends the cluster off */
    feeds.set_cluster(false);
    pump(5000, &f2);
    FeedStatus st = feeds.status("decode");
    ok("Off from the cluster: no feed or layer, nothing wanted, the map's slot and the hold released, the stock map and box back",
       !feeds.cluster() && !feeds.hud_feed() && layers() == 0 && !feeds.wants_frames() && stock_back() && st.state == "off" &&
           st.hud_state == "off" && !st.cluster && !st.hud);
    const int requests = b.port.requests;
    asked = false;
    pump(10000, &f2);
    ok("Off: nothing asked of the renderer for 10 s, no layer, the stock map and box still shown",
       b.port.requests == requests && !asked && layers() == 0 && stock_back());

    /* Cluster again: the next frame, the hold back */
    feeds.set_cluster(true);
    pump(2000, &f3);
    ok("Off then Cluster: the map with the next frame, the stock map and box parked again, the hold back",
       map_up() && shown("map", f3) && feeds.status("decode").state == "showing");

    /* Off from both: the app sends the cluster off, then the HUD off */
    feeds.set_hud(true);
    pump(2000, &f1);
    ok("Off: both up first, the HUD's scene in its slot", map_up() && status_of(0x323, kHud) == 1 &&
                                                         feeds.status("decode").hud_state == "showing");
    feeds.set_cluster(false);
    feeds.set_hud(false);
    pump(5000, &f2);
    st = feeds.status("decode");
    ok("Off from both: no feed or layer, the map's and the HUD's slots and the hold released, the stock map and box back",
       !feeds.cluster() && !feeds.hud_feed() && layers() == 0 && !feeds.wants_frames() && stock_back() && st.state == "off" &&
           st.hud_state == "off");
    feeds.set_hud(true);
    feeds.set_cluster(true);
    pump(2000, &f3);
    ok("Off then both: both back with the next frame", map_up() && status_of(0x323, kHud) == 1 && shown("map", f3) &&
                                                         feeds.status("decode").hud_state == "showing");
}

/* The HUD map only near turns (FeedConfig::hud_near_turn): the HUD's layer
 * hidden, its slot let go, until the app says a turn is near, and again
 * when it says it is past or stops saying so for hud_near_expire_ms; the
 * cluster's map never follows it; off, the HUD as always. */
static void near_turns()
{
    const std::vector<uint8_t> f1 = atlas(21), f2 = atlas(22);
    const uint64_t kStock = 0x01000101, kMap = 0x010004f0, kHud = 0x01000400;
    Rig b(false, 6000, false, true, true);
    b.feed.reset();
    b.start();
    b.port.stock_map(0x100, kStock);
    FeedConfig fc;
    fc.socket = b.path;
    fc.video.level_bits = 0;
    fc.hud_near_turn = true;
    fc.hud_near_expire_ms = 10000;
    MapFeeds feeds(fc, true, true, 10);
    feeds.set_stream(true, false, Picture::Shown);
    std::vector<uint8_t> spare;
    const auto pump = [&](int64_t ms, const std::vector<uint8_t> *frame) {
        for (int64_t t = 0; t < ms; t += 5) {
            b.now += 5;
            if (frame && t % 65 == 0 && feeds.wants_frames()) {
                spare.assign(frame->begin(), frame->end());
                spare[(size_t)(t / 65) % 1088] ^= 0x80;
                feeds.offer(&spare, (uint64_t)b.now, b.now);
            }
            for (int round = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                const size_t first = b.svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                b.port.tick(b.now);
                b.svc->after_poll(fds, first, b.now);
                const bool pending = (feeds.cluster() && feeds.cluster()->wants_write()) || (feeds.hud_feed() && feeds.hud_feed()->wants_write());
                const bool scaling = hud_scaling(feeds, b.now);
                if (scaling)
                    usleep(1000);
                feeds.poll(b.now);
                if (round > 0 && n <= 0 && !pending && !scaling)
                    break;
            }
        }
    };
    const auto status_of = [&](uint64_t slot, uint64_t content) -> int {
        const crs::SlotInfo *r = b.port.row(slot, content);
        return r ? (int)r->map_status : -1;
    };
    const auto map_up = [&]() { return status_of(0x100, kMap) == 1; };
    const auto hud_up = [&]() { return status_of(0x323, kHud) == 1; };

    pump(2000, &f1);
    FeedStatus st = feeds.status("decode");
    ok("near turns, at start: the cluster's map up, the HUD's hidden and its slot not asked for, waiting for a turn",
       map_up() && !hud_up() && st.hud_near_turn && !st.hud_gate && st.hud_state == "turn_wait" && st.state == "showing");

    feeds.set_turn_near(true, b.now);
    pump(2000, &f2);
    st = feeds.status("decode");
    ok("a turn near: the HUD's map shown in its slot", hud_up() && st.hud_gate && st.hud_state == "showing" && map_up());

    feeds.set_turn_near(false, b.now);
    pump(5000, &f1);
    st = feeds.status("decode");
    ok("past the turn: the HUD's slot let go after its linger, the cluster's map still up",
       !hud_up() && !st.hud_gate && st.hud_state == "turn_wait" && map_up());

    feeds.set_turn_near(true, b.now);
    pump(6000, &f2);
    feeds.set_turn_near(true, b.now);
    pump(6000, &f1);
    ok("near said again within hud_near_expire_ms: still shown", hud_up() && feeds.status("decode").hud_gate);
    pump(10000, &f2);
    st = feeds.status("decode");
    ok("near not said again for hud_near_expire_ms: hidden, its slot let go", !hud_up() && !st.hud_gate && map_up());

    feeds.set_hud_near_turn(false);
    pump(2000, &f1);
    st = feeds.status("decode");
    ok("near turns off: the HUD's map shown with no turn near", hud_up() && st.hud_gate && !st.hud_near_turn &&
                                                                 st.hud_state == "showing");
    feeds.set_hud_near_turn(true);
    pump(5000, &f2);
    st = feeds.status("decode");
    ok("near turns on again, no turn near: hidden again", !hud_up() && !st.hud_gate && st.hud_state == "turn_wait");

    /* How far before a turn the HUD map shows: the app's to gate on, kept and reported, the gate as it was. */
    ok("near turns: 1000 m before a turn by default", feeds.status("decode").hud_near_m == 1000);
    feeds.set_hud_near_m(400);
    pump(1000, &f1);
    st = feeds.status("decode");
    ok("400 m before a turn: in the status, the gate as it was", st.hud_near_m == 400 && feeds.hud_settings().hud_near_m == 400 &&
                                                                 !hud_up() && !st.hud_gate && st.hud_state == "turn_wait");

    feeds.set_stream(false, false, Picture::None);
    pump(500, nullptr);
    ok("no stream: the HUD's state is the stream's, not waiting for a turn", feeds.status("decode").hud_state != "turn_wait");
}

/* The HUD map's zoom (FeedConfig::hud_zoom_pct): the part of the picture
 * hud_crop() picks, scaled to the HUD's box; a zoom past the box's cap
 * (hud_zoom_cap: the part never smaller than the box) is used at the cap;
 * 100 % is the whole picture as before; the status has the zoom asked and
 * the zoom used. */
static void hud_zoom()
{
    const std::vector<uint8_t> f = atlas(31);
    Rig b(false, 6000, false, true, true);
    b.feed.reset();
    b.start();
    FeedConfig fc;
    fc.socket = b.path;
    fc.video.level_bits = 0;
    fc.hud_round_corners = false;
    fc.hud_black_background = false;
    fc.hud_clear = false;
    MapFeeds feeds(fc, true, true, 10);
    feeds.set_stream(true, false, Picture::Shown);
    std::vector<uint8_t> spare;
    const auto pump = [&](int64_t ms) {
        for (int64_t t = 0; t < ms; t += 5) {
            b.now += 5;
            if (t % 65 == 0 && feeds.wants_frames()) {
                spare.assign(f.begin(), f.end());
                feeds.offer(&spare, (uint64_t)b.now, b.now);
            }
            for (int round = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                const size_t first = b.svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                b.port.tick(b.now);
                b.svc->after_poll(fds, first, b.now);
                const bool pending = (feeds.cluster() && feeds.cluster()->wants_write()) || (feeds.hud_feed() && feeds.hud_feed()->wants_write());
                const bool scaling = hud_scaling(feeds, b.now);
                if (scaling)
                    usleep(1000);
                feeds.poll(b.now);
                if (round > 0 && n <= 0 && !pending && !scaling)
                    break;
            }
        }
    };
    /* the picture the HUD shows: the crop of f scaled to dw x dh as the HUD's worker does */
    const auto scaled = [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t dw, uint32_t dh) {
        AtlasScaler sc;
        std::vector<uint8_t> out;
        if (sc.configure(1088, 480, dw, dh, fc.scaled_level_bits, x, y, w, h))
            sc.scale(f.data(), &out);
        return out;
    };
    const auto hud_shows = [&](const std::vector<uint8_t> &want) {
        crs::FakeGraph *g = b.gpu.graph("hud");
        bool on = false;
        if (g && !want.empty())
            for (const auto &kv : g->shown.buffers)
                on = on || (kv.second.f == crs::TexFormat::R8 && kv.second.px == want);
        return on;
    };

    pump(2000);
    FeedStatus st = feeds.status("decode");
    ok("zoom 250 (the default): used at the standard box's cap, 240, the cluster's own scale: the middle 452x200 1:1",
       hud_shows(scaled(318, 140, 452, 200, 452, 200)) && st.hud_zoom_pct == 250 && st.hud_zoom_used == 240);

    feeds.set_hud_zoom(200);
    pump(2000);
    st = feeds.status("decode");
    ok("zoom 200: the middle 544x240 of the picture scaled to 452x200",
       hud_shows(scaled(272, 120, 544, 240, 452, 200)) && st.hud_zoom_pct == 200 && st.hud_zoom_used == 200 &&
           st.state == "showing");

    feeds.set_hud_width(85);
    feeds.set_hud_zoom(250);
    pump(2000);
    st = feeds.status("decode");
    ok("zoom 250 in the 680x300 box: used at its cap, 160, the middle 680x300 shown 1:1",
       hud_shows(scaled(204, 90, 680, 300, 680, 300)) && st.hud_zoom_pct == 250 && st.hud_zoom_used == 160);

    feeds.set_hud_width(0);
    pump(2000);
    st = feeds.status("decode");
    ok("the box back to standard: the zoom asked, 250, used at that box's cap, 240",
       hud_shows(scaled(318, 140, 452, 200, 452, 200)) && st.hud_zoom_used == 240);

    feeds.set_hud_zoom(100);
    pump(2000);
    ok("zoom 100 again: the whole picture", hud_shows(scaled(0, 0, 1088, 480, 452, 200)) &&
                                              feeds.status("decode").hud_zoom_used == 100);
}

/* The HUD map wider than the HUD (hud_width_pct above 100): its box the
 * scene's width, its height the picture's shape at the map's width, centred;
 * the zoom capped by the map's width, not the box's; the box shows the
 * middle of the (zoomed) picture, which hud_offset_x pans, held inside the
 * picture, while the box stays at x 0; the speed patch (fixed to the
 * scene: it follows the box's place) on the same layer, the
 * newest picture scaled again without a new frame; without the head unit's
 * scaler the map is held to 100 %. */
static void hud_wide()
{
    const std::vector<uint8_t> f = atlas(37);
    Rig b(false, 6000, false, true, true);
    b.feed.reset();
    b.start();
    FeedConfig fc;
    fc.socket = b.path;
    fc.video.level_bits = 0;
    fc.hud_round_corners = false;
    fc.hud_black_background = false;
    fc.hud_clear = false;
    fc.hud_zoom_pct = 100;
    fc.hud_width_pct = 100;
    fc.video.dirty_threshold = 0;   /* every change reaches the renderer: a patch 2 px lower changes only its feather */
    MapFeeds feeds(fc, true, true, 10);
    feeds.set_stream(true, false, Picture::Shown);
    std::vector<uint8_t> spare;
    /* frames: offered every 65 ms; else none, the HUD scales only what it holds */
    const auto pump = [&](int64_t ms, bool frames) {
        for (int64_t t = 0; t < ms; t += 5) {
            b.now += 5;
            if (frames && t % 65 == 0 && feeds.wants_frames()) {
                spare.assign(f.begin(), f.end());
                feeds.offer(&spare, (uint64_t)b.now, b.now);
            }
            for (int round = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                const size_t first = b.svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                b.port.tick(b.now);
                b.svc->after_poll(fds, first, b.now);
                const bool pending = (feeds.cluster() && feeds.cluster()->wants_write()) || (feeds.hud_feed() && feeds.hud_feed()->wants_write());
                const bool scaling = hud_scaling(feeds, b.now);
                if (scaling)
                    usleep(1000);
                feeds.poll(b.now);
                if (round > 0 && n <= 0 && !pending && !scaling)
                    break;
            }
        }
    };
    /* the picture the HUD shows: the part of f at x, y scaled to dw x dh, with a patch */
    const auto scaled = [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t dw, uint32_t dh, int32_t px = 0,
                            int32_t py = 0, uint32_t pw = 0, uint32_t ph = 0) {
        AtlasScaler sc;
        std::vector<uint8_t> out;
        if (sc.configure(1088, 480, dw, dh, fc.scaled_level_bits, x, y, w, h)) {
            sc.set_clear(px, py, pw, ph);
            sc.scale(f.data(), &out);
        }
        return out;
    };
    const auto hud_shows = [&](const std::vector<uint8_t> &want) {
        crs::FakeGraph *g = b.gpu.graph("hud");
        bool on = false;
        if (g && !want.empty())
            for (const auto &kv : g->shown.buffers)
                on = on || (kv.second.f == crs::TexFormat::R8 && kv.second.px == want);
        return on;
    };
    const auto hud_layer = [&]() -> const crs::Layer * {
        for (const crs::Layer *x : b.svc->compositor().layers())
            if (x->target() == 2)
                return x;
        return nullptr;
    };
    const auto at = [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        const crs::Layer *l = hud_layer();
        return l && l->params.rect.x == (int)x && l->params.rect.y == (int)y && l->params.rect.w == w && l->params.rect.h == h;
    };

    pump(2000, true);
    ok("100 %: the HUD's width, 800x352 at 0,63 (the box 353 high), the whole picture scaled",
       at(0, 63, 800, 352) && hud_shows(scaled(0, 0, 1088, 480, 800, 352)) && feeds.status("decode").hud_width_pct == 100);

    feeds.set_hud_width(120);
    feeds.set_hud_zoom(250);
    pump(2000, true);
    FeedStatus st = feeds.status("decode");
    ok("120 % (a map 960 wide): its box the HUD's width, 424 high, centred (0,28)", at(0, 28, 800, 424));
    ok("120 %: the zoom capped by the map's width (113 %), not the box's: the middle 800 of its 960x424 part, 1:1",
       hud_shows(scaled(144, 28, 800, 424, 800, 424)) && st.hud_zoom_used == 113 && st.hud_width_pct == 120);

    feeds.set_hud_zoom(100);
    feeds.set_hud_width(135);
    pump(2000, true);
    ok("135 % (a map 1080 wide): 800x476 at 0,2, the middle 804x480 of the picture scaled to it",
       at(0, 2, 800, 476) && hud_shows(scaled(142, 0, 804, 480, 800, 476)) && feeds.status("decode").hud_zoom_used == 100);
    const crs::Layer *l135 = hud_layer();
    const uint32_t id135 = l135 ? l135->id() : 0;

    feeds.set_hud_offset(50, 0);
    pump(1000, false);
    st = feeds.status("decode");
    ok("135 %, moved 50 right: the box stays at 0,2, it shows 51 px more of the picture's left (92 instead of 142)",
       at(0, 2, 800, 476) && hud_shows(scaled(92, 0, 804, 480, 800, 476)) && st.hud_offset_x == 50 && st.hud_offset_y == 0);
    ok("135 %, moved: the same layer, its newest picture scaled again without a new frame", hud_layer() && hud_layer()->id() == id135);

    feeds.set_hud_offset(800, -800);
    pump(1000, false);
    st = feeds.status("decode");
    ok("135 %, moved too far: the picture's left edge, the box at the top, the offset as far as it goes (140,-2)",
       at(0, 0, 800, 476) && hud_shows(scaled(0, 0, 804, 480, 800, 476)) && st.hud_offset_x == 140 && st.hud_offset_y == -2);

    feeds.set_hud_offset(0, 0);
    feeds.set_hud_clear(true, 110, 320, 220, 90);
    pump(1000, false);
    st = feeds.status("decode");
    ok("the speed patch: black at the scene's 110,320 (the map's 110,318, its box at 0,2), no new frame",
       hud_shows(scaled(142, 0, 804, 480, 800, 476, 110, 318, 220, 90)) && st.hud_clear && st.hud_clear_x == 110 &&
           st.hud_clear_y == 320 && st.hud_clear_w == 220 && st.hud_clear_h == 90);
    feeds.set_hud_offset(0, -2);
    pump(1000, false);
    ok("the speed patch fixed to the scene: the box 2 up, the patch 2 down in the map (110,320)",
       at(0, 0, 800, 476) && hud_shows(scaled(142, 0, 804, 480, 800, 476, 110, 320, 220, 90)) && hud_layer() &&
           hud_layer()->id() == id135);
    feeds.set_hud_width(40);
    feeds.set_hud_offset(0, 0);
    pump(2000, true);
    ok("40 % (320x140 at 240,169): the patch is the scene's rect less the box's place (at -130,151 of the map), clipped to it",
       at(240, 169, 320, 140) && hud_shows(scaled(0, 0, 1088, 480, 320, 140, 110 - 240, 320 - 169, 220, 90)));
    feeds.set_hud_offset(0, -100);
    pump(1000, false);
    ok("40 %, moved up past the patch: none on the map", at(240, 69, 320, 140) && hud_shows(scaled(0, 0, 1088, 480, 320, 140)));
    feeds.set_hud_offset(0, 0);
    feeds.set_hud_clear(false, 110, 320, 220, 90);
    pump(1000, false);
    ok("hud_clear 0: no patch", hud_shows(scaled(0, 0, 1088, 480, 320, 140)) && !feeds.status("decode").hud_clear);

    /* without the head unit's scaler: held to 100 %, no corners, no patch */
    Rig c(false, 6000, false, true, true);
    c.feed.reset();
    c.start();
    FeedConfig nc = fc;
    nc.socket = c.path;
    nc.scale_down = false;
    nc.hud_width_pct = 135;
    nc.hud_round_corners = true;
    nc.hud_clear = true;
    MapFeeds plain(nc, true, true, 10);
    plain.set_stream(true, false, Picture::Shown);
    for (int64_t t = 0; t < 1500; t += 5) {
        c.now += 5;
        if (t % 65 == 0 && plain.wants_frames()) {
            spare.assign(f.begin(), f.end());
            plain.offer(&spare, (uint64_t)c.now, c.now);
        }
        for (int round = 0; round < 400; round++) {
            std::vector<pollfd> fds;
            const size_t first = c.svc->poll_fds(&fds);
            const int n = ::poll(fds.data(), fds.size(), 0);
            c.port.tick(c.now);
            c.svc->after_poll(fds, first, c.now);
            plain.poll(c.now);
            if (round > 0 && n <= 0)
                break;
        }
    }
    const crs::Layer *pl = nullptr;
    for (const crs::Layer *x : c.svc->compositor().layers())
        if (x->target() == 2)
            pl = x;
    crs::FakeGraph *pg = c.gpu.graph("hud");
    bool whole = false;
    if (pg)
        for (const auto &kv : pg->shown.buffers)
            whole = whole || (kv.second.f == crs::TexFormat::R8 && kv.second.px == f);
    ok("no scaler (hud_scale_down 0), 135 %: held to 100 % (800x353 at 0,63), the frame whole, no corners or patch",
       pl && pl->params.rect.x == 0 && pl->params.rect.y == 63 && pl->params.rect.w == 800 && pl->params.rect.h == 353 && whole &&
           plain.status("decode").hud_width_pct == 135);
}

/* Only what the renderer needs goes over its socket. */
static void only_what_is_needed()
{
    const std::vector<uint8_t> f1 = atlas(1), f2 = atlas(2);
    Rig r(false, 6000);
    r.repeats = true;
    r.pump(1500, &f1);
    bool drawn = false;
    r.drawing("map", f1, &drawn);
    ok("repeats: the first frame whole, shown", r.st().state == "showing" && drawn);
    const unsigned long long first = r.received();
    bool steady = true;
    for (int s = 0; s < 10; s++) {
        r.pump(1000, &f1);
        steady = steady && r.st().state == "showing";
    }
    ok("repeats: a static map's repeated picture keeps it showing past stall_ms, each repeat counted",
       steady && r.map_layer()->params.visible && r.received() >= first + 150);
    int fps = 0;
    std::sscanf(r.st().page.c_str(), "map \xe2\x80\x94 %d frames/s", &fps);
    ok("repeats: the frame rate counts them, a frame each 65 ms", fps == 15 || fps == 16);

    r.feed->set_stream(false, false, Picture::Shown);
    r.pump(100, &f1);
    const unsigned long long hidden = r.received();
    r.pump(3000, &f2);
    ok("hidden: nothing reaches the renderer, frames or repeats", r.received() == hidden && r.st().state == "idle");
    r.feed->set_stream(true, false, Picture::Shown);
    bool first_drawn = false, newest_first = false;
    for (int step = 0; step < 300 && !first_drawn; step++) {
        r.pump(5);
        first_drawn = r.drawing("map", f2, &newest_first);
    }
    ok("shown again: the newest picture is what it draws first", first_drawn && newest_first);
    r.pump(1500, &f2);
    ok("shown again: showing", r.st().state == "showing");

    r.svc.reset();
    r.pump(100, &f2);
    r.start();
    r.pump(3000, &f2);
    r.drawing("map", f2, &drawn);
    ok("the renderer restarted: repeats alone bring the picture back", r.st().state == "showing" && drawn);
}

/* The w x h part at x, y of a 1088x480 atlas, as an atlas of its own. */
static std::vector<uint8_t> crop_of(const std::vector<uint8_t> &f, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    std::vector<uint8_t> c((size_t)w * (h + h / 2));
    for (uint32_t r = 0; r < h; r++)
        std::copy_n(&f[(size_t)(y + r) * 1088 + x], w, &c[(size_t)r * w]);
    for (uint32_t r = 0; r < h / 2; r++)
        for (uint32_t p = 0; p < 2; p++)
            std::copy_n(&f[(size_t)(480 + y / 2 + r) * 1088 + p * 544 + x / 2], w / 2, &c[(size_t)(h + r) * w + p * w / 2]);
    return c;
}

/* A scene with layers on the panel hosting the map (cluster-render
 * PROTOCOL.md "host"): the map's layer moves into it, at the host's rect,
 * its frames the host's part of the picture, the map's own slot released;
 * hidden by the host, no layer at all and nothing sent, idle with the
 * reason; shown again, the newest picture's part; the host gone, the map on
 * its own target again. */
static void hosting()
{
    char tmpl[] = "/tmp/woc-hosting-XXXXXX";
    const std::string root = mkdtemp(tmpl) ? tmpl : "/nonexistent";
    const std::string file = root + "/design.scene";
    std::ofstream(file) << "scene 0x010004f8\nnode plate -\npass main\n";
    chmod(file.c_str(), 0644);
    const std::vector<uint8_t> f = atlas(3), f2 = atlas(4);
    Rig r;
    r.sc.targets[1].scene_files = true;
    r.sc.scene_files.root = root;
    r.sc.scene_files.owner = getuid();
    r.start();
    r.pump(2000, &f);
    ok("hosting: the map shown on its own target", r.st().state == "showing" && r.st().target == "map" && !r.st().hosted);

    cr::Client design("design", r.path);
    r.other = &design;
    cr::SceneOptions so;
    so.target = "panel";
    so.file = file;
    so.sha256 = {crs::sha256_file(file)};
    so.layers = true;
    const uint32_t sc = design.scene(so);
    r.pump(500, &f);
    cr::Host h;
    h.target = "map";
    h.x = 700; h.y = 160; h.w = 560; h.h = 400;
    h.src_x = 264; h.src_y = 40; h.src_w = 560; h.src_h = 400;
    design.host(sc, h);
    r.pump(2500, &f);
    bool with = false;
    const int map_i = r.svc->slots().index("map");
    ok("hosted: the map in the panel's scene over the file, its frames the host's 560x400 part, the map's slot released",
       r.st().state == "showing" && r.st().target == "panel" && r.st().hosted && r.drawing("panel", crop_of(f, 264, 40, 560, 400), &with) &&
           with && r.gpu.graph("panel")->over_file && r.svc->compositor().layers().size() == 1 &&
           r.svc->slots().status(map_i).state == crs::TargetState::Idle);
    ok("hosted, hosted_vpp_scale off (the default): no part asked of the decoder, the part scaled by the worker",
       !r.feed->part().set() && r.feed->scaled_frames() > 0);

    h.shown = false;
    design.host(sc, h);
    r.pump(1000, &f2);
    ok("hidden by the host: no layer, nothing sent, idle with the reason",
       r.svc->compositor().layers().empty() && r.st().state == "idle" && r.st().reason == MapFeed::kHostHidden &&
           r.svc->slots().status(map_i).state == crs::TargetState::Idle);
    h.shown = true;
    design.host(sc, h);
    r.pump(1500);
    ok("shown again: the newest picture's part first", r.st().state == "showing" &&
                                                         r.drawing("panel", crop_of(f2, 264, 40, 560, 400), &with) && with);

    design.drop(sc);
    r.pump(2000, &f2);
    ok("the host gone: the map on its own target again",
       r.st().state == "showing" && r.st().target == "map" && !r.st().hosted && r.drawing("map", f2, &with) && with);

    /* A map smaller than the host's src (map_rect: half size, centred):
     * the host stretches src over its rect, so the map lands as that much
     * of the rect, at its place in src, not over the whole rect. */
    FeedConfig fc;
    fc.socket = r.path;
    fc.video.level_bits = 0;
    fc.rect = MapRect{272, 120, 544, 240};
    r.feed.reset(new MapFeed(fc));
    r.feed->set_stream(true, false, Picture::Shown);
    const uint32_t sc2 = design.scene(so);
    r.pump(500, &f);
    cr::Host whole;
    whole.target = "map";
    whole.x = 700; whole.y = 160; whole.w = 544; whole.h = 240;   /* the map target, half size, no src: all of it */
    design.host(sc2, whole);
    r.pump(2500, &f);
    const crs::Layer *placed = r.map_layer();
    ok("a map smaller than the host's src: the half-size map at its place in the rect (836,220 272x120), "
       "its frame scaled to that",
       r.st().hosted && placed && placed->params.rect.x == 836 && placed->params.rect.y == 220 &&
           placed->params.rect.w == 272 && placed->params.rect.h == 120 &&
           r.drawing("panel", f, &with));
    design.drop(sc2);
    r.pump(500, &f);
    r.other = nullptr;
    const int removed = std::system(("rm -rf " + root).c_str());
    (void)removed;
}

/* hosted_vpp_scale on (FeedConfig::vpp_scale): hosted, the feed asks the
 * decoder for the host's part at the size it is shown at; whole frames that
 * come meanwhile go through the worker, a frame of the part goes as it is,
 * one of another part is dropped; the host's move asks for its new part, and
 * the host gone, for none, the whole frame on the map's own target. */
static void vpp_hosting()
{
    char tmpl[] = "/tmp/woc-vpp-hosting-XXXXXX";
    const std::string root = mkdtemp(tmpl) ? tmpl : "/nonexistent";
    const std::string file = root + "/design.scene";
    std::ofstream(file) << "scene 0x010004f8\nnode plate -\npass main\n";
    chmod(file.c_str(), 0644);
    const std::vector<uint8_t> f = atlas(5), f2 = atlas(6), f3 = atlas(7);
    Rig r;
    r.sc.targets[1].scene_files = true;
    r.sc.scene_files.root = root;
    r.sc.scene_files.owner = getuid();
    r.start();
    FeedConfig fc;
    fc.socket = r.path;
    fc.video.level_bits = 0;
    fc.vpp_scale = true;
    r.feed.reset(new MapFeed(fc));
    r.feed->set_stream(true, false, Picture::Shown);
    r.pump(2000, &f);
    ok("vpp: on its own target, no part asked for", r.st().target == "map" && !r.feed->part().set());

    cr::Client design("design", r.path);
    r.other = &design;
    cr::SceneOptions so;
    so.target = "panel";
    so.file = file;
    so.sha256 = {crs::sha256_file(file)};
    so.layers = true;
    const uint32_t sc = design.scene(so);
    r.pump(500, &f);
    cr::Host h;
    h.target = "map";
    h.x = 700; h.y = 160; h.w = 560; h.h = 400;
    h.src_x = 264; h.src_y = 40; h.src_w = 560; h.src_h = 400;
    design.host(sc, h);
    r.pump(2500, &f);
    bool with = false;
    const OutputPart want{264, 40, 560, 400, 560, 400};
    ok("vpp: hosted, the host's part asked for at the size it is shown at", r.st().hosted && r.feed->part() == want);
    ok("vpp: a whole frame meanwhile scaled by the worker",
       r.drawing("panel", crop_of(f, 264, 40, 560, 400), &with) && with && r.feed->scaled_frames() > 0);

    const std::vector<uint8_t> p2 = crop_of(f2, 264, 40, 560, 400), p3 = crop_of(f3, 264, 40, 560, 400);
    r.part = want;
    r.pump(200, &p2);
    /* a whole frame the worker still had, done and taken (its own thread) */
    for (int i = 0; i < 2000 && r.feed->wake_in_ms(r.now) == MapFeed::kScaleWaitMs; i++) {
        usleep(1000);
        r.pump(5);
    }
    const uint64_t scaled = r.feed->scaled_frames();
    r.pump(1300, &p2);
    ok("vpp: a frame of the part sent as it is, not through the worker",
       r.st().state == "showing" && r.drawing("panel", p2, &with) && with && r.feed->scaled_frames() == scaled);
    OutputPart other = want;
    other.x = 200;
    r.part = other;
    r.pump(1000, &p3);
    ok("vpp: a frame of another part dropped", r.drawing("panel", p2, &with) && with);

    h.src_x = 200;
    design.host(sc, h);
    r.pump(500);
    ok("vpp: the host moved, its new part asked for", r.feed->part() == other);
    r.pump(1000, &p3);
    ok("vpp: the new part's frames shown", r.drawing("panel", p3, &with) && with && r.feed->scaled_frames() == scaled);

    design.drop(sc);
    r.part = OutputPart();
    r.pump(2000, &f2);
    ok("vpp: the host gone, no part asked for, the whole frame on the map's own target",
       !r.feed->part().set() && r.st().target == "map" && !r.st().hosted && r.drawing("map", f2, &with) && with);
    r.other = nullptr;
    const int removed = std::system(("rm -rf " + root).c_str());
    (void)removed;
}

/* Whether the target's scene shows f (a w x h atlas) packed, and in *exact
 * whether it shows an exact atlas. */
static bool shows_packed(Rig &r, const char *target, const std::vector<uint8_t> &f, uint32_t w, uint32_t h, bool *exact)
{
    crs::FakeGraph *g = r.gpu.graph(target);
    *exact = false;
    if (!g || g->drawn().empty())
        return false;
    std::vector<uint8_t> want(crk::block_bytes(w, h));
    crk::pack_blocks(f.data(), w, h, want.data(), 1);
    bool with = false;
    for (const auto &q : g->shown.quads) {
        auto t = q.second.textures.find("u_image");
        if (!q.second.visible || t == q.second.textures.end())
            continue;
        auto b = g->shown.buffers.find(t->second);
        if (b == g->shown.buffers.end())
            continue;
        if (b->second.f == crs::TexFormat::R8)
            *exact = true;
        else if (b->second.px == want)
            with = true;
    }
    return with;
}

/* The map layer's video settings as the renderer took them; null: none. */
static const crs::VideoParams *map_video(Rig &r)
{
    const crs::Layer *l = r.map_layer();
    return l && std::string(l->kind()) == "video" ? &static_cast<const crs::VideoLayer *>(l)->video() : nullptr;
}

/* always_packed: 0 (the default), the layer without always_packed, a still map
 * exact; 1, every update packed, the still map's too, never the exact atlas
 * or back to it; with hosted_vpp_scale, the VPP-made part packed; a host's
 * part not a multiple of 4 high goes as with 0, and shows. */
static void always_packed_map()
{
    const std::vector<uint8_t> f1 = atlas(11), f2 = atlas(12);
    bool exact = false, with = false;
    {
        Rig r;
        r.pump(2000, &f1);
        const crs::VideoParams *vp = map_video(r);
        ok("always_packed 0: the layer without it, pan_blocks as set, the still map exact",
           vp && !vp->always_packed && vp->pan_blocks && r.st().state == "showing" && r.drawing("map", f1, &with) &&
               with && !shows_packed(r, "map", f1, 1088, 480, &exact) && exact);
    }
    {
        Rig r;
        FeedConfig fc;
        fc.socket = r.path;
        fc.video.level_bits = 0;
        fc.always_packed = true;
        r.feed.reset(new MapFeed(fc));
        r.feed->set_stream(true, false, Picture::Shown);
        r.pump(1000, &f1);
        r.pump(1000, &f2);   /* one change, then a still map */
        crs::FakeGraph *g = r.gpu.graph("map");
        const uint64_t still0 = g ? g->flushed_bytes : 0;
        r.pump(2000, &f2);
        const crs::VideoParams *vp = map_video(r);
        const std::string s = r.map_layer() ? r.map_layer()->stats() : std::string();
        ok("always_packed 1: the layer with it", vp && vp->always_packed && vp->pan_blocks);
        ok("always_packed 1: a still map sends nothing", g && g->flushed_bytes == still0);
        ok("always_packed 1: the still map packed, the exact atlas never shown",
           r.st().state == "showing" && shows_packed(r, "map", f2, 1088, 480, &exact) && !exact);
        ok("always_packed 1: every write packed, none exact or back to exact",
           s.find(" 0 whole, 0 partial, 0 rects, ") != std::string::npos && s.find("rects, 0 packed") == std::string::npos &&
               s.find(" 0 back to exact") != std::string::npos);
    }
    char tmpl[] = "/tmp/woc-packed-XXXXXX";
    const std::string root = mkdtemp(tmpl) ? tmpl : "/nonexistent";
    const std::string file = root + "/design.scene";
    std::ofstream(file) << "scene 0x010004f8\nnode plate -\npass main\n";
    chmod(file.c_str(), 0644);
    {
        Rig r;
        r.sc.targets[1].scene_files = true;
        r.sc.scene_files.root = root;
        r.sc.scene_files.owner = getuid();
        r.start();
        FeedConfig fc;
        fc.socket = r.path;
        fc.video.level_bits = 0;
        fc.always_packed = true;
        fc.vpp_scale = true;
        r.feed.reset(new MapFeed(fc));
        r.feed->set_stream(true, false, Picture::Shown);
        r.pump(1000, &f1);
        cr::Client design("design", r.path);
        r.other = &design;
        cr::SceneOptions so;
        so.target = "panel";
        so.file = file;
        so.sha256 = {crs::sha256_file(file)};
        so.layers = true;
        const uint32_t sc = design.scene(so);
        r.pump(500, &f1);
        cr::Host h;
        h.target = "map";
        h.x = 700; h.y = 160; h.w = 560; h.h = 400;
        h.src_x = 264; h.src_y = 40; h.src_w = 560; h.src_h = 400;
        design.host(sc, h);
        r.pump(1500, &f1);
        r.part = OutputPart{264, 40, 560, 400, 560, 400};
        const std::vector<uint8_t> p2 = crop_of(f2, 264, 40, 560, 400);
        r.pump(1500, &p2);
        const crs::VideoParams *vp = map_video(r);
        ok("always_packed with hosted_vpp_scale: the VPP-made part packed, never exact",
           vp && vp->always_packed && r.st().hosted && shows_packed(r, "panel", p2, 560, 400, &exact) && !exact);

        /* The host moves the map: a new layer, its first write the whole
         * packed part once, then nothing while the part stands still. */
        crs::FakeGraph *pg = r.gpu.graph("panel");
        const uint64_t moved0 = pg ? pg->flushed_bytes : 0;
        h.src_x = 200;
        design.host(sc, h);
        r.part = OutputPart{200, 40, 560, 400, 560, 400};
        const std::vector<uint8_t> p3 = crop_of(f2, 200, 40, 560, 400);
        r.pump(1500, &p3);
        const uint64_t moved = pg ? pg->flushed_bytes - moved0 : 0;
        char name[200];
        std::snprintf(name, sizeof name, "always_packed: the host moved, the new part's whole packed frame once (%llu B), "
                      "then nothing", (unsigned long long)moved);
        ok(name, moved == crk::block_bytes(560, 400) && shows_packed(r, "panel", p3, 560, 400, &exact) && !exact);
        h.src_x = 264;

        h.h = 430;
        h.src_h = 430;
        design.host(sc, h);
        r.part = OutputPart();
        r.pump(2000, &f1);
        vp = map_video(r);
        ok("always_packed: a host's part 430 high (not a multiple of 4) as with 0, and shown",
           vp && !vp->always_packed && r.st().state == "showing" && r.st().hosted &&
               r.drawing("panel", crop_of(f1, 264, 40, 560, 430), &with) && with);
        design.drop(sc);
        r.pump(500, &f1);
        r.other = nullptr;
    }
    const int removed = std::system(("rm -rf " + root).c_str());
    (void)removed;
}

/* A cluster-render from before always_packed: its hello without it (with
 * packed, a newer one's, with it); every JSON message a client sends on the
 * current connection kept. */
struct OldRenderer {
    std::string path = "/tmp/woc-old-render-" + std::to_string(getpid()) + ".sock";
    int lfd = -1, cfd = -1;
    std::string in;
    std::vector<std::string> ops;
    bool packed = false;

    OldRenderer()
    {
        unlink(path.c_str());
        lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
        if (bind(lfd, (const sockaddr *)&a, sizeof a) != 0 || listen(lfd, 4) != 0)
            std::printf("old renderer: %s not listening\n", path.c_str());
    }
    ~OldRenderer()
    {
        if (cfd >= 0)
            close(cfd);
        close(lfd);
        unlink(path.c_str());
    }
    void poll()
    {
        if (cfd < 0)
            cfd = accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK);
        if (cfd < 0)
            return;
        char buf[65536];
        ssize_t n;
        while ((n = read(cfd, buf, sizeof buf)) > 0)
            in.append(buf, (size_t)n);
        size_t off = 0;
        for (;;) {
            size_t used = 0, len = 0;
            uint8_t type = 0;
            const char *body = nullptr;
            bool bad = false;
            if (!cr::next_message(in.data() + off, in.size() - off, &used, &type, &body, &len, &bad))
                break;
            off += used;
            if (type != cr::kJson)
                continue;
            ops.emplace_back(body, len);
            cr::Json j;
            if (cr::parse_json(ops.back(), &j) && j["op"] == "hello") {
                std::vector<std::pair<std::string, std::string>> kv = {
                    {"ev", cr::json_quote("hello")}, {"proto", "1"}, {"backend", cr::json_quote("id7")},
                    {"targets", cr::json_quote("map:1088x480,panel:1920x720")}, {"repeat", "1"}};
                if (packed)
                    kv.push_back({"always_packed", "1"});
                const std::string h = cr::encode(cr::kJson, cr::json_object(kv));
                if (write(cfd, h.data(), h.size()) != (ssize_t)h.size())
                    std::printf("old renderer: hello not written\n");
            }
        }
        in.erase(0, off);
    }
    /* the last layer op */
    std::string layer_op() const
    {
        for (auto o = ops.rbegin(); o != ops.rend(); ++o)
            if (o->find("\"layer\"") != std::string::npos && o->find("\"kind\"") != std::string::npos)
                return *o;
        return std::string();
    }
    /* The connection ends (the service restarted, as an upgrade does). */
    void hang_up()
    {
        close(cfd);
        cfd = -1;
        in.clear();
        ops.clear();
    }
};

/* Compatibility: the map's layer to a cluster-render whose hello does not
 * take always_packed carries no always_packed field, set or not (with it
 * set, logged once, the map as pan_blocks decides); and by default the
 * field is never sent, so a cluster-render of any age gets the layer it
 * always did. */
static void old_renderer()
{
    for (int packed = 0; packed < 2; packed++) {
        OldRenderer old;
        FeedConfig fc;
        fc.socket = old.path;
        fc.always_packed = packed == 1;
        MapFeed feed(fc);
        feed.set_stream(true, false, Picture::Shown);
        int64_t now = 100000;
        for (int i = 0; i < 400 && old.layer_op().empty(); i++) {
            now += 5;
            old.poll();
            feed.poll(now);
            old.poll();
        }
        const std::string op = old.layer_op();
        ok(packed ? "an older cluster-render: always_packed 1 sends the layer without always_packed"
                  : "always_packed 0: the layer without always_packed",
           !op.empty() && op.find("always_packed") == std::string::npos && op.find("pan_blocks") != std::string::npos);
    }
}

/* always_packed follows each connection's hello: cluster-render upgraded
 * (or downgraded) under a running feed gets the map's layer made again for
 * what it takes, with the one log line when it no longer takes it. */
static void renderer_changed()
{
    for (int up = 0; up < 2; up++) {
        OldRenderer r;
        r.packed = !up;
        FeedConfig fc;
        fc.socket = r.path;
        fc.always_packed = true;
        MapFeed feed(fc);
        feed.set_stream(true, false, Picture::Shown);
        int64_t now = 100000;
        auto run = [&] {
            for (int i = 0; i < 400; i++) {
                now += 5;
                r.poll();
                feed.poll(now);
                r.poll();
            }
        };
        run();
        const bool before = r.layer_op().find("always_packed") != std::string::npos;
        uint64_t seq = 0;
        std::vector<std::string> lines;
        cld::log_lines_since(&seq, &lines);
        lines.clear();
        r.packed = up;
        r.hang_up();
        run();
        const std::string op = r.layer_op();
        cld::log_lines_since(&seq, &lines);
        int logged = 0;
        for (const std::string &line : lines)
            logged += line.find("render: always_packed:") != std::string::npos;
        if (up)
            ok("always_packed: an older cluster-render upgraded under the feed: the layer with always_packed after the "
               "reconnect",
               !before && op.find("\"always_packed\"") != std::string::npos && logged == 0);
        else
            ok("always_packed: cluster-render downgraded under the feed: the layer without always_packed after the "
               "reconnect, logged once",
               before && !op.empty() && op.find("always_packed") == std::string::npos && logged == 1);
    }
}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, nullptr, _IONBF, 0);
    hosting();
    vpp_hosting();
    always_packed_map();
    old_renderer();
    renderer_changed();
    Rig r;
    r.pump(100);
    const crs::Layer *l = r.map_layer();
    ok("connected: one contending content layer on the map target, covering the 1088x480 slot",
       l && l->target() == 0 && l->params.contend && l->params.klass == crs::Klass::Content && l->params.rect.x == 0 &&
           l->params.rect.y == 0 && l->params.rect.w == 1088 && l->params.rect.h == 480);
    ok("no frame yet: idle, and the slot is not asked for", r.st().state == "idle" && r.port.requests == 0);

    const std::vector<uint8_t> f1 = atlas(1);
    r.pump(20, &f1);
    ok("the first frame asks for the slot: starting", r.st().state == "starting" && r.port.requests == 1);
    r.pump(1100, &f1);
    crs::FakeGraph *g = r.gpu.graph("map");
    bool drawn = false;
    if (g)
        for (const auto &kv : g->shown.buffers)
            drawn = drawn || (kv.second.f == crs::TexFormat::R8 && kv.second.px == f1);
    ok("granted: showing, with the decoded frame in the slot's scene",
       r.st().state == "showing" && drawn && r.st().page.find("map — ") == 0);
    ok("the status line counts the frames handed over", r.st().page.find("15 frames/s") != std::string::npos);
    const std::string lost = "Stream lost";   /* until the layer stalls */
    ok("showing carries the stream's reason", r.feed->status("decode", lost).state == "showing" &&
                                                  r.feed->status("decode", lost).reason == lost &&
                                                  r.feed->status("decode", lost).stream_reason);

    r.port.steal(r.sc.targets[0]);
    r.pump(20, &f1);
    ok("the stock map takes the slot: starting, with that reason",
       r.st().state == "starting" && r.st().reason == "the stock map holds the slot");
    r.pump(2200, &f1);
    const crs::SlotInfo *ours = r.port.row(0x100, 0x010004f0), *stock = r.port.row(0x100, 0x01000101);
    ok("the map contends: it takes the stock map over and is shown, the stock map parked behind it",
       r.st().state == "showing" && r.port.requests == 2 && r.port.taken_over.size() == 1 &&
           r.port.taken_over[0] == 0x01000101 && ours && ours->map_status == 1 && stock && stock->map_status == 3);

    /* Many frames between two writes: only the newest is drawn. */
    const std::vector<uint8_t> f2 = atlas(2), f3 = atlas(3);
    std::vector<uint8_t> b2 = f2, b3 = f3;
    r.feed->offer(&b2, 2, r.now);
    r.feed->offer(&b3, 3, r.now);
    ok("a frame is taken without a copy: an earlier frame's buffer comes back for each, the last sent kept",
       b2.size() == f2.size() && b3.size() == f3.size() && b2 == f1 && b3 == f1);
    r.pump(200);
    g = r.gpu.graph("map");
    bool newest = false, stale = false;
    for (const auto &kv : g->shown.buffers)
        if (kv.second.f == crs::TexFormat::R8) {
            newest = newest || kv.second.px == f3;
            stale = stale || kv.second.px == f2;
        }
    ok("frames faster than the writes: the newest is drawn", newest && !stale);

    const int releases = r.port.releases;
    r.pump(6100);
    ok("frames stop: idle after stall_ms", r.st().state == "idle");
    const std::string waiting = "Stream lost";
    ok("idle while the stream is live carries the stream's reason", r.feed->status("decode", waiting).reason == waiting);
    r.pump(2000);   /* the fade out, then the linger */
    ok("and the slot is released after the linger",
       r.port.releases > releases && !r.port.row(0x100, 0x010004f0));

    r.svc.reset();
    r.pump(100);
    ok("the renderer gone: starting while it may be starting", r.st().state == "starting" &&
                                                               r.st().reason == "waiting for the cluster renderer");
    r.pump(15000);
    ok("still gone 15 s on: an error the app shows", r.st().state == "error" &&
                                                     r.st().reason == "cluster renderer not reachable");
    r.start();
    r.pump(3000, &f1);
    ok("it comes back: the map layer made again, showing with the next frames",
       r.map_layer() && r.st().state == "showing");

    /* the governor counts evidence seen while writes go */
    r.svc->compositor().governor().activity(r.now);
    r.svc->compositor().governor().evidence(crs::Evidence::LinkStall, r.now);
    r.svc->compositor().governor().evidence(crs::Evidence::LinkStall, r.now);
    r.pump(100, &f1);
    ok("a lowered shared upload cap shows on the status line",
       r.st().page.find("upload cap 5000 KB/s") != std::string::npos);
    ok("the status carries the cap and why",
       r.st().upload_cap_kb == 5000 && r.st().upload_reason.find("lowered from 5500 KB/s") == 0 &&
           r.st().upload_reason.find("heartbeat replies silent") != std::string::npos);
    /* A minute of healthy upload raises it back to the ceiling, which the
     * status line leaves out. */
    for (int s = 0; s < 62; s++) {
        r.svc->compositor().governor().activity(r.now);
        r.pump(1000, &f1);
    }
    ok("back at the ceiling after healthy upload: the cap leaves the status line, its reason stays",
       r.st().upload_cap_kb == 5500 && r.st().page.find("upload cap") == std::string::npos &&
           r.st().upload_reason.find("raised from 5000 KB/s") == 0);
    /* the stream's liveness drives the layer, whatever the frames do */
    const int before = r.port.releases;
    r.feed->set_stream(false, false, Picture::Shown);
    r.pump(2000, &f1);   /* the fade out, then the linger */
    stock = r.port.row(0x100, 0x01000101);
    ok("the stream not live: the layer hidden, idle, and the slot released, the parked stock map shown",
       r.map_layer() && !r.map_layer()->params.visible && r.st().state == "idle" && r.port.releases > before &&
           !r.port.row(0x100, 0x010004f0) && stock && stock->map_status == 1);
    ok("and no stream reason", r.feed->status("decode", waiting).reason.empty());
    r.feed->set_stream(true, false, Picture::Shown);
    r.pump(1200, &f1);
    ok("live again: visible and showing with the frames", r.map_layer()->params.visible && r.st().state == "showing");
    r.feed->set_stream(true, true, Picture::Blank);
    r.pump(100, &f1);
    ok("frozen: hidden, and the status says why",
       !r.map_layer()->params.visible && r.st().state == "frozen" && r.st().reason == "the phone's cluster stream froze");
    r.feed->set_stream(true, false, Picture::Shown);
    r.pump(1200, &f1);
    ok("no longer frozen: shown again", r.map_layer()->params.visible && r.st().state == "showing");

    Rig h;
    h.feed->set_stream(false, false, Picture::None);
    h.svc.reset();
    h.start();
    h.pump(3000, &f1);
    ok("a layer made while the stream is not live is hidden: idle, the slot not asked for",
       h.map_layer() && !h.map_layer()->params.visible && h.st().state == "idle" && h.port.requests == 0);

    /* The stream's own pictures, from a stream start with the stock map up. */
    std::vector<uint8_t> black(1088 * 720, 128);
    std::fill(black.begin(), black.begin() + 1088 * 480, 16);
    Rig b(true, 60000);
    b.port.stock_map(0x100, 0x01000101);
    b.pump(100);
    ok("blank start: live, no frame yet: idle, no reason, the slot not asked for",
       b.st().state == "idle" && b.st().reason.empty() && b.port.requests == 0);
    b.pump(2000, &black, 20);   /* the car's start: about 100 blank frames in 2 s */
    b.pump(60000);              /* then a pause */
    const crs::SlotInfo *stock_b = b.port.row(0x100, 0x01000101);
    ok("blank start: the burst and a minute's pause never ask for the slot; the stock map stays",
       b.port.requests == 0 && !b.port.row(0x100, 0x010004f0) && stock_b && stock_b->map_status == 1 &&
           b.map_layer() && !b.map_layer()->params.visible);
    ok("blank start: idle, and the app told it waits for the phone's map",
       b.st().state == "idle" && b.st().reason == MapFeed::kWaiting && !b.st().stream_reason &&
           b.st().reason == "Waiting for the phone's map");
    ok("blank start: a lost stream's reason comes first",
       b.feed->status("decode", lost).reason == lost && b.feed->status("decode", lost).stream_reason);
    b.pump(5, &f1);   /* one non-blank frame, then nothing */
    b.pump(1500);
    g = b.gpu.graph("map");
    drawn = false;
    if (g)
        for (const auto &kv : g->shown.buffers)
            drawn = drawn || (kv.second.f == crs::TexFormat::R8 && kv.second.px == f1);
    stock_b = b.port.row(0x100, 0x01000101);
    ok("first picture: the slot asked for once, the stock map taken over, that frame shown",
       b.st().state == "showing" && drawn && b.port.requests == 1 && b.port.taken_over.size() == 1 && stock_b &&
           stock_b->map_status == 3);
    for (int spell = 0; spell < 6; spell++) {
        b.pump(9000, &black, 1000);   /* a blank frame a second for 9 s */
        b.pump(5, &f1);
    }
    ok("blank spells of 9 s between pictures: showing throughout, the slot not asked for again",
       b.st().state == "showing" && b.port.requests == 1 && b.map_layer()->params.visible);
    const int rel_b = b.port.releases;
    b.pump(5, &black);   /* one blank frame, then nothing */
    b.pump(9900);
    ok("one blank frame, then nothing: the map stays until blank_ms",
       b.st().state == "showing" && b.port.releases == rel_b);
    b.pump(2500);   /* blank_ms, then the fade out and the linger */
    stock_b = b.port.row(0x100, 0x01000101);
    ok("blank for blank_ms: hidden, the slot given back, the stock map shown, the app told it waits for the phone's map",
       !b.map_layer()->params.visible && b.port.releases > rel_b && !b.port.row(0x100, 0x010004f0) && stock_b &&
           stock_b->map_status == 1 && b.st().state == "idle" && b.st().reason == MapFeed::kWaiting);
    b.pump(5, &f1);
    b.pump(1500);
    ok("the next picture takes the slot again", b.st().state == "showing" && b.port.requests == 2);

    Rig fz(true, 60000);
    fz.port.stock_map(0x100, 0x01000101);
    fz.pump(31000, &black, 65);   /* blank at 15 fps from the start */
    ok("blank at full rate from the start: frozen after 30 s, the slot never asked for",
       fz.st().state == "frozen" && fz.port.requests == 0 && !fz.map_layer()->params.visible);

    Rig lf(true, 60000);
    lf.live_stall_ms = 6000;
    lf.port.stock_map(0x100, 0x01000101);
    lf.pump(100);
    bool every = true;
    for (int k = 0; k < 3; k++) {
        lf.pump(5, &f1);   /* one frame, then nothing for 10 s */
        lf.pump(1500);
        every = every && lf.st().state == "showing" && lf.map_layer()->params.visible;
        lf.pump(8500);
        every = every && lf.st().state == "idle";
    }
    ok("live through a recent frame: a static map's frame every 10 s is shown each time it makes the stream live",
       every && lf.port.requests == 3);

    Rig nl(true, 60000);
    nl.port.stock_map(0x100, 0x01000101);
    nl.pump(1500, &f1);
    ok("reconnect: the first stream shows", nl.st().state == "showing" && nl.port.requests == 1);
    uint64_t seq = 0;
    std::vector<std::string> lines;
    cld::log_lines_since(&seq, &lines);
    lines.clear();
    nl.live = false;   /* the connection ends */
    nl.pump(2000);
    nl.pump(5, &f1);   /* the ended stream's last frame, still on its way */
    nl.pump(200, &black, 20);
    nl.pump(3500);
    nl.live = true;   /* the next connection */
    nl.pump(1500);
    const crs::SlotInfo *stock_nl = nl.port.row(0x100, 0x01000101);
    ok("reconnect: a frame decoded after the end, the next stream live seconds later: idle, hidden, the stock map shown",
       nl.st().state == "idle" && nl.st().reason.empty() && !nl.map_layer()->params.visible &&
           nl.port.requests == 1 && stock_nl && stock_nl->map_status == 1);
    cld::log_lines_since(&seq, &lines);
    int not_live = 0;
    for (const std::string &line : lines)
        not_live += line.find("cluster stream is not live") != std::string::npos;
    ok("reconnect: the stream ending is logged once, whatever frames still come", not_live == 1);
    nl.live = false;
    nl.pump(2000);
    nl.pump(5, &f1);
    nl.pump(500);
    nl.live = true;   /* a new stream's first frame, decoded before its connection is read */
    nl.pump(1500);
    ok("reconnect: a frame decoded just before the stream is live takes the slot",
       nl.st().state == "showing" && nl.port.requests == 2);

    StatusLog sl;
    FeedStatus a;
    a.state = "showing";
    a.target = "map";
    a.page = "map — 15 frames/s";
    int64_t t = 1000000;
    ok("status log: the first status", sl.due(a, t));
    a.page = "map — 14 frames/s";
    a.upload_cap_kb = 5500;
    a.upload_reason = "slow flushes";
    ok("status log: not for the page or the upload cap", !sl.due(a, t));
    a.reason = "Stream lost";
    a.stream_reason = true;
    ok("status log: the stream's reason", sl.due(a, t));
    a.reason.clear();
    a.stream_reason = false;
    ok("status log: the stream whole again within 10 s waits", !sl.due(a, t + 2000));
    a.reason = "Stream lost";
    a.stream_reason = true;
    ok("status log: so does its next loss; the logged reason is the same, nothing to log",
       !sl.due(a, t + 4000) && !sl.due(a, t + 10000));
    a.reason.clear();
    a.stream_reason = false;
    ok("status log: a change still standing 10 s after the last such line is logged",
       !sl.due(a, t + 9000) && sl.due(a, t + 10500));
    a.state = "idle";
    a.reason = "Stream lost";
    a.stream_reason = true;
    ok("status log: a state change is logged at once, with its reason", sl.due(a, t + 11000));
    a.state = "starting";
    a.reason = "the stock map holds the slot";
    a.stream_reason = false;
    ok("status log: a state and reason", sl.due(a, t + 11100));
    ok("status log: the same again is not", !sl.due(a, t + 11200));
    a.reason = "another app holds the slot";
    ok("status log: a reason of the feed's own alone, at once", sl.due(a, t + 11300));
    a.target = "panel";
    ok("status log: a target alone", sl.due(a, t + 11400));

    guide_hold();
    show_off();
    near_turns();
    hud_zoom();
    hud_wide();
    only_what_is_needed();
    {   /* (its own scope: the names of the rest of main) */
    /* ---- MapFeeds: the cluster, and the HUD's copy when it is on ---- */
    Rig b(false, 6000, false, true, true);
    b.feed.reset();   /* the rig's single feed is not used here */
    b.sc.targets[2].share = true;
    b.start();
    FeedConfig bfc;
    bfc.socket = b.path;
    bfc.video.level_bits = 0;
    bfc.hud_zoom_pct = 100;   /* the whole frame on the HUD: this checks the scaling, hud_zoom() the zoom */
    bfc.hud_clear = false;   /* and hud_wide() the speed patch */
    MapFeeds feeds(bfc, true, false, 10);
    feeds.set_stream(true, false, Picture::Shown);
    std::vector<uint8_t> bspare, boffered;
    bool hud_asked = false;
    /* vary: each frame differs from the one before (a moving map); a frame
     * the same as the last offered goes as a repeat, as the producer hands
     * it over */
    const auto bpump = [&](int64_t ms, const std::vector<uint8_t> *frame, bool vary = false) {
        for (int64_t t = 0; t < ms; t += 5) {
            b.now += 5;
            if (frame && t % 65 == 0) {
                bspare.assign(frame->begin(), frame->end());
                if (vary)
                    bspare[(size_t)(t / 65) % 1088] ^= 0x80;
                if (bspare == boffered) {
                    feeds.repeat((uint64_t)b.now, b.now);
                } else {
                    boffered = bspare;
                    feeds.offer(&bspare, (uint64_t)b.now, b.now);
                }
            }
            for (int round = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                const size_t first = b.svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                b.port.tick(b.now);
                b.svc->after_poll(fds, first, b.now);
                const bool pending = (feeds.cluster() && feeds.cluster()->wants_write()) || (feeds.hud_feed() && feeds.hud_feed()->wants_write());
                const bool scaling = hud_scaling(feeds, b.now);
                if (scaling)
                    usleep(1000);
                feeds.poll(b.now);
                hud_asked = hud_asked || b.svc->slots().requested(2);
                if (round > 0 && n <= 0 && !pending && !scaling)
                    break;
            }
        }
    };
    const auto layer_on = [&](int target) -> const crs::Layer * {
        for (const crs::Layer *x : b.svc->compositor().layers())
            if (x->target() == target)
                return x;
        return nullptr;
    };
    const auto shown_r8 = [&](const char *target, const std::vector<uint8_t> &px) {
        crs::FakeGraph *g = b.gpu.graph(target);
        bool on = false;
        if (g)
            for (const auto &kv : g->shown.buffers)
                on = on || (kv.second.f == crs::TexFormat::R8 && kv.second.px == px);
        return on;
    };
    bpump(1500, &f1);
    ok("HUD off (the default): the map on the cluster, no HUD feed, layer or request",
       layer_on(0) && shown_r8("map", f1) && !feeds.hud_feed() && !layer_on(2) && !hud_asked &&
           feeds.status("decode").state == "showing" && feeds.status("decode").hud_state == "off" && !feeds.status("decode").hud);
    ok("HUD off: no hud scene ever published", !b.gpu.graph("hud"));

    feeds.set_hud(true);
    bpump(1500, &f1);
    /* the HUD shows the map smaller: its frame is scaled on the head unit */
    AtlasScaler ref;
    std::vector<uint8_t> f1_hud;
    ref.configure(1088, 480, 452, 200, 2);   /* FeedConfig::scaled_level_bits */
    ref.set_corner_radius(200 / 12);         /* FeedConfig::hud_round_corners */
    ref.set_black_background(true);          /* FeedConfig::hud_black_background */
    ref.scale(f1.data(), &f1_hud);
    const crs::Layer *bh = layer_on(2);
    ok("HUD on: a layer on each target, the frame on the cluster whole and on the HUD scaled to 452x200, showing",
       layer_on(0) && bh && shown_r8("map", f1) && shown_r8("hud", f1_hud) && feeds.status("decode").state == "showing" &&
           feeds.status("decode").hud_state == "showing" && feeds.status("decode").target == "map");
    ok("HUD on: its map fitted inside its 800x480 scene",
       bh && bh->params.rect.w <= 800 && bh->params.rect.h <= 480 && bh->params.rect.x + bh->params.rect.w <= 800);
    {
        const uint64_t before = feeds.hud_feed()->scaled_frames();
        bpump(2000, &f2, true);
        const uint64_t scaled = feeds.hud_feed()->scaled_frames() - before;
        char name[160];
        std::snprintf(name, sizeof name, "HUD on: at most 10 frames a second while the cluster takes 15 (%llu in 2 s)",
                      (unsigned long long)scaled);
        ok(name, scaled >= 15 && scaled <= 21);
        /* one more frame right after a HUD frame went, then none: it is not
         * due yet, and still reaches the HUD */
        const auto hud_px = [&]() {
            crs::FakeGraph *g = b.gpu.graph("hud");
            std::vector<uint8_t> px;
            if (g)
                for (const auto &kv : g->shown.buffers)
                    if (kv.second.f == crs::TexFormat::R8)
                        px = kv.second.px;
            return px;
        };
        const std::vector<uint8_t> before_last = hud_px(), f3 = atlas(3);
        bspare.assign(f3.begin(), f3.end());
        boffered = f3;
        feeds.offer(&bspare, (uint64_t)b.now, b.now);
        bpump(600, nullptr);
        ok("HUD on: the newest frame, offered between two HUD frames, reaches the HUD once the stream stops",
           shown_r8("map", f3) && !before_last.empty() && hud_px() != before_last);
        const uint64_t still_from = feeds.hud_feed()->scaled_frames();
        bpump(8000, &f3);   /* past the layers' stall_ms (6 s) */
        const crs::Layer *hs = layer_on(2);
        std::string why;
        ok("HUD on, a still map: the same frame not scaled again, the HUD layer kept fed past its stall time",
           feeds.hud_feed()->scaled_frames() == still_from && shown_r8("map", f3) && hs &&
               b.svc->compositor().state(hs->id(), &why) == crs::LayerState::Shown &&
               feeds.status("decode").hud_state == "showing");
    }


    /* ---- the HUD map's size ---- */
    const crs::Layer *hl = layer_on(2);
    const unsigned default_w = hl ? hl->params.rect.w : 0;
    ok("the default HUD map is the kit's fitted box, narrower than the HUD", hl && default_w > 0 && default_w < 800);
    feeds.set_hud_width(85);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("85 %: 680x300, the picture's shape kept, centred",
       hl && hl->params.rect.w == 680 && hl->params.rect.h == 300 && hl->params.rect.x == 60 && hl->params.rect.y == 90 &&
           feeds.status("decode").hud_width_pct == 85);
    feeds.set_hud_width(200);
    bpump(300, &f2);
    ok("200 %: held to the widest, 135 %", feeds.status("decode").hud_width_pct == 135);
    feeds.set_hud_width(40);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("40 %: resized live, centred", hl && hl->params.rect.w == 320 && hl->params.rect.h == 140 &&
                                      hl->params.rect.x == 240 && hl->params.rect.y == 169);
    /* the frame goes at the size it is shown: 320x140, 67,200 B instead of 783,360 */
    crs::FakeGraph *g40 = b.gpu.graph("hud");
    bool small = false;
    if (g40)
        for (const auto &kv : g40->shown.buffers)
            small = small || (kv.second.f == crs::TexFormat::R8 && kv.second.px.size() == 320u * 210);
    ok("40 %: the HUD's texture is the scaled frame, 320x140 (67,200 B)", small);
    ok("the size leaves the cluster's map alone", layer_on(0) && layer_on(0)->params.rect.w == 1088);
    feeds.set_hud(false);
    bpump(2500, &f2);
    ok("HUD off again: its layer gone, the cluster's map as it was", !layer_on(2) && layer_on(0) && shown_r8("map", f2));
    feeds.set_hud(true);
    bpump(2500, &f2);
    ok("a HUD feed made later takes the size", layer_on(2) && layer_on(2)->params.rect.w == 320);
    feeds.set_hud_width(0);
    bpump(300, &f2);
    ok("0: back to the default box", layer_on(2) && layer_on(2)->params.rect.w == default_w);

    /* ---- where the HUD map is ---- */
    feeds.set_hud_width(40);
    bpump(300, &f2);
    const crs::Layer *at40 = layer_on(2);
    const uint32_t ref40 = at40 ? at40->id() : 0;
    feeds.set_hud_offset(100, -50);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("moved: 100 right, 50 up from the centre (240,169 -> 340,119)", hl && hl->params.rect.x == 340 && hl->params.rect.y == 119 &&
                                                                          hl->params.rect.w == 320 && hl->params.rect.h == 140);
    ok("moved: the same layer, only its place changed (no new texture)", hl && hl->id() == ref40);
    ok("moved: the status carries the offset", feeds.status("decode").hud_offset_x == 100 && feeds.status("decode").hud_offset_y == -50);
    feeds.set_hud_offset(800, 800);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("moved too far: held inside the HUD (its bottom right corner)",
       hl && hl->params.rect.x + hl->params.rect.w == 800 && hl->params.rect.y + hl->params.rect.h >= 479 && hl->params.rect.y + hl->params.rect.h <= 480);
    ok("moved too far: the offset reported as far as it goes (240,170 for 320x140)",
       feeds.status("decode").hud_offset_x == 240 && feeds.status("decode").hud_offset_y == 170);
    feeds.set_hud_offset(-800, -800);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("moved too far the other way: the top left corner", hl && hl->params.rect.x == 0 && hl->params.rect.y == 0);
    feeds.set_hud_offset(-30, 20);
    feeds.set_hud_width(60);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("resized while moved: the new size, still moved (60 %: 480x211 centred at 160,134, so 130,154)",
       hl && hl->params.rect.w == 480 && hl->params.rect.x == 130 && hl->params.rect.y == 154);
    feeds.set_hud_offset(200, 0);
    feeds.set_hud_width(85);
    bpump(300, &f2);
    feeds.set_hud_width(40);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("moved, made larger (held to 60 px) and smaller again: back where it was asked (240+200 = 440)",
       hl && hl->params.rect.x == 440 && feeds.status("decode").hud_offset_x == 200);
    feeds.set_hud_offset(-30, 20);
    feeds.set_hud_width(60);
    bpump(300, &f2);
    ok("the offset leaves the cluster's map alone", layer_on(0) && layer_on(0)->params.rect.x == 0 && layer_on(0)->params.rect.y == 0);
    feeds.set_hud_offset(0, 0);
    feeds.set_hud_width(0);

    /* ---- the HUD map over BMW's pop-ups ---- */
    bpump(300, &f2);
    ok("by default the HUD map gives way (its layer's yield 1)",
       layer_on(2) && layer_on(2)->params.yield && !feeds.status("decode").hud_keep_shown);
    b.port.stock_map(0x322, 0x01000251);   /* the media source list on the HUD */
    bpump(300, &f2);
    ok("by default: a list on the HUD releases the HUD map",
       b.svc->slots().status(2).state == crs::TargetState::Waiting);
    const uint32_t yield_ref = layer_on(2) ? layer_on(2)->id() : 0;
    feeds.set_hud_keep_shown(true);
    bpump(2500, &f2);
    ok("kept shown: the same layer with yield 0, shown over the list, the status says so",
       layer_on(2) && layer_on(2)->id() == yield_ref && !layer_on(2)->params.yield &&
           b.svc->slots().status(2).state == crs::TargetState::Shown && feeds.status("decode").hud_keep_shown);
    ok("kept shown: the cluster's map layer untouched", layer_on(0) && layer_on(0)->params.yield);
    feeds.set_hud(false);
    bpump(2500, &f2);
    feeds.set_hud(true);
    bpump(2500, &f2);
    ok("a HUD feed made later keeps it shown", layer_on(2) && !layer_on(2)->params.yield &&
                                                 b.svc->slots().status(2).state == crs::TargetState::Shown);
    feeds.set_hud_keep_shown(false);
    bpump(300, &f2);
    ok("giving way again: released for the list", layer_on(2) && layer_on(2)->params.yield &&
                                                     b.svc->slots().status(2).state == crs::TargetState::Waiting);
    b.port.stock_unmap(0x322, 0x01000251);
    bpump(2500, &f2);
    ok("the list gone: the HUD map shown again", b.svc->slots().status(2).state == crs::TargetState::Shown);

    /* ---- where the map shows: the cluster, the HUD, both, neither ---- */
    {
        const std::vector<uint8_t> f4 = atlas(4), f5 = atlas(5);
        feeds.set_cluster(false);
        bool map_asked = false;
        for (int k = 0; k < 100; k++) {   /* the map's linger (1.5 s) and fade first */
            bpump(50, &f4, true);
            map_asked = map_asked || (k > 50 && b.svc->slots().requested(0));
        }
        FeedStatus st = feeds.status("decode");
        ok("the HUD alone: no cluster feed or map layer, the map target let go, the HUD showing, status off/on",
           !feeds.cluster() && !layer_on(0) && !map_asked && layer_on(2) && feeds.wants_frames() && st.state == "off" &&
               !st.cluster && st.hud && st.hud_state == "showing");
        const uint64_t alone_from = feeds.hud_feed()->scaled_frames();
        bpump(2000, &f4, true);
        const uint64_t alone = feeds.hud_feed()->scaled_frames() - alone_from;
        char name[160];
        std::snprintf(name, sizeof name, "the HUD alone: its frames taken without a copy, at most 10 a second (%llu in 2 s)",
                      (unsigned long long)alone);
        ok(name, alone >= 15 && alone <= 21);
        feeds.set_hud(false);
        bpump(2500, &f4, true);
        st = feeds.status("decode");
        ok("neither: no feed, no layer, no frames wanted, status off/off",
           !feeds.cluster() && !feeds.hud_feed() && !layer_on(0) && !layer_on(2) && !feeds.wants_frames() &&
               st.state == "off" && st.hud_state == "off" && !st.cluster && !st.hud);
        bspare.assign(f4.begin(), f4.end());
        feeds.offer(&bspare, 1, b.now);
        ok("neither: an offered frame goes nowhere", !layer_on(0) && !layer_on(2));
        feeds.set_cluster(true);
        bpump(1500, &f5);
        ok("the cluster on again: its layer back, the next frame shown, the HUD still off",
           layer_on(0) && shown_r8("map", f5) && !layer_on(2) && feeds.status("decode").state == "showing" &&
               feeds.wants_frames());
        feeds.set_hud(true);
        bpump(1500, &f5);
        ok("both again: the HUD's layer back beside the cluster's", layer_on(0) && layer_on(2) &&
                                                                     feeds.status("decode").hud_state == "showing");
        feeds.set_cluster(false);
        feeds.set_cluster(true);
        bpump(1500, &f5);
        ok("the cluster off and on with the HUD on: both shown, the HUD kept", layer_on(0) && layer_on(2) && shown_r8("map", f5));
    }

    /* ---- repeats: a still map costs nothing on either target ---- */
    {
        /* frames whole, and every frame the renderer counted (whole, repeats
         * and the frames they superseded) */
        const auto counted = [&](int target, unsigned long long *whole) {
            unsigned long long n[4] = {};
            if (const crs::Layer *l = layer_on(target))
                std::sscanf(l->stats().c_str(), "%llu sent, %llu unchanged, %llu within threshold, %llu superseded",
                            &n[0], &n[1], &n[2], &n[3]);
            *whole = n[0];
            return n[0] + n[1] + n[2] + n[3];
        };
        const auto shown = [&](int target) {
            const crs::Layer *l = layer_on(target);
            std::string why;
            return l && b.svc->compositor().state(l->id(), &why) == crs::LayerState::Shown;
        };
        const auto hud_r8 = [&]() {
            std::vector<uint8_t> px;
            if (crs::FakeGraph *g = b.gpu.graph("hud"))
                for (const auto &kv : g->shown.buffers)
                    if (kv.second.f == crs::TexFormat::R8)
                        px = kv.second.px;
            return px;
        };
        const std::vector<uint8_t> f6 = atlas(6), f7 = atlas(7);
        bpump(1500, &f6);
        unsigned long long cw0, hw0;
        const unsigned long long c0 = counted(0, &cw0), h0 = counted(2, &hw0);
        const uint64_t s0 = feeds.hud_feed()->scaled_frames();
        const std::vector<uint8_t> still_hud = hud_r8();
        bpump(8000, &f6);   /* repeats only, past the layers' stall_ms (6 s) */
        unsigned long long cw1, hw1;
        const unsigned long long c1 = counted(0, &cw1), h1 = counted(2, &hw1);
        const uint64_t s1 = feeds.hud_feed()->scaled_frames();
        char name[200];
        std::snprintf(name, sizeof name,
                      "both, a still map: repeats keep both layers shown past stall_ms, no frame whole and none scaled "
                      "(cluster %llu, HUD %llu counted in 8 s)",
                      c1 - c0, h1 - h0);
        ok(name, cw1 == cw0 && hw1 == hw0 && s1 == s0 && c1 - c0 >= 100 && h1 - h0 >= 100 && shown(0) && shown(2) &&
                     shown_r8("map", f6) && hud_r8() == still_hud && feeds.status("decode").state == "showing" &&
                     feeds.status("decode").hud_state == "showing");
        bpump(2000, &f7, true);   /* moving */
        unsigned long long cw2, hw2;
        const unsigned long long c2 = counted(0, &cw2);
        counted(2, &hw2);
        const uint64_t s2 = feeds.hud_feed()->scaled_frames();
        std::snprintf(name, sizeof name,
                      "still, then moving: the HUD scales at most 10 a second (%llu in 2 s) and is written (%llu), "
                      "the cluster keeps its rate (%llu frames in 2 s, %llu in 2 s still)",
                      (unsigned long long)(s2 - s1), hw2 - hw1, c2 - c1, (c1 - c0) / 4);
        ok(name, s2 - s1 >= 15 && s2 - s1 <= 21 && hw2 - hw1 >= 5 && hud_r8() != still_hud && cw2 - cw1 >= 25 &&
                     c2 - c1 + 3 >= (c1 - c0) / 4 && shown(0) && shown(2));

        /* the HUD alone, from both with a still map: its picture kept, and
         * repeats of its last scaled frame */
        bpump(1500, &f6);
        feeds.set_cluster(false);
        bpump(2500, &f6);
        unsigned long long hw3;
        const unsigned long long h3 = counted(2, &hw3);
        const uint64_t s3 = feeds.hud_feed()->scaled_frames();
        bpump(8000, &f6);
        unsigned long long hw4;
        const unsigned long long h4 = counted(2, &hw4);
        ok("the HUD alone, a still map: repeats keep it shown past stall_ms, no frame whole and none scaled",
           !layer_on(0) && hw4 == hw3 && h4 - h3 >= 100 && feeds.hud_feed()->scaled_frames() == s3 && shown(2) &&
               feeds.status("decode").hud_state == "showing");
        bpump(2000, &f7, true);
        const uint64_t s5 = feeds.hud_feed()->scaled_frames();
        ok("the HUD alone, then moving: scaled at most 10 a second", s5 - s3 >= 15 && s5 - s3 <= 21 && shown(2));

        /* a feed made beside the other starts from its picture: no new one
         * comes, the map is still */
        bpump(1500, &f6);
        feeds.set_cluster(true);
        bpump(1500, &f6);
        ok("the cluster on beside the HUD, a still map: the picture shown from the HUD's at once",
           shown(0) && shown_r8("map", f6) && shown(2));
        feeds.set_hud(false);
        bpump(1500, &f6);
        feeds.set_hud(true);
        feeds.set_cluster(false);
        bpump(2500, &f6);
        ok("the cluster's map moved to the HUD, a still map: the HUD shows it, scaled once",
           !layer_on(0) && shown(2) && feeds.hud_feed()->scaled_frames() == 1 && !hud_r8().empty());
        feeds.set_cluster(true);
        bpump(1500, &f6);
    }

    feeds.set_stream(false, false, Picture::None);
    bpump(200, &f2);
    ok("the stream not live reaches both feeds: idle", feeds.status("decode").state == "idle" &&
                                                      feeds.status("decode").hud_state == "idle");
    }

    {   /* ---- before the renderer: the HUD's place as saved ---- */
    FeedConfig nfc;
    nfc.socket = "/tmp/woc-feed-no-renderer-" + std::to_string(getpid()) + ".sock";
    nfc.hud_offset_x = 30;
    nfc.hud_offset_y = -20;
    MapFeeds none(nfc, true, true, 10);
    none.poll(1000);
    ok("HUD on, no renderer yet: the status has the saved place, not the centre",
       none.status("decode").hud_offset_x == 30 && none.status("decode").hud_offset_y == -20);
    none.set_hud_offset(40, -20);
    ok("and a move then is taken as asked", none.status("decode").hud_offset_x == 40);
    }

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
