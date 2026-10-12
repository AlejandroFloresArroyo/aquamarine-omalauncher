// CPU-only test of the wl_shm presentation logic (aquamarine src/backend/WaylandShm.hpp, added by
// patches/0001-wayland-wl_shm-presentation-fallback.patch). No Wayland, no GPU.
//
// Build against a patched aquamarine checkout (one line):
//   g++ -std=c++23 -O2 -I<aquamarine>/src/backend -I/usr/include/pixman-1 tests/shm_copy_test.cpp -lhyprutils -lpixman-1 -o shm_copy_test
//   ./shm_copy_test
//
// 1. clippedRects + copyRects copy exactly the clipped damage, from a padded source with an offset.
// 2. hostDamageRects returns the region, its bounding box above MAX_DAMAGE_RECTS, or one pixel for
//    an empty region.
// 3. A step-by-step simulation (one step = one host frame) of CWaylandOutput::presentShm,
//    advanceShm, recopyUnverifiedShm, onFrameDone and the wl_buffer.release handler, with the same
//    helpers and the same decisions, against:
//    - a host that copies the damaged rectangles of each commit into its own store and releases
//      buffers immediately, late, randomly or only after stalls;
//    - a GPU that finishes each frame 0-3 host frames after it was rendered. Frames come with a
//      fence (copied only after it signals), without one (copied right away, which may read the
//      buffer's old content in the frame's damage; the GPU is done one host frame later, before
//      the re-copy timer fires), or mixed.
//    The consumer renders on frame events, on idle frames when nothing is in flight, and now and
//    then while a frame is in flight (as after an xdg_surface.configure).
//    Checked: no buffer changes while the host holds it; a frame in flight always has a
//    wl_surface.frame or a pending present that will end it; whenever nothing is pending or
//    unverified, the host's store equals the newest frame; the consumer is never stuck.

#include "WaylandShm.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

using Hyprutils::Math::CRegion;
namespace WS = Aquamarine::WaylandShm;

static int g_failures = 0;
#define CHECK(cond, ...)                                                                                                                                                           \
    do {                                                                                                                                                                           \
        if (!(cond)) {                                                                                                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                                                                  \
            std::fprintf(stderr, __VA_ARGS__);                                                                                                                                     \
            std::fprintf(stderr, "\n");                                                                                                                                            \
            if (++g_failures > 20)                                                                                                                                                 \
                std::exit(1);                                                                                                                                                      \
        }                                                                                                                                                                          \
    } while (0)

static std::mt19937 g_rng(0xA9A11);

static int          rnd(int lo, int hi) { // inclusive
    return std::uniform_int_distribution<int>(lo, hi)(g_rng);
}

// A 32 bpp image with a padded stride and some bytes in front, like a dmabuf plane with an offset.
struct SImage {
    int                  w = 0, h = 0;
    size_t               stride = 0, offset = 0;
    std::vector<uint8_t> mem;

    SImage(int w_, int h_, size_t pad, size_t offset_) : w(w_), h(h_), stride((size_t)w_ * 4 + pad), offset(offset_), mem(offset_ + stride * h_ + 16) {}

    uint8_t* px() {
        return mem.data() + offset;
    }
    const uint8_t* px() const {
        return mem.data() + offset;
    }
    uint32_t at(int x, int y) const {
        uint32_t v;
        std::memcpy(&v, px() + (size_t)y * stride + (size_t)x * 4, 4);
        return v;
    }
    void set(int x, int y, uint32_t v) {
        std::memcpy(px() + (size_t)y * stride + (size_t)x * 4, &v, 4);
    }
};

static CRegion randomDamage(int w, int h, int maxRects) {
    CRegion   r;
    const int N = rnd(0, maxRects);
    for (int i = 0; i < N; ++i) {
        // allowed to start before 0 and to reach past the buffer, like Hyprland's damage
        const int x = rnd(-w / 4, w), y = rnd(-h / 4, h);
        r.add(x, y, rnd(1, w / 2 + 1), rnd(1, h / 2 + 1));
    }
    if (rnd(0, 30) == 0)
        r.add(0, 0, w * 10, h * 10); // Hyprland's "damage everything" box
    return r;
}

static bool inRegion(const std::vector<pixman_box32_t>& rects, int x, int y) {
    for (const auto& r : rects) {
        if (x >= r.x1 && x < r.x2 && y >= r.y1 && y < r.y2)
            return true;
    }
    return false;
}

