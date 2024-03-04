!contains(ARCH_PATH, android_arm) {
    ARCH_PATH = x86
    contains(QMAKE_HOST.arch, x86_64) : ARCH_PATH=x86_64
    contains(ANDROID_TARGET_ARCH, armeabi-v7a)|contains(ANDROID_TARGET_ARCH, arm64-v8a) {
        ARCH_PATH = android_arm
    }
}

CONFIG_PATH = debug
CONFIG(debug, debug|release) {
    CONFIG_PATH = debug
} else {
    CONFIG_PATH = release
}

CONFIG(static)|CONFIG(staticlib)|qtConfig(static)|contains(STATIC_BUILD, true) {
    TYPE_PATH = static
} else {
    TYPE_PATH = shared
}
