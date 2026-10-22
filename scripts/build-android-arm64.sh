NDK=$1
INSTALL_DIR=$2

if [ -z "${NDK}" ]; then
    if [ -z "${ANDROID_NDK_HOME}" ]; then
        NDK_VERSION="$(ls $HOME/Android/Sdk/ndk/ | tail -1)"
        if [ -z "${NDK_VERSION}" ]; then
            echo "ERROR: ANDROID_NDK_HOME is not set, and $HOME/Android/Sdk/ndk/ does not contain a valid NDK" >&2
            exit 1
        fi
        echo "Warning: ANDROID_NDK_HOME is not set, trying $HOME/Android/Sdk/ndk/$NDK_VERSION" >&2
        export ANDROID_NDK_HOME="$HOME/Android/Sdk/ndk/$NDK_VERSION"
    fi
else
    export ANDROID_NDK_HOME="$NDK"
fi

if [ ! -z "${INSTALL_DIR}" ]; then
    INSTALL_FLAG="-DCMAKE_INSTALL_PREFIX=$INSTALL_DIR"
fi

cmake -B build-android-arm64 \
    -DCMAKE_SYSTEM_NAME=Android \
    -DANDROID_PLATFORM=26 \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_TOOLCHAIN=clang \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
    -DCMAKE_WARN_DEPRECATED=OFF \
    $INSTALL_FLAG

cmake --build build-android-arm64 -j 8

if [ ! -z "${INSTALL_DIR}" ]; then
    cmake --install build-android-arm64
fi
