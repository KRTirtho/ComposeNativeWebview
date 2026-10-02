package dev.nucleusframework.webview.web.linux

import dev.nucleusframework.core.runtime.NativeLibraryLoader
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CompletableDeferred

/**
 * JNI bridge to the GTK/IPC C++ backend (`libcompose_cef_linux.so`). Chromium
 * runs in a separate `cef_subprocess` host; the JVM never initializes CEF.
 *
 * Loaded only on Linux when the CEF runtime is bundled under
 * `nucleus/native/linux-<arch>/` in the jar. All native calls that touch
 * GTK must run on the GTK main thread (Tao application thread), mirroring
 * the WebKit backend.
 *
 * The bridge extracts the full CEF runtime (libcef.so, cef_subprocess, pak
 * files, locales, icudtl.dat) as [NativeLibraryLoader] sidecars into a
 * content-addressed cache dir and passes that directory to native so CEF can
 * locate its resources and subprocess executable.
 */
internal object CefLinuxBridge {
    private const val LIBRARY_NAME = "compose_cef_linux"

    /** CEF runtime payload filenames bundled next to the bridge library. */
    private val RUNTIME_FILES = listOf(
        "libcef.so",
        "cef_subprocess",
        "chrome_100_percent.pak",
        "chrome_200_percent.pak",
        "resources.pak",
        "icudtl.dat",
        "v8_context_snapshot.bin",
        "snapshot_blob.bin",
        "en-US.pak",
    )

    /**
     * Directory containing the extracted CEF runtime, or null when the
     * runtime is not bundled (CEF backend unavailable → WebKit fallback).
     */
    val runtimeDir: String? by lazy {
        val loaded = NativeLibraryLoader.load(
            LIBRARY_NAME,
            CefLinuxBridge::class.java,
            sidecarFiles = RUNTIME_FILES,
        )
        if (!loaded) return@lazy null
        // The loader extracted the bridge .so next to the sidecars; locate
        // the shared cache dir from the library's code source resource.
        resolveExtractedDir()
    }

    val isAvailable: Boolean get() = runtimeDir != null

    /**
     * Resolves the directory the bridge library was extracted to by
     * [NativeLibraryLoader]. The loader content-addresses the main library
     * plus sidecars into `<cache>/linux-<arch>/<fingerprint>/`; we recover
     * that path from the loaded library resource URL.
     */
    private fun resolveExtractedDir(): String? {
        // In dev mode (classes dir) or jar mode, the bridge resource lives at
        // /nucleus/native/linux-<arch>/libcompose_cef_linux.so. The loader
        // copies it (plus sidecars) into its cache dir. We cannot query the
        // loader for the final path directly, so re-derive it by calling the
        // same extraction logic the loader used: ask for the resource URL of
        // the bridge lib and, if it is a file: URL, use its parent; if it is
        // a jar: URL, the loader already extracted to a deterministic cache
        // path which we reproduce here.
        val arch = if (System.getProperty("os.arch").let { it == "aarch64" || it == "arm64" }) {
            "linux-aarch64"
        } else {
            "linux-x64"
        }
        val resourcePath = "/nucleus/native/$arch/lib$LIBRARY_NAME.so"
        val url = CefLinuxBridge::class.java.getResource(resourcePath) ?: return null
        return when (url.protocol) {
            "file" -> java.io.File(url.toURI()).parentFile?.absolutePath
            "jar" -> {
                // Rebuild the loader's cache path: fingerprint from the jar
                // entries of the bridge + sidecars.
                val connection = url.openConnection() as java.net.JarURLConnection
                val fingerprint = buildString {
                    append(connection.jarEntry.crc).append('-').append(connection.jarEntry.size)
                    for (sidecar in RUNTIME_FILES) {
                        val sidecarUrl = CefLinuxBridge::class.java
                            .getResource("/nucleus/native/$arch/$sidecar") ?: continue
                        val sc = sidecarUrl.openConnection() as java.net.JarURLConnection
                        append('_').append(sc.jarEntry.crc).append('-').append(sc.jarEntry.size)
                    }
                }
                val cacheBase = System.getenv("XDG_CACHE_HOME")?.let { java.nio.file.Path.of(it) }
                    ?: java.nio.file.Path.of(System.getProperty("user.home"), ".cache")
                cacheBase.resolve("nucleus").resolve("native").resolve(arch).resolve(fingerprint)
                    .toAbsolutePath().toString()
            }
            else -> null
        }
    }

