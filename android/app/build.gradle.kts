plugins {
    id("com.android.application")
}

// The release workflow passes the firmware version (-PappVersion=2.0.0); local builds are 0.0.0.
val appVersion = (findProperty("appVersion") as String?) ?: "0.0.0"

// 2.0.0 -> 20000: Android only installs an update with a higher versionCode.
val appVersionCode = appVersion.substringBefore('-').split('.')
    .map { it.toIntOrNull() ?: 0 }
    .let { (it + listOf(0, 0, 0)).take(3) }
    .let { (major, minor, patch) -> major * 10000 + minor * 100 + patch }
    .coerceAtLeast(1)

// Release signing key from the environment (the release workflow writes it from the
// repository secrets, docs/firmware-switcher.md); without one, the debug key signs.
val keystore = System.getenv("FWSWITCH_KEYSTORE")?.takeIf { it.isNotEmpty() }

android {
    namespace = "io.github.daafip.fwswitch"
    compileSdk = 36

    defaultConfig {
        applicationId = "io.github.daafip.fwswitch"
        minSdk = 29
        targetSdk = 36
        versionCode = appVersionCode
        versionName = appVersion
    }

    signingConfigs {
        if (keystore != null) {
            create("release") {
                storeFile = file(keystore)
                storePassword = System.getenv("FWSWITCH_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("FWSWITCH_KEY_ALIAS")
                keyPassword = System.getenv("FWSWITCH_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName(if (keystore != null) "release" else "debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
