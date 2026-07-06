#!/bin/sh

compiler="afl-g++"
flags="COMPILER_IS_GCC_LIKE=yes"

if [ "$(uname)" = "Darwin" ]; then
    compiler="afl-clang++"
fi

echo "Building sync"

cd ../../
BARQ_ENABLE_ENCRYPTION=yes sh build.sh config
CXX="$compiler" BARQ_HAVE_CONFIG=yes make -j check-debug-norun "$flags"

echo "Building fuzz target"

cd -
CXX="$compiler" BARQ_HAVE_CONFIG=yes make -j check-debug-norun "$flags"

