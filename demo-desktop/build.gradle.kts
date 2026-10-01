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

// The host-OS native WebView backend is gitignored — build it before run if missing.
tasks.matching { it.name == "run" || it.name == "jvmRun" || it.name == "hotRunJvm" }.configureEach {
    when {
        Os.isFamily(Os.FAMILY_WINDOWS) ->
            dependsOn(":webview-compose:buildNativeWindows")
        Os.isFamily(Os.FAMILY_MAC) ->
            dependsOn(":webview-compose:buildNativeMacos")
        Os.isFamily(Os.FAMILY_UNIX) ->
            dependsOn(":webview-compose:buildNativeLinux", ":webview-compose:buildNativeLinuxCef")
    }
}
