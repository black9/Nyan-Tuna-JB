#!/bin/bash

make clean
export ARCH=arm
export SUBARCH=arm
export CROSS_COMPILE=arm-eabi-
make tuna_defconfig
make ARCH=arm CROSS_COMPILE=/home/marcusant/linaro/bin/arm-eabi- -j8