static void testCopyRects() {
    for (int iter = 0; iter < 400; ++iter) {
        const int w = rnd(1, 97), h = rnd(1, 61);
        SImage    src(w, h, rnd(0, 3) * 64, rnd(0, 2) * 4096);
        SImage    dst(w, h, 0, 0);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                src.set(x, y, (uint32_t)g_rng());
                dst.set(x, y, 0xDEADBEEF);
            }
        }

        const auto   DAMAGE = randomDamage(w, h, 6);
        const auto   RECTS  = WS::clippedRects(DAMAGE, w, h);
        const size_t BYTES  = WS::copyRects(dst.px(), dst.stride, src.px(), src.stride, RECTS);

        size_t       area = 0;
        for (const auto& r : RECTS) {
            CHECK(r.x1 >= 0 && r.y1 >= 0 && r.x2 <= w && r.y2 <= h, "rect %d,%d-%d,%d outside %dx%d", r.x1, r.y1, r.x2, r.y2, w, h);
            area += (size_t)(r.x2 - r.x1) * (r.y2 - r.y1);
        }
        CHECK(BYTES == area * 4, "copied %zu bytes for an area of %zu px", BYTES, area);

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const bool IN = inRegion(RECTS, x, y);
                CHECK(IN == DAMAGE.containsPoint({x + 0.5, y + 0.5}), "clip mismatch at %d,%d", x, y);
                if (IN)
                    CHECK(dst.at(x, y) == src.at(x, y), "pixel %d,%d not copied", x, y);
                else
                    CHECK(dst.at(x, y) == 0xDEADBEEF, "pixel %d,%d written outside the damage", x, y);
            }
        }
    }
    std::printf("copyRects: ok\n");
}

static void testHostDamageRects() {
    CRegion few;
    few.add(0, 0, 10, 10);
    few.add(20, 20, 5, 5);
    CHECK(WS::hostDamageRects(few).size() == 2, "expected 2 rects");

    CRegion many;
    for (int i = 0; i < 100; ++i) {
        many.add(i * 3, i * 2, 1, 1); // 100 disjoint pixels
    }
    const auto R = WS::hostDamageRects(many);
    CHECK(R.size() == 1, "expected the bounding box, got %zu rects", R.size());
    CHECK(R[0].x1 == 0 && R[0].y1 == 0 && R[0].x2 == 298 && R[0].y2 == 199, "bounding box %d,%d-%d,%d", R[0].x1, R[0].y1, R[0].x2, R[0].y2);

    const auto E = WS::hostDamageRects(CRegion{});
    CHECK(E.size() == 1 && E[0].x1 == 0 && E[0].y1 == 0 && E[0].x2 == 1 && E[0].y2 == 1, "an empty region must give one pixel");
    std::printf("hostDamageRects: ok\n");
}

// ---- simulation ----

// A rendered swapchain buffer. Until the GPU is done, reading it gives `before` in the frame's
// damage (the buffer's old content there) and the right pixels elsewhere, as with buffer age.
struct SFrame {
    std::shared_ptr<SImage> img, before;
    bool                    fenced    = false;
    int                     gpuDoneAt = 0; // host frame (step) at which the GPU work and its fence complete
};
using SPFrame = std::shared_ptr<SFrame>;

struct SFakeShmBuffer {
    std::unique_ptr<SImage> img;
    CRegion                 stale;
    bool                    busy    = false;
    uint64_t                syncSeq = 0;
    std::vector<uint8_t>    snapshotAtAttach; // to prove nothing writes while the host holds it
};
using SPBuf = std::shared_ptr<SFakeShmBuffer>;

// Mirrors CWaylandOutput::presentShm / advanceShm / forgetShmPending / recopyUnverifiedShm /
// onFrameDone and the release handler. `sched` is reduced to inFlight, `frameCallback` to
// callbackOutstanding, `recopyTimer` to the step it fires at.
struct SPresenter {
    struct SPending {
        SPFrame frame;
        bool    isFrame = true;
    };

