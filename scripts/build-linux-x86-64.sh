INSTALL_DIR=$1

if [ ! -z "${INSTALL_DIR}" ]; then
    INSTALL_FLAG="-DCMAKE_INSTALL_PREFIX=$INSTALL_DIR"
fi

cmake -B build-linux-x86-64 \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_BUILD_TYPE=Release \
  $INSTALL_FLAG

cmake --build build-linux-x86-64

if [ ! -z "${INSTALL_DIR}" ]; then
    cmake --install build-linux-x86-64
fi
