#!/bin/bash

make clean
export ARCH=arm
export SUBARCH=arm
export CROSS_COMPILE=arm-eabi-
make tuna_defconfig
make ARCH=arm CROSS_COMPILE=/home/marcusant/prebuilt/linux-x86/toolchain/arm-eabi-4.4.3/bin/arm-eabi- -j8
