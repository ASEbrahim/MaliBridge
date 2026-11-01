cmake -S host -B host/build
cmake --build host/build -j 8
cmake --build host/build --target quick-check layer-quick-check
