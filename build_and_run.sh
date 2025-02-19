#!/bin/sh

rm ./dp/concord
make clean
make -sj64 USE_CI=1
mv ./dp/shinjuku ./dp/concord
./dp/concord