    std::vector<SPBuf>      buffers;
    CRegion                 hostDamage;
    int                     w = 0, h = 0;
    uint64_t                seq        = 0;
    bool                    configured = false, presented = false;
    bool                    inFlight = false, callbackOutstanding = false;
    int                     callbackDueAt = 0; // a host answers a frame callback at its next frame
    std::optional<SPending> pending;
    CRegion                 unverified;
    SPFrame                 unverifiedSource;
    std::optional<int>      recopyTimerAt; // the re-copy timer (25 ms: two host frames at 90 Hz)
    int                     now         = 0;
    size_t                  bytesCopied = 0, waits = 0, recopies = 0;

    // the host: called on every wl_surface.commit with the attached buffer and the damage sent
    std::function<void(const SPBuf&, const std::vector<pixman_box32_t>&)> hostCommit;

    void present(const SPFrame& f, const std::optional<CRegion>& damageBit) {
        if (w != f->img->w || h != f->img->h) {
            pending.reset();
            buffers.clear();
            w = f->img->w, h = f->img->h;
            hostDamage = CRegion(0, 0, w, h);
            unverified.clear();
            unverifiedSource.reset();
        }

        CRegion damage = damageBit ? *damageBit : CRegion(0, 0, w, h);
        damage.add(unverified);
        damage.intersect(0, 0, w, h);
        unverified.clear();
        unverifiedSource.reset();
        WS::addFrameDamage(buffers, hostDamage, damage);

        if (!configured)
            return;

        if (hostDamage.empty() && !pending && (inFlight || !presented))
            return;

        pending = SPending{.frame = f, .isFrame = true};
        inFlight = true;
        advance();
    }

    void advance() {
        if (!pending)
            return;

        auto buf = WS::pickFree(buffers);
        if (!buf && buffers.size() < WS::MAX_BUFFERS) {
            buf        = std::make_shared<SFakeShmBuffer>();
            buf->img   = std::make_unique<SImage>(w, h, 0, 0);
            buf->stale = CRegion(0, 0, w, h);
            buffers.emplace_back(buf);
        }
        if (!buf) {
            waits++;
            return; // a release continues
        }

        CHECK(!buf->busy, "picked a buffer the host still holds");
        const auto RECTS = WS::clippedRects(buf->stale, w, h);
        const auto F     = pending->frame;

        if (!RECTS.empty() && F->fenced && now < F->gpuDoneAt) {
            waits++;
            return; // the fence goes into the epoll set; its signal continues
        }

        // what the CPU reads: the frame, or its old content where the GPU has not written yet
        const SImage& SRC = now >= F->gpuDoneAt ? *F->img : *F->before;
        bytesCopied += WS::copyRects(buf->img->px(), buf->img->stride, SRC.px(), SRC.stride, RECTS);

        const bool UNVERIFIED = pending->isFrame && !RECTS.empty() && !F->fenced;
        if (UNVERIFIED) {
            unverified.add(buf->stale);
            unverified.intersect(0, 0, w, h);
            unverifiedSource = F;
        }
        if (!pending->isFrame)
            recopies++;

        buf->stale.clear();
        buf->syncSeq = ++seq;

        const auto DAMAGE = WS::hostDamageRects(hostDamage);
        hostDamage.clear();
        buf->busy = true;
        presented = true;

        if (UNVERIFIED)
            recopyTimerAt = now + 2;

        if (pending->isFrame && !callbackOutstanding) {
            callbackOutstanding = true;
            callbackDueAt       = now + 1;
            inFlight            = true;
        }

        pending.reset();
        hostCommit(buf, DAMAGE);
    }

    void recopyUnverified() {
        if (unverified.empty() || pending)
            return;

        auto source = unverifiedSource;
        auto region = unverified.copy();
        unverified.clear();
        unverifiedSource.reset();
        if (!source || source->img->w != w || source->img->h != h)
            return;

        WS::addFrameDamage(buffers, hostDamage, region);
        pending = SPending{.frame = source, .isFrame = false};
        advance();
    }

    // wl_callback.done of the frame loop; returns whether the consumer gets a frame event
    bool frameDone() {
        callbackOutstanding = false;
        if (pending && pending->isFrame)
            return false; // the pending frame ends this cycle

        inFlight = false;
        return true;
    }

    void release(const SPBuf& b) {
        b->busy = false;
        advance();
    }
};

enum eRelease {
    RELEASE_IMMEDIATE = 0,
    RELEASE_LATE,
    RELEASE_RANDOM,
    RELEASE_STALLS,
};

enum eFences {
    FENCES_ALL = 0,
    FENCES_NONE,
    FENCES_MIXED,
};

