#!/usr/bin/env bash
set -ev

pushd build
build_type=$(cmake -L .. | grep CMAKE_BUILD_TYPE | sed 's/CMAKE_BUILD_TYPE:STRING=//g')
coverage=$(cmake -L .. | grep COVERAGE | sed 's/COVERAGE:BOOL=//g')
popd

if [[ "$build_type" != "Coverage" ]]; then
    ./scripts/setup_build.sh "-DCMAKE_BUILD_TYPE=Coverage"
fi

pushd build
make MiniLua-tests-coverage $@
popd

