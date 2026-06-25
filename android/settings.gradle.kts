pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}
plugins {
    id("org.gradle.toolchains.foojay-resolver-convention") version "0.10.0"
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        // Huawei maven, for the HMS Health Kit SDK (com.huawei.hms:health). Only
        // needed for the Huawei on-device health path; harmless on GMS-only builds.
        maven { url = uri("https://developer.huawei.com/repo/") }
    }
}

rootProject.name = "Mirobody"
include(":app")
