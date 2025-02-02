#!/bin/sh

# Options to pass
# SCHEDULE_METHOD = < 0, 1, 2, 3 > | < pi, yield, none, concord >
# FAKE_WORK = <0,1>
# RUN_UBENCH = <0,1>
# -> (if run_ubench == 0) BENCHMARK_TYPE <0, 1, 2, 3, 4, 5>
# -> (if run_ubench == 1) BENCHMARK_TYPE <1>

LOAD_LEVEL=${1:-50}

rm -rf /tmpfs/experiments/leveldb/
sudo make clean 2> /dev/null
#make -j6 -s LOAD_LEVEL=$LOAD_LEVEL FAKE_WORK=1  2> /dev/null
sudo make -j6 -s LOAD_LEVEL=0 RUN_UBENCH=1 BENCHMARK_TYPE=1 DISPATCHER_DO_WORK=1 SCHEDULE_METHOD=3
rm ./dp/concord
mv ./dp/shinjuku ./dp/concord
sudo ./dp/concord
