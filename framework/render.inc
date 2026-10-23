// ============================================================================
// render.inc - M1 render / input dispatch layer (framework side).
//
// This is the "seam" slice of M1: it does NOT move any hook yet. The Present
// hook, vtable patch, BGRA device hook and FLUTTERVIEW subclassing all stay in
// overlay.inc; overlay.inc now calls into here at two points:
//   * DrawD2D()  -> plugins::RenderFrame()   after it draws its own pill + log
//   * ViewProc() -> plugins::DispatchClick()  for clicks the pill did not take
// so plugin addDraw / addRegion callbacks go live while the old monolithic
// overlay keeps drawing (gray release, see docs/规划与进度-精简版.md M1).
//
// Contract (sdk/ttplugin.h): draw/region run in logical pixels (96 dpi) and the
// framework owns the DPI scale, so RenderFrame sets the scale transform and
// hands plugins logical width/height. Draw runs on the render thread, click on
// the window thread. Every callback is wrapped in the same inflight guard as
// RunPendingTicks/RunPendingTasks, so an unload can prove nothing is on a stack
// (see plugin_loader.inc).
//
// Package: framework. Depends on plugin_api.inc (included below). Pure ASCII.
// ============================================================================

#ifndef TTVOICE_RENDER_INC
#define TTVOICE_RENDER_INC

#include "plugin_api.inc"   // g_slots, PluginSlot, DrawEntry, RegionEntry, PluginState

