# webview-compose

Compose Multiplatform WebView library exposing the `dev.nucleusframework.webview.*` API
(inspired by `compose-webview-multiplatform`).

## Usage

```kotlin
dependencies {
    implementation(project(":webview-compose"))
    // or: implementation("dev.nucleusframework:composewebview:<version>")
}
```

```kotlin
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import dev.nucleusframework.webview.web.WebView
import dev.nucleusframework.webview.web.rememberWebViewState

@Composable
fun App() {
    val state = rememberWebViewState("https://sample.com")
    WebView(state = state, modifier = Modifier.fillMaxSize())
}
```

## Platforms

- **Android**: `android.webkit.WebView`
- **iOS**: `WKWebView`
- **WasmJs**: `HTMLIFrameElement`
- **Desktop (JVM)**: Nucleus Tao `NativeView` — WKWebView (macOS), WebView2 (Windows).
  On **Linux** two engines are available: direct **CEF (Chromium)** and **WebKit2GTK**.

### Linux backend selection (CEF vs WebKit2GTK)

Linux defaults to the direct-**CEF (Chromium)** engine when its runtime is bundled
(built via `./gradlew :webview-compose:buildNativeLinuxCef`), and falls back to
**WebKit2GTK** automatically when the CEF runtime is not present. The CEF backend
is windowless (OSR) Chromium rendered into a GTK `DrawingArea`, with a native GTK
context menu offering Chromium essentials (Cut / Copy / Paste / Select All / Undo /
Redo / Copy Link / Reload …).

To force a specific engine, set `linuxBackend` on the desktop settings:

```kotlin
import dev.nucleusframework.webview.setting.LinuxWebBackend

val state = rememberWebViewState("https://sample.com")
state.webSettings.desktopWebSettings.linuxBackend = LinuxWebBackend.WEBKIT // or .CEF
WebView(state = state, modifier = Modifier.fillMaxSize())
```

CEF binaries are large (~1.3 GiB `libcef.so` uncompressed) and are **gitignored**;
build them locally with `./gradlew :webview-compose:buildNativeLinuxCef` (downloads the
official CEF SDK at build time). They are staged into
`src/jvmMain/resources/nucleus/native/linux-<arch>/` and packaged into the jar.
