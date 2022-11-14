#!/usr/bin/env bash

# building SVF
cd ./SVF
./build.sh
cd ../

# building COLLATE
rm -r ./build
mkdir ./build
