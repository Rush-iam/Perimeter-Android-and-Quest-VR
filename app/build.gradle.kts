import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
}

val perimeterVersion = Regex(
    "(?im)^\\s*project\\s*\\(\\s*perimeter\\s+VERSION\\s+([^\\s)]+)\\s*\\)"
).find(rootProject.file("Perimeter/CMakeLists.txt").readText())?.groupValues?.get(1)
    ?: error("Could not determine the Perimeter engine version")

android {
    namespace = "com.queststoredb.perimeter"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.queststoredb.perimeter"
        minSdk = 29
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"
        buildConfigField("String", "PERIMETER_VERSION", "\"$perimeterVersion\"")
        ndk {
            abiFilters.add("arm64-v8a")
        }
        @Suppress("UnstableApiUsage")
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_shared"
                arguments += "-DPERIMETER_ANDROID_XR=OFF"
                val androidStacktrace = providers.gradleProperty("androidStacktrace").orNull
                    ?.takeIf { it.isNotBlank() } ?: "ON"
                arguments += "-DPERIMETER_ANDROID_STACKTRACE=$androidStacktrace"
                val androidOptimization = providers.gradleProperty("androidOptimization").orNull
                    ?.takeIf { it.isNotBlank() } ?: "O2"
                arguments += "-DPERIMETER_ANDROID_OPTIMIZATION=$androidOptimization"
                val androidThinLto = providers.gradleProperty("androidThinLto").orNull
                    ?.takeIf { it.isNotBlank() } ?: "OFF"
                arguments += "-DPERIMETER_ANDROID_THIN_LTO=$androidThinLto"
                val compilerCacheMode = providers.gradleProperty("androidCompilerCache")
                    .orNull?.takeIf { it.isNotBlank() } ?: "AUTO"
                arguments += "-DANDROID_COMPILER_CACHE=$compilerCacheMode"
                val compilerCacheDir = providers.gradleProperty("androidCompilerCacheDir").orNull
                    ?.takeIf { it.isNotBlank() }
                    ?.let(rootProject::file)
                    ?: gradle.gradleUserHomeDir.resolve("caches/perimeter-android/compiler-cache")
                arguments += "-DANDROID_COMPILER_CACHE_DIR=${compilerCacheDir.invariantSeparatorsPath}"
                providers.gradleProperty("androidCompilerCacheExecutable").orNull
                    ?.takeIf { it.isNotBlank() }?.let {
                        arguments += "-DANDROID_COMPILER_CACHE_EXECUTABLE=${rootProject.file(it).invariantSeparatorsPath}"
                    }
                for ((property, variable) in mapOf(
                    "dxvkPython" to "ANDROID_DXVK_PYTHON",
                    "dxvkMeson" to "ANDROID_DXVK_MESON",
                    "dxvkGlslang" to "ANDROID_DXVK_GLSLANG",
                    "boostSource" to "FETCHCONTENT_SOURCE_DIR_BOOST_HEADERS",
                    "dxvk1Source" to "FETCHCONTENT_SOURCE_DIR_ANDROID_DXVK_SOURCE_V1",
                    "dxvk2Source" to "FETCHCONTENT_SOURCE_DIR_ANDROID_DXVK_SOURCE_V2",
                    "dxvkSdlSource" to "FETCHCONTENT_SOURCE_DIR_SDL2",
                    "dxvkSdlNetSource" to "FETCHCONTENT_SOURCE_DIR_SDL2_NET",
                    "dxvkSdlMixerSource" to "FETCHCONTENT_SOURCE_DIR_SDL2_MIXER",
                    "dxvkSdlImageSource" to "FETCHCONTENT_SOURCE_DIR_SDL2_IMAGE",
                    "ffmpegSource" to "FETCHCONTENT_SOURCE_DIR_FFMPEG_PREBUILT",
                    "mesonSource" to "FETCHCONTENT_SOURCE_DIR_MESON_SRC",
                    "glslangSource" to "FETCHCONTENT_SOURCE_DIR_GLSLANG_BIN",
                    "simpleiniSource" to "FETCHCONTENT_SOURCE_DIR_SIMPLEINI",
                    "peventsSource" to "FETCHCONTENT_SOURCE_DIR_PEVENTS",
                    "gameMathSource" to "FETCHCONTENT_SOURCE_DIR_GAMEMATH",
                    "imguiSource" to "FETCHCONTENT_SOURCE_DIR_IMGUI",
                    "sokolSource" to "FETCHCONTENT_SOURCE_DIR_SOKOL"
                )) {
                    providers.gradleProperty(property).orNull?.takeIf { it.isNotBlank() }?.let {
                        arguments += "-D$variable=${rootProject.file(it).invariantSeparatorsPath}"
                    }
                }
            }
        }
    }

    signingConfigs {
        create("release") {
            val keystoreProperties = Properties()
            val keystorePropertiesFile = rootProject.file("local.properties")
            if (keystorePropertiesFile.exists()) {
                keystorePropertiesFile.inputStream().use(keystoreProperties::load)
            }
            storeFile = keystoreProperties.getProperty("RELEASE_STORE_FILE")?.let(rootProject::file)
            storePassword = keystoreProperties.getProperty("RELEASE_STORE_PASSWORD")
            keyAlias = keystoreProperties.getProperty("RELEASE_KEY_ALIAS")
            keyPassword = keystoreProperties.getProperty("RELEASE_KEY_PASSWORD")
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("release")
            optimization {
                enable = true
            }
        }
        create("releaseBenchmark") {
            initWith(getByName("release"))
            signingConfig = signingConfigs.getByName("debug")
            applicationIdSuffix = ".debug"
            resValue("string", "app_name", "Perimeter Dev")
            isDebuggable = true
            matchingFallbacks += listOf("release")
        }
        debug {
            applicationIdSuffix = ".debug"
            resValue("string", "app_name", "Perimeter Dev")
        }
    }
    flavorDimensions += "device"
    productFlavors {
        create("mobile") {
            dimension = "device"
        }
        create("quest") {
            dimension = "device"
            applicationIdSuffix = ".quest"
            externalNativeBuild {
                cmake {
                    arguments += "-DPERIMETER_ANDROID_XR=ON"
                }
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    buildFeatures {
        buildConfig = true
        prefab = true
        resValues = true
        viewBinding = true
    }
}

dependencies {
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.constraintlayout)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.games.frame.pacing)
    implementation(libs.material)
    add("questImplementation", libs.openxr.loader.android)
}