    private val navigateHandlers =
        ConcurrentHashMap<Long, MutableList<(String) -> Boolean>>()
    private val ipcQueues =
        ConcurrentHashMap<Long, ConcurrentLinkedQueue<String>>()
    private val jsCallbacks =
        ConcurrentHashMap<Long, ConcurrentLinkedQueue<(String) -> Unit>>()
    private val cookieDeferreds =
        ConcurrentHashMap<Long, ConcurrentLinkedQueue<CompletableDeferred<String>>>()
    private val screenshotDeferreds =
        ConcurrentHashMap<Long, ConcurrentLinkedQueue<CompletableDeferred<ByteArray?>>>()
    private val pointerFocusHandlers = ConcurrentHashMap<Long, () -> Unit>()

    fun setPointerFocusHandler(handle: Long, handler: (() -> Unit)?) {
        if (handler == null) pointerFocusHandlers.remove(handle)
        else pointerFocusHandlers[handle] = handler
    }

    fun addNavigateListener(handle: Long, listener: (String) -> Boolean) {
        navigateHandlers.getOrPut(handle) { mutableListOf() }.add(listener)
    }

    fun removeNavigateListener(handle: Long, listener: (String) -> Boolean) {
        navigateHandlers[handle]?.remove(listener)
    }

    fun drainIpcMessages(handle: Long): List<String> {
        val queue = ipcQueues[handle] ?: return emptyList()
        val drained = ArrayList<String>()
        while (true) {
            val next = queue.poll() ?: break
            drained += next
        }
        return drained
    }

    fun registerJsCallback(handle: Long, callback: (String) -> Unit) {
        jsCallbacks.getOrPut(handle) { ConcurrentLinkedQueue() }.add(callback)
    }

    fun registerCookieDeferred(handle: Long, deferred: CompletableDeferred<String>) {
        cookieDeferreds.getOrPut(handle) { ConcurrentLinkedQueue() }.add(deferred)
    }

    fun registerScreenshotDeferred(handle: Long, deferred: CompletableDeferred<ByteArray?>) {
        screenshotDeferreds.getOrPut(handle) { ConcurrentLinkedQueue() }.add(deferred)
    }

    fun clearHandle(handle: Long) {
        pointerFocusHandlers.remove(handle)
        navigateHandlers.remove(handle)
        ipcQueues.remove(handle)
        jsCallbacks.remove(handle)?.forEach { it.invoke("") }
        cookieDeferreds.remove(handle)?.forEach { it.complete("[]") }
        screenshotDeferreds.remove(handle)?.forEach { it.complete(null) }
    }

    // ── Callbacks from native (must be public for JNI) ────────────────

    @JvmStatic
    fun nativeOnNavigate(handle: Long, url: String): Boolean {
        val handlers = navigateHandlers[handle]
        if (handlers.isNullOrEmpty()) return true
        return handlers.any { it(url) }
    }

    @JvmStatic
    fun nativeOnIpcMessage(handle: Long, message: String) {
        ipcQueues.getOrPut(handle) { ConcurrentLinkedQueue() }.add(message)
    }

    @JvmStatic
    fun nativeOnJsResult(handle: Long, result: String) {
        jsCallbacks[handle]?.poll()?.invoke(result)
    }

    @JvmStatic
    fun nativeOnCookiesResult(handle: Long, json: String) {
        cookieDeferreds[handle]?.poll()?.complete(json)
    }

