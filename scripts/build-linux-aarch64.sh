INSTALL_DIR=$1

if [ ! -z "${INSTALL_DIR}" ]; then
    INSTALL_FLAG="-DCMAKE_INSTALL_PREFIX=$INSTALL_DIR"
fi

cmake -B build-linux-aarch64 \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
  $INSTALL_FLAG

cmake --build build-linux-aarch64 -j 8

if [ ! -z "${INSTALL_DIR}" ]; then
    cmake --install build-linux-aarch64
fi
