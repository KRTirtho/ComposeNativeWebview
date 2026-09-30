#include "compose_cef_internal.h"

#include <cstring>

/* OSR OnPaint → BGRA buffer → cairo surface, drawn on the GTK widget.
 * The CEF OSR buffer is 32-bit BGRA premultiplied, row stride = width*4,
 * identical layout to CAIRO_FORMAT_ARGB32 on little-endian. */

void compose_cef_on_paint(
    const std::shared_ptr<ComposeCefViewState> &state,
    const void *buffer,
    int width,
    int height) {
    if (width <= 2 || height <= 2) return;  // ignore tiny startup buffers
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closing) return;
        const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        state->pixels.resize(size);
        std::memcpy(state->pixels.data(), buffer, size);
        state->frame_width = width;
        state->frame_height = height;
    }
    compose_cef_queue_draw(state);
}

void compose_cef_queue_draw(const std::shared_ptr<ComposeCefViewState> &state) {
    bool expected = false;
    if (!state->gtk_draw_pending.compare_exchange_strong(expected, true)) return;

    auto *payload = new std::shared_ptr<ComposeCefViewState>(state);
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            auto *state_ref = static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
            GtkWidget *widget = nullptr;
            {
                std::lock_guard<std::mutex> lock((*state_ref)->mutex);
                if (!(*state_ref)->closing) widget = (*state_ref)->widget;
            }
            if (widget != nullptr) gtk_widget_queue_draw(widget);
            (*state_ref)->gtk_draw_pending.store(false);
            return G_SOURCE_REMOVE;
        },
        payload,
        [](gpointer data) {
            delete static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
        });
}