static void simulate(eRelease policy, eFences fences, const char* name) {
    SPresenter                                  p;
    std::vector<uint32_t>                       store; // the host's copy of our surface
    int                                         sw = 0, sh = 0;
    std::vector<std::pair<SPBuf, int /*step*/>> held; // buffers the host holds, with the step to release them
    SPFrame                                     latest;
    bool                                        wantsRender = true; // the consumer has damage it has not rendered
    int                                         stuckFor    = 0;
    int                                         waitingFrameSince = -1; // a configured frame was committed, no frame event since
    int                                         step        = 0;
    size_t                                      damageArea = 0, frames = 0, commits = 0, maxBuffers = 0;

    p.hostCommit = [&](const SPBuf& buf, const std::vector<pixman_box32_t>& damage) {
        commits++;
        CHECK(buf && !damage.empty(), "[%s] step %d: a commit without a buffer or without damage", name, step);
        if (sw != buf->img->w || sh != buf->img->h) {
            sw = buf->img->w, sh = buf->img->h;
            store.assign((size_t)sw * sh, 0x00BADBAD);
        }
        for (const auto& r : damage) {
            for (int y = std::max(r.y1, 0); y < std::min(r.y2, sh); ++y) {
                for (int x = std::max(r.x1, 0); x < std::min(r.x2, sw); ++x) {
                    store[(size_t)y * sw + x] = buf->img->at(x, y);
                }
            }
        }
        buf->snapshotAtAttach = buf->img->mem;

        int releaseAt = step;
        switch (policy) {
            case RELEASE_IMMEDIATE: releaseAt = step; break;
            case RELEASE_LATE: releaseAt = step + 2; break;
            case RELEASE_RANDOM: releaseAt = step + rnd(0, 4); break;
            case RELEASE_STALLS: releaseAt = step + (rnd(0, 15) == 0 ? rnd(5, 30) : 0); break;
        }
        held.emplace_back(buf, releaseAt);
    };

    // the consumer renders a frame and commits it
    const auto render = [&]() {
        std::optional<CRegion> damage;
        auto                   f = std::make_shared<SFrame>();

        if (!latest || rnd(0, 400) == 0) {
            // first frame or resize: everything is new; the whole output damaged, or no damage at all
            const int W = rnd(20, 120), H = rnd(20, 90);
            f->img      = std::make_shared<SImage>(W, H, rnd(0, 2) * 64, rnd(0, 1) * 4096);
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    f->img->set(x, y, (uint32_t)g_rng());
                }
            }
            if (rnd(0, 1))
                damage = CRegion(0, 0, W, H);
        } else {
            f->img = std::make_shared<SImage>(*latest->img);
            if (!wantsRender && rnd(0, 1))
                damage = CRegion{}; // a frame without changes (Hyprland's frame callbacks for clients)
            else {
                damage = randomDamage(f->img->w, f->img->h, rnd(0, 1) ? 3 : 40);
                // change pixels only inside the reported damage (the damage may report more)
                for (const auto& r : WS::clippedRects(*damage, f->img->w, f->img->h)) {
                    for (int y = r.y1; y < r.y2; ++y) {
                        for (int x = r.x1; x < r.x2; ++x) {
                            if (rnd(0, 3))
                                f->img->set(x, y, (uint32_t)g_rng());
                        }
                    }
                }
            }
        }

        // before the GPU is done: old content (garbage here) inside the frame's damage
        f->before        = std::make_shared<SImage>(*f->img);
        const auto DRECT = WS::clippedRects(damage ? *damage : CRegion(0, 0, f->img->w, f->img->h), f->img->w, f->img->h);
        for (const auto& r : DRECT) {
            for (int y = r.y1; y < r.y2; ++y) {
                for (int x = r.x1; x < r.x2; ++x) {
                    f->before->set(x, y, (uint32_t)g_rng() | 1);
                }
            }
            damageArea += (size_t)(r.x2 - r.x1) * (r.y2 - r.y1);
        }

        f->fenced    = fences == FENCES_ALL || (fences == FENCES_MIXED && rnd(0, 1));
        f->gpuDoneAt = step + (f->fenced ? rnd(0, 3) : rnd(0, 1)); // without a fence: done within one host frame
        latest       = f;
        wantsRender  = false;
        frames++;
        if (p.configured && waitingFrameSince < 0)
            waitingFrameSince = step;
        p.present(f, damage);
        maxBuffers = std::max(maxBuffers, p.buffers.size());
    };

    const int STEPS = 6000;
    for (step = 0; step < STEPS + 200; ++step) {
        p.now = step;

        // host: releases due now
        auto due = held;
        std::erase_if(held, [&](auto& e) { return e.second <= step; });
        for (auto& [buf, at] : due) {
            if (at > step)
                continue;
            CHECK(buf->img->mem == buf->snapshotAtAttach, "[%s] a buffer changed while the host held it", name);
            // buffers dropped on a resize were destroyed (wl_buffer.destroy): no release event for them
            if (std::ranges::find(p.buffers, buf) != p.buffers.end())
                p.release(buf);
            else
                buf->busy = false;
        }

        // GPU: a signaled fence makes the epoll fd readable -> dispatchShmFences -> advanceShm
        if (p.pending && p.pending->frame->fenced && step >= p.pending->frame->gpuDoneAt)
            p.advance();

        if (step == 5)
            p.configured = true; // xdg_surface.configure acked; earlier frames are only recorded

        // new damage from clients (none in the last 200 steps: the session settles)
        if (step < STEPS && rnd(0, 2) == 0)
            wantsRender = true;

        // a frame while one is in flight (xdg_surface.configure emits events.frame directly)
        if (p.configured && p.inFlight && step < STEPS && rnd(0, 30) == 0)
            render();

        // the re-copy timer (in the same epoll set as the fences)
        if (p.recopyTimerAt && step >= *p.recopyTimerAt) {
            p.recopyTimerAt.reset();
            p.recopyUnverified();
        }

        // host frame: wl_callback.done -> events.frame -> the consumer renders if it wants to
        if (p.callbackOutstanding && step >= p.callbackDueAt) {
            if (p.frameDone()) {
                waitingFrameSince = -1;
                if (wantsRender || (step < STEPS && rnd(0, 9) == 0))
                    render();
            }
        } else if (wantsRender && !p.inFlight)
            render(); // scheduleFrame -> idle -> events.frame

        // invariants
        CHECK(!p.inFlight || p.callbackOutstanding || (p.pending && p.pending->isFrame), "[%s] step %d: a frame in flight that nothing will end", name, step);
        if (p.configured && p.presented && !p.pending && p.unverified.empty() && latest && latest->img->w == sw && latest->img->h == sh) {
            bool same = true;
            for (int y = 0; y < sh && same; ++y) {
                for (int x = 0; x < sw && same; ++x) {
                    same = store[(size_t)y * sw + x] == latest->img->at(x, y);
                }
            }
            CHECK(same, "[%s] step %d: the host's store differs from the newest frame", name, step);
        }
        // every committed frame is followed by a frame event (Hyprland sends its clients' frame
        // callbacks from there), also when it had nothing new for the host
        CHECK(waitingFrameSince < 0 || step - waitingFrameSince < 60, "[%s] step %d: no frame event for 60 host frames after a commit", name, step);
        stuckFor = (wantsRender && p.configured) ? stuckFor + 1 : 0;
        CHECK(stuckFor < 60, "[%s] step %d: the consumer has not been able to render for 60 host frames", name, step);
    }

    CHECK(!p.pending && p.unverified.empty(), "[%s] still pending after the session settled", name);
    std::printf("%-22s frames %5zu  commits %5zu  recopies %4zu  waits %4zu  buffers %zu  copied/damaged %.3f\n", name, frames, commits, p.recopies, p.waits, maxBuffers,
                damageArea ? (double)(p.bytesCopied / 4) / (double)damageArea : 0.0);
}

int main() {
    testCopyRects();
    testHostDamageRects();

    const std::pair<eRelease, const char*> RELEASES[] = {{RELEASE_IMMEDIATE, "immediate"}, {RELEASE_LATE, "late"}, {RELEASE_RANDOM, "random"}, {RELEASE_STALLS, "stalls"}};
    const std::pair<eFences, const char*>  FENCES[]   = {{FENCES_ALL, "fenced"}, {FENCES_NONE, "no-fence"}, {FENCES_MIXED, "mixed"}};
    for (const auto& [f, fname] : FENCES) {
        for (const auto& [r, rname] : RELEASES) {
            simulate(r, f, (std::string(rname) + "/" + fname).c_str());
        }
    }

    if (g_failures) {
        std::printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
