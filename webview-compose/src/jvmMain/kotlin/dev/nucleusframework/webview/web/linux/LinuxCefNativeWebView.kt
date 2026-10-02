package dev.nucleusframework.webview.web.linux

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import dev.nucleusframework.webview.web.NativeWebView
import dev.nucleusframework.window.tao.NucleusPlatformView
import java.io.File
import kotlinx.coroutines.CompletableDeferred

/**
 * Linux [NativeWebView] backed by a direct Chromium Embedded Framework (CEF)
 * browser in windowless (off-screen) mode, embedded via a GTK `DrawingArea`
 * through [NucleusPlatformView.GtkWidget] / Nucleus
 * [dev.nucleusframework.window.tao.NativeView].
 *
 * Requires the Tao window backend and the bundled CEF runtime (see
 * [CefLinuxBridge.isAvailable]).
 */
class LinuxCefNativeWebView(
    customUserAgent: String? = null,
    dataDirectory: String? = null,
    initScript: String? = null,
    /** JS bridge bootstrap injected at document start in all frames. */
    jsBridgeScript: String? = null,
    incognito: Boolean = false,
    enableDevtools: Boolean = false,
    javascriptEnabled: Boolean = true,
    zoomLevel: Double = 1.0,
    transparent: Boolean = false,
    backgroundColor: Color = Color.White,
    initialUrl: String? = null,
) : NativeWebView() {
    private val handle: Long
    private val gtkWidgetHandle: Long
    private var released = false

    init {
        val runtimeDir = requireNotNull(CefLinuxBridge.runtimeDir) {
            "CEF runtime is not available (not bundled in this application)"
        }
        // Non-transparent: always fully opaque so pages look like a normal browser.
        val effective =
            if (transparent) {
                backgroundColor
            } else if (backgroundColor.alpha < 1f) {
                Color.White
            } else {
                backgroundColor.copy(alpha = 1f)
            }
        val argb = effective.toArgb()
        val a = ((argb ushr 24) and 0xFF) / 255f
        val r = ((argb ushr 16) and 0xFF) / 255f
        val g = ((argb ushr 8) and 0xFF) / 255f
        val b = (argb and 0xFF) / 255f
        val cacheDir = dataDirectory?.trim()?.takeIf { it.isNotEmpty() }
            ?: File(System.getProperty("user.home"), ".cache/composewebview-cef").absolutePath
        handle = CefLinuxBridge.nativeCreate(
            runtimeDir = runtimeDir,
            cacheDir = cacheDir,
            userAgent = customUserAgent?.trim()?.takeIf { it.isNotEmpty() },
            initScript = initScript?.trim()?.takeIf { it.isNotEmpty() },
            jsBridgeScript = jsBridgeScript?.trim()?.takeIf { it.isNotEmpty() },
            incognito = incognito,
            enableDevtools = enableDevtools,
            javascriptEnabled = javascriptEnabled,
            zoomLevel = zoomLevel,
            transparent = transparent,
            bgR = r,
            bgG = g,
            bgB = b,
            bgA = a,
            initialUrl = initialUrl,
        )
        require(handle != 0L) { "Failed to create native CEF browser" }
        gtkWidgetHandle = CefLinuxBridge.nativeGetGtkWidget(handle)
        require(gtkWidgetHandle != 0L) { "Failed to get GtkWidget handle" }
    }

    /** Creates the [NucleusPlatformView] used by NativeView embedding. */
    internal fun setOnPointerFocus(handler: (() -> Unit)?) {
        CefLinuxBridge.setPointerFocusHandler(handle, handler)
    }

    fun asPlatformView(): NucleusPlatformView.GtkWidget =
        object : NucleusPlatformView.GtkWidget {
            override val gtkWidgetHandle: Long
                get() = this@LinuxCefNativeWebView.gtkWidgetHandle

            override fun resize(widthPx: Int, heightPx: Int) {
                if (isReady()) CefLinuxBridge.nativeResize(handle, widthPx, heightPx)
            }

            override fun dispose() {
                // Lifecycle owned by NativeWebView.destroy(); NativeView
                // also calls dispose — keep it idempotent.
            }
        }

    override fun isReady(): Boolean = !released && handle != 0L && CefLinuxBridge.nativeIsReady(handle)

    override fun isLoading(): Boolean =
        if (!isReady()) false else CefLinuxBridge.nativeIsLoading(handle)

    override fun getCurrentUrl(): String? =
        if (!isReady()) null else CefLinuxBridge.nativeCurrentUrl(handle)

    override fun getTitle(): String? =
        if (!isReady()) null else CefLinuxBridge.nativeGetTitle(handle)

    override fun canGoBack(): Boolean =
        if (!isReady()) false else CefLinuxBridge.nativeCanGoBack(handle)

    override fun canGoForward(): Boolean =
        if (!isReady()) false else CefLinuxBridge.nativeCanGoForward(handle)

    override fun loadUrl(url: String, additionalHttpHeaders: Map<String, String>) {
        if (!isReady()) return
        if (additionalHttpHeaders.isEmpty()) {
            CefLinuxBridge.nativeLoadUrl(handle, url)
        } else {
            val names = additionalHttpHeaders.keys.toTypedArray()
            val values = additionalHttpHeaders.values.toTypedArray()
            CefLinuxBridge.nativeLoadUrlWithHeaders(handle, url, names, values)
        }
    }

    override fun loadHtml(html: String) {
        if (!isReady()) return
        CefLinuxBridge.nativeLoadHtml(handle, html, null)
    }

    fun loadHtml(html: String, baseUri: String?) {
        if (!isReady()) return
        CefLinuxBridge.nativeLoadHtml(handle, html, baseUri)
    }

    override fun goBack() {
        if (!isReady()) return
        CefLinuxBridge.nativeGoBack(handle)
    }

    override fun goForward() {
        if (!isReady()) return
        CefLinuxBridge.nativeGoForward(handle)
    }

    override fun reload() {
        if (!isReady()) return
        CefLinuxBridge.nativeReload(handle)
    }

    override fun stopLoading() {
        if (!isReady()) return
        CefLinuxBridge.nativeStopLoading(handle)
    }

    override fun evaluateJavaScript(script: String, callback: (String) -> Unit) {
        if (!isReady()) {
            callback("")
            return
        }
        CefLinuxBridge.registerJsCallback(handle, callback)
        CefLinuxBridge.nativeEvaluateJavaScript(handle, script)
    }

    override fun drainIpcMessages(): List<String> =
        if (!isReady()) emptyList() else CefLinuxBridge.drainIpcMessages(handle)

    override fun addNavigateListener(listener: (String) -> Boolean) {
        if (!isReady()) return
        CefLinuxBridge.addNavigateListener(handle, listener)
    }

    override fun removeNavigateListener(listener: (String) -> Boolean) {
        if (!isReady()) return
        CefLinuxBridge.removeNavigateListener(handle, listener)
    }

    override fun captureScreenshotNative(): ByteArray? {
        // Synchronous API is not available; callers should use the suspend path.
        return null
    }

    suspend fun captureScreenshotAsync(): ByteArray? {
        if (!isReady()) return null
        val deferred = CompletableDeferred<ByteArray?>()
        CefLinuxBridge.registerScreenshotDeferred(handle, deferred)
        CefLinuxBridge.nativeCaptureScreenshot(handle)
        return deferred.await()
    }

    suspend fun getCookiesJson(url: String): String {
        if (!isReady()) return "[]"
        val deferred = CompletableDeferred<String>()
        CefLinuxBridge.registerCookieDeferred(handle, deferred)
        CefLinuxBridge.nativeGetCookies(handle, url)
        return deferred.await()
    }

    fun setCookieNative(
        name: String,
        value: String,
        domain: String?,
        path: String?,
        secure: Boolean,
        httpOnly: Boolean,
        expiresMs: Long,
        sameSite: String?,
    ) {
        if (!isReady()) return
        CefLinuxBridge.nativeSetCookie(
            handle, name, value, domain, path, secure, httpOnly, expiresMs, sameSite,
        )
    }

    fun removeAllCookiesNative() {
        if (!isReady()) return
        CefLinuxBridge.nativeRemoveAllCookies(handle)
    }

    fun removeCookiesForUrlNative(url: String) {
        if (!isReady()) return
        CefLinuxBridge.nativeRemoveCookiesForUrl(handle, url)
    }

    fun setZoomLevel(zoom: Double) {
        if (!isReady()) return
        CefLinuxBridge.nativeSetZoomLevel(handle, zoom)
    }

    override fun openDevTools() {
        if (!isReady()) return
        CefLinuxBridge.nativeOpenDevTools(handle)
    }

    override fun closeDevTools() {
        if (!isReady()) return
        CefLinuxBridge.nativeCloseDevTools(handle)
    }

    override fun focus() {
        if (!isReady()) return
        CefLinuxBridge.nativeFocus(handle)
    }

    override fun destroy() {
        if (released) return
        released = true
        CefLinuxBridge.clearHandle(handle)
        CefLinuxBridge.nativeRelease(handle)
    }
}