namespace plugins {

// Draw every active plugin's draw callbacks, lower priority first (matches
// TTPluginHost::addDraw: "lower priority draws first"). d2dCtx is the shared
// ID2D1DeviceContext* as void*; width/height are logical pixels. The caller has
// already set the DPI scale transform.
static void RenderFrame(void* d2dCtx, float width, float height) {
    if (d2dCtx == nullptr) return;

    struct Item { int priority; int slot; int draw; };
    Item items[kMaxPlugins * TTPLUGIN_MAX_DRAWS];
    int n = 0;

    for (int s = 0; s < g_slotCount; ++s) {
        PluginSlot& slot = g_slots[s];
        if (slot.state != PluginState::kActive) continue;
        for (int d = 0; d < slot.drawCount; ++d) {
            if (n < static_cast<int>(sizeof(items) / sizeof(items[0]))) {
                items[n].priority = slot.draws[d].priority;
                items[n].slot = s;
                items[n].draw = d;
                ++n;
            }
        }
    }

    // One-shot proof that the seam is live (goes to %TEMP%\tt_version_proxy.log).
    static bool s_loggedFrame = false;
    if (!s_loggedFrame) {
        s_loggedFrame = true;
        Log("render: frame dispatch active, %d plugin draw(s)", n);
    }

    // Ascending by priority; insertion sort is fine (n is tiny and bounded).
    for (int i = 1; i < n; ++i) {
        const Item key = items[i];
        int j = i - 1;
        while (j >= 0 && items[j].priority > key.priority) {
            items[j + 1] = items[j];
            --j;
        }
        items[j + 1] = key;
    }

    for (int i = 0; i < n; ++i) {
        PluginSlot& slot = g_slots[items[i].slot];
        const DrawEntry entry = slot.draws[items[i].draw];  // copy before guarding
        if (entry.fn == nullptr) continue;
        InterlockedIncrement(&slot.inflight);
        if (slot.state == PluginState::kActive) {
            entry.fn(d2dCtx, width, height, entry.user);
        }
        InterlockedDecrement(&slot.inflight);
    }
}

// ---------------------------------------------------------------------------
// Pointer dispatch (hover / drag).
//
// MOVE is offered to every active sink until one of them consumes it, so a
// plugin that only cares about its own rect can ignore the rest for free. A
// sink that consumes DOWN owns the pointer: the following MOVE/UP go to that
// plugin only and are always swallowed, because a drag must never leak to the
// app underneath. Capture state lives on the window thread, like DispatchClick.
// ---------------------------------------------------------------------------
static int g_pointerSlot = -1;   // slot owning the capture, -1 when free

static bool CallPointer(int index, int kind, float x, float y) {
    if (index < 0 || index >= g_slotCount) return false;
    PluginSlot& slot = g_slots[index];
    if (slot.state != PluginState::kActive || slot.pointer.fn == nullptr) {
        return false;
    }
    const PointerEntry entry = slot.pointer;   // copy before guarding
    int consumed = 0;
    InterlockedIncrement(&slot.inflight);
    if (slot.state == PluginState::kActive) {
        consumed = entry.fn(kind, x, y, entry.user);
    }
    InterlockedDecrement(&slot.inflight);
    return consumed != 0;
}

// Returns true when the event was consumed. The caller stops forwarding it.
static bool DispatchPointer(int kind, float x, float y) {
    if (g_pointerSlot >= 0) {
        const int owner = g_pointerSlot;
        if (kind == TTPLUGIN_POINTER_UP) {
            g_pointerSlot = -1;   // release first: UP ends the capture either way
            CallPointer(owner, kind, x, y);
        } else if (!CallPointer(owner, kind, x, y)) {
            g_pointerSlot = -1;   // owner unloaded mid drag: drop the capture
            Log("render: pointer capture dropped");
        }
        return true;
    }

    if (kind == TTPLUGIN_POINTER_UP) {
        return false;   // nobody captured: the region click path owns the release
    }

    for (int s = 0; s < g_slotCount; ++s) {
        if (!CallPointer(s, kind, x, y)) continue;
        if (kind == TTPLUGIN_POINTER_DOWN) {
            g_pointerSlot = s;
            Log("render: pointer captured by plugin %s", g_slots[s].name);
        }
        return true;
    }
    return false;
}

// The OS took the capture away (alt-tab, another window grabbing the mouse).
// Close the drag at the last known point so the plugin cannot stay latched.
static bool CancelPointerCapture(float x, float y) {
    if (g_pointerSlot < 0) return false;
    const int owner = g_pointerSlot;
    g_pointerSlot = -1;
    CallPointer(owner, TTPLUGIN_POINTER_UP, x, y);
    return true;
}

// Hit-test a logical-pixel point against every active plugin's regions; the
// highest-priority region under the point wins and receives the click. Returns
// true when a region consumed it, so the caller can stop forwarding the event.
// kind: 0 = left (only left is dispatched in the M1 seam; right/double follow).
// Pointer hover/drag goes through DispatchPointer above instead.
static bool DispatchClick(float logicalX, float logicalY, int kind) {
    int bestSlot = -1;
    int bestRegion = -1;
    int bestPriority = 0;
    bool found = false;

    for (int s = 0; s < g_slotCount; ++s) {
        PluginSlot& slot = g_slots[s];
        if (slot.state != PluginState::kActive) continue;
        for (int r = 0; r < slot.regionCount; ++r) {
            const RECT& rc = slot.regions[r].rect;
            if (logicalX < static_cast<float>(rc.left) ||
                logicalX >= static_cast<float>(rc.right) ||
                logicalY < static_cast<float>(rc.top) ||
                logicalY >= static_cast<float>(rc.bottom)) {
                continue;
            }
            if (!found || slot.regions[r].priority > bestPriority) {
                found = true;
                bestPriority = slot.regions[r].priority;
                bestSlot = s;
                bestRegion = r;
            }
        }
    }

    if (!found) return false;

    Log("render: click kind=%d hit region at (%.0f,%.0f)", kind, logicalX, logicalY);

    PluginSlot& slot = g_slots[bestSlot];
    const RegionEntry entry = slot.regions[bestRegion];  // copy before guarding
    if (entry.fn == nullptr) return false;
    InterlockedIncrement(&slot.inflight);
    if (slot.state == PluginState::kActive) {
        entry.fn(kind, logicalX, logicalY, entry.user);
    }
    InterlockedDecrement(&slot.inflight);
    return true;
}

}  // namespace plugins

#endif  // TTVOICE_RENDER_INC
