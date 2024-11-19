#!/bin/sh
sudo apt-get install libconfig-dev libnuma-dev -y

# Remove kernel modules
rmmod pcidma
rmmod dune

# Set huge pages
sudo sh -c 'for i in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages; do echo 8192 > $i; done'

# Unbind NICs
sudo ./deps/dpdk/tools/dpdk_nic_bind.py -u --force 0000:18:00.1

# Build required kernel modules.
make -s -j16 -C deps/dune
make -s -j16 -C deps/pcidma
make -s -j16 -C deps/dpdk config T=x86_64-native-linuxapp-gcc
cd deps/dpdk
    git apply ../dpdk.mk.patch
    git apply ../dpdk_i40e.patch
    git apply ../dpdk.vars.patch
cd ../../
make -s -j16 -C deps/dpdk

# Insert kernel modules
sudo insmod deps/dune/kern/dune.ko
sudo insmod deps/pcidma/pcidma.ko
