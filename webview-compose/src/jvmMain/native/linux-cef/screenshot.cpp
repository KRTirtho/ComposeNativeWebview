#include "compose_cef_internal.h"

/* Screenshot: encode the latest OSR frame (BGRA) to PNG via cairo. */

extern "C" {

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeCaptureScreenshot(
    JNIEnv *, jclass, jlong handle) {
    std::shared_ptr<ComposeCefViewState> state = compose_cef_shared_from_handle(handle);
    if (state == nullptr) {
        compose_cef_call_on_screenshot(handle, {});
        return;
    }
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        bgra = state->pixels;
        width = state->frame_width;
        height = state->frame_height;
    }
    if (bgra.empty() || width <= 0 || height <= 0) {
        compose_cef_call_on_screenshot(handle, {});
        return;
    }
    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        bgra.data(), CAIRO_FORMAT_ARGB32, width, height, width * 4);
    std::vector<uint8_t> png;
    cairo_write_func_t write_fn = [](void *closure, const unsigned char *data, unsigned int length) {
        auto *out = static_cast<std::vector<uint8_t> *>(closure);
        out->insert(out->end(), data, data + length);
        return CAIRO_STATUS_SUCCESS;
    };
    cairo_surface_write_to_png_stream(surface, write_fn, &png);
    cairo_surface_destroy(surface);
    compose_cef_call_on_screenshot(handle, png);
}

}  // extern "C"