    @JvmStatic
    fun nativeOnScreenshotResult(handle: Long, bytes: ByteArray?) {
        screenshotDeferreds[handle]?.poll()?.complete(bytes)
    }

    @JvmStatic
    fun nativeOnPointerFocus(handle: Long) {
        pointerFocusHandlers[handle]?.invoke()
    }

    // ── Native methods ────────────────────────────────────────────────

    /**
     * Acquires a CEF host process and creates a browser instance. The host is
     * shut down and reaped when its last view is released; reopen starts a new
     * process rather than attempting to restart Chromium inside the JVM.
     * Returns an opaque handle, or 0 on failure.
     *
     * [runtimeDir] must be the directory containing libcef.so, cef_subprocess,
     * the pak files, locales/ and icudtl.dat. [cacheDir] is the CEF profile
     * directory (created if missing). [initialUrl] is loaded immediately.
     */
    @JvmStatic
    external fun nativeCreate(
        runtimeDir: String,
        cacheDir: String,
        userAgent: String?,
        initScript: String?,
        jsBridgeScript: String?,
        incognito: Boolean,
        enableDevtools: Boolean,
        javascriptEnabled: Boolean,
        zoomLevel: Double,
        transparent: Boolean,
        bgR: Float,
        bgG: Float,
        bgB: Float,
        bgA: Float,
        initialUrl: String?,
    ): Long

    @JvmStatic
    external fun nativeGetGtkWidget(handle: Long): Long

    @JvmStatic
    external fun nativeIsReady(handle: Long): Boolean

    @JvmStatic
    external fun nativeRelease(handle: Long)

    @JvmStatic
    external fun nativeLoadUrl(handle: Long, url: String)

    @JvmStatic
    external fun nativeLoadUrlWithHeaders(
        handle: Long,
        url: String,
        headerNames: Array<String>,
        headerValues: Array<String>,
    )

    @JvmStatic
    external fun nativeLoadHtml(handle: Long, html: String, baseUri: String?)

    @JvmStatic
    external fun nativeGoBack(handle: Long)

    @JvmStatic
    external fun nativeGoForward(handle: Long)

    @JvmStatic
    external fun nativeReload(handle: Long)

    @JvmStatic
    external fun nativeStopLoading(handle: Long)

    @JvmStatic
    external fun nativeCanGoBack(handle: Long): Boolean

    @JvmStatic
    external fun nativeCanGoForward(handle: Long): Boolean

    @JvmStatic
    external fun nativeCurrentUrl(handle: Long): String?

    @JvmStatic
    external fun nativeGetTitle(handle: Long): String?

    @JvmStatic
    external fun nativeIsLoading(handle: Long): Boolean

    @JvmStatic
    external fun nativeSetZoomLevel(handle: Long, zoom: Double)

    @JvmStatic
    external fun nativeFocus(handle: Long)

    @JvmStatic
    external fun nativeOpenDevTools(handle: Long)

    @JvmStatic
    external fun nativeCloseDevTools(handle: Long)

    @JvmStatic
    external fun nativeEvaluateJavaScript(handle: Long, script: String)

    @JvmStatic
    external fun nativeGetCookies(handle: Long, url: String)

    @JvmStatic
    external fun nativeSetCookie(
        handle: Long,
        name: String,
        value: String,
        domain: String?,
        path: String?,
        secure: Boolean,
        httpOnly: Boolean,
        expiresMs: Long,
        sameSite: String?,
    )

    @JvmStatic
    external fun nativeRemoveAllCookies(handle: Long)

    @JvmStatic
    external fun nativeRemoveCookiesForUrl(handle: Long, url: String)

    @JvmStatic
    external fun nativeCaptureScreenshot(handle: Long)

    @JvmStatic
    external fun nativeResize(handle: Long, widthPx: Int, heightPx: Int)

}
