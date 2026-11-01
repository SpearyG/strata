#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
for source_file in src/*.cpp; do
    "${STRATA_ANALYZER:-clang++}" --analyze -std=c++26 -Iinclude \
        -Xanalyzer -analyzer-output=text -Xanalyzer -analyzer-werror "$source_file"
done
