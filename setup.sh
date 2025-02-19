#!/bin/sh

# Remove kernel modules
rmmod pcidma
rmmod dune

# Set huge pages
sudo sh -c 'for i in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages; do echo 8192 > $i; done'

# Unbind NICs
sudo ./deps/dpdk/tools/dpdk_nic_bind.py --force -u 18:00.1

# Build required kernel modules.
make -sj64 -C deps/dune
make -sj64 -C deps/pcidma
make -sj64 -C deps/dpdk config T=x86_64-native-linuxapp-gcc
cd deps/dpdk
    git apply ../dpdk.mk.patch
    git apply ../dpdk.vars.patch
cd ../../
make -sj64 -C deps/dpdk

# Insert kernel modules
sudo insmod deps/dune/kern/dune.ko
sudo insmod deps/pcidma/pcidma.ko
