plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "io.github.jmacheta.gtfog"
    compileSdk = 36

    defaultConfig {
        applicationId = "io.github.jmacheta.gtfog"
        minSdk = 26
        targetSdk = 36
        versionCode = 4
        versionName = "0.3.1"
    }
    // Release signing only when the keystore is given as Gradle properties (gtfog.storeFile, ...); never in the repo.
    val storeFile = providers.gradleProperty("gtfog.storeFile").orNull
    signingConfigs {
        if (storeFile != null) {
            create("release") {
                this.storeFile = file(storeFile)
                storePassword = providers.gradleProperty("gtfog.storePassword").get()
                keyAlias = providers.gradleProperty("gtfog.keyAlias").get()
                keyPassword = providers.gradleProperty("gtfog.keyPassword").get()
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.findByName("release")
        }
    }
    lint {
        abortOnError = true
        warningsAsErrors = false
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlin {
        compilerOptions {
            jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
        }
    }
}

dependencies {
    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.material3)
    implementation(libs.activity.compose)
    implementation(libs.lifecycle.runtime.compose)
    implementation(libs.core.ktx)
    implementation(libs.coroutines.android)
    implementation(libs.mcumgr.ble)
    testImplementation(libs.junit)
}
