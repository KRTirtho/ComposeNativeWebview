import org.apache.tools.ant.taskdefs.condition.Os

plugins {
    alias(libs.plugins.kotlinMultiplatform)
    alias(libs.plugins.composeMultiplatform)
    alias(libs.plugins.composeCompiler)
    alias(libs.plugins.nucleus)
}

kotlin {
    jvm()

    sourceSets {
        jvmMain.dependencies {
            implementation(compose.desktop.currentOs)
            implementation(compose.material3)
            implementation(libs.kotlinx.coroutinesSwing)
            implementation(project(":webview-compose"))
            // Tao backend required for desktop WebView (NativeView / WebKit2GTK).
            implementation(libs.nucleus.application)
            implementation(libs.nucleus.decorated.window.tao)
            implementation(libs.nucleus.core.runtime)
        }
    }
}

nucleus.application {
    mainClass = "dev.nucleusframework.webview.demo.MainKt"
}

// Compose Hot Reload's agent starts AWT before main(), and JBR's X11 toolkit pins
// GTK to XWayland, where the embedded WebKitWebView renders black. Tao owns the
// window and never needs AWT, so the reloadable app JVM stays headless (the
// DevTools window runs in its own JVM and is unaffected).
if (Os.isFamily(Os.FAMILY_UNIX) && !Os.isFamily(Os.FAMILY_MAC)) {
    tasks.withType<JavaExec>().matching { it.name.startsWith("hotRun") }.configureEach {
        jvmArgs("-Djava.awt.headless=true")
    }
}

// The host-OS native WebView backend is gitignored — build it before run if missing.
tasks.matching { it.name == "run" || it.name == "jvmRun" }.configureEach {
    when {
        Os.isFamily(Os.FAMILY_WINDOWS) ->
            dependsOn(":webview-compose:buildNativeWindows")
        Os.isFamily(Os.FAMILY_MAC) ->
            dependsOn(":webview-compose:buildNativeMacos")
        Os.isFamily(Os.FAMILY_UNIX) ->
            dependsOn(":webview-compose:buildNativeLinux")
    }
}
