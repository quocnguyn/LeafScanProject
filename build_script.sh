#!/usr/bin/bash

if [ -d build ]; then
    echo "Removing build directory"
    rm -rf build
fi

mkdir build
cd build
cmake ..
make