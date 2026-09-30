package dev.nucleusframework.webview.demo

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.rememberWindowState
import dev.nucleusframework.application.DecoratedWindow
import dev.nucleusframework.application.NucleusBackend
import dev.nucleusframework.application.nucleusApplication
import dev.nucleusframework.webview.web.WebView
import dev.nucleusframework.webview.web.rememberWebViewNavigator
import dev.nucleusframework.webview.web.rememberWebViewState
import dev.nucleusframework.window.TitleBar
import kotlin.system.exitProcess

private const val DEFAULT_URL = "https://accounts.google.com"

fun main() {
    nucleusApplication(backend = NucleusBackend.Tao) {
        val windowState = rememberWindowState(size = DpSize(1080.dp, 720.dp))
        DecoratedWindow(
            onCloseRequest = {
                exitApplication()
                exitProcess(0)
            },
            state = windowState,
            title = "ComposeNativeWebView Demo",
        ) {
            TitleBar()
            WebViewDemo()
        }
    }
}

@Composable
private fun WebViewDemo() {
    val navigator = rememberWebViewNavigator()
    val state = rememberWebViewState(DEFAULT_URL)
    var urlInput by remember { mutableStateOf(DEFAULT_URL) }

    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(8.dp)) {
            OutlinedTextField(
                value = urlInput,
                onValueChange = { urlInput = it },
                modifier = Modifier.weight(1f),
                singleLine = true,
                label = { Text("URL") },
            )
            Button(
                onClick = { navigator.loadUrl(urlInput) },
                modifier = Modifier.padding(start = 8.dp),
            ) {
                Text("Go")
            }
        }
        WebView(
            state = state,
            navigator = navigator,
            modifier = Modifier.fillMaxSize(),
        )
    }
}
