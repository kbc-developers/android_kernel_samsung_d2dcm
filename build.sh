#!/bin/bash

# Bash Color
green='\033[01;32m'
red='\033[01;31m'
blink_red='\033[05;31m'
restore='\033[0m'

clear

# Device Config
TARGET_DEVICE=SC06D
KERNEL_DEFCONFIG="cyanogen_d2dcm_defconfig"
AK2_BRANCH="d2"
TOOLCHAIN_VER=arm-eabi-4.9

# Resources
THREAD="-j$(grep -c ^processor /proc/cpuinfo)"
KERNEL="zImage"
DTBIMAGE="dtb"
KERNEL_DIR=$PWD
BUILD_VERSION=`date +%Y%m%d`
TOOLCHAIN_DIR=${HOME}/toolchains

# Kernel Details
BASE_AK_VER="KBC"
VER="$BUILD_VERSION-MM"
AK_VER="$BASE_AK_VER-$TARGET_DEVICE-$VER"

# Vars
export LOCALVERSION=~`echo $AK_VER`
export CROSS_COMPILE="$TOOLCHAIN_DIR/$TOOLCHAIN_VER/bin/arm-eabi-"
export ARCH=arm
export SUBARCH=arm
export KBUILD_BUILD_USER=lawn
export KBUILD_BUILD_HOST=KBC


# Make Directory
mkdir -p out/$TARGET_DEVICE

# Paths
BIN_DIR=out/$TARGET_DEVICE/bin
OBJ_DIR=out/$TARGET_DEVICE/obj
ANYKERNEL_DIR="AnyKernel2"
PATCH_DIR="$ANYKERNEL_DIR/patch"
MODULES_DIR="$ANYKERNEL_DIR/modules"
ZIP_MOVE="$KERNEL_DIR/$BIN_DIR"
ZIMAGE_DIR="$OBJ_DIR"

# Functions
function clean_all {
                echo -e "${red}"
                echo ""
                echo "=====> CLEANING..."
                echo -e "${restore}"
		ccache -c -C
                if [ `find $BIN_DIR -type f | wc -l` -gt 0 ]; then
                 rm -rf $BIN_DIR/*
                fi
                mkdir -p $BIN_DIR
		rm -rf $MODULES_DIR/*
                if [ -d $ANYKERNEL_DIR ]; then
		  cd $ANYKERNEL_DIR
		  rm -rf $KERNEL
		  rm -rf $DTBIMAGE
		  git reset --hard > /dev/null 2>&1
		  git clean -f -d > /dev/null 2>&1
		  cd $KERNEL_DIR
                fi
		echo
		make clean && make mrproper
}

function get_ubertc {
                if [ ! -d $TOOLCHAIN_DIR/$TOOLCHAIN_VER ]; then
                  echo -e "${green}"
                  echo ""
                  echo "=====> Get ToolChain"
                  echo -e "${restore}"
                  mkdir -p ${HOME}/toolchains
                  cd $TOOLCHAIN_DIR
                  repo init -u git://github.com/lawnn/UBERTC.git -b master
                  repo sync
                  cd $KERNEL_DIR
                fi       
}

function get_anykernel2 {
                if [ ! -d $ANYKERNEL_DIR ]; then
                  echo -e "${green}"
                  echo ""
                  echo "=====> Get AnyKernel2"
                  echo -e "${restore}"
                  git clone -b $AK2_BRANCH git@github.com:lawnn/AnyKernel2.git
                fi
}

function make_kernel {
                echo -e "${green}"
                echo ""
                echo "=====> BUILDING..."
                echo -e "${restore}"
                cp -f ./arch/arm/configs/$KERNEL_DEFCONFIG $OBJ_DIR/.config
                make -C $PWD O=$OBJ_DIR oldconfig || exit -1
                if [ -e make.log ]; then
                  mv make.log make_old.log
                fi
                nice -n 10 make O=$OBJ_DIR -j12 2>&1 | tee make.log
		echo
		cp -vr $OBJ_DIR/arch/arm/boot/zImage $ANYKERNEL_DIR
}

function check_compile_error {
                COMPILE_ERROR=`grep 'error:' ./make.log`
                if [ "$COMPILE_ERROR" ]; then
                  echo ""
                  echo "=====> ERROR"
                  grep 'error:' ./make.log
                  exit -1
                fi
}

function make_modules {
                echo -e "${green}"
                echo ""
                echo "=====> INSTALL KERNEL MODULES"
                echo -e "${restore}"
		rm `echo $MODULES_DIR"/*"`
		find $KERNEL_DIR -name '*.ko' -exec cp -v {} $MODULES_DIR \;
}

function make_dtb {
                echo -e "${green}"
                echo ""
                echo "=====> make dtb"
                echo -e "${restore}"
		  $ANYKERNEL_DIR/tools/dtbToolCM -2 -o $ANYKERNEL_DIR/$DTBIMAGE -s 2048 -p scripts/dtc/ arch/arm/boot/
                
}

function make_zip {
                echo -e "${green}"
                echo ""
                echo "=====> make_recovery_image"
                echo -e "${restore}"
		cd $ANYKERNEL_DIR
		zip -r9 `echo $AK_VER`.zip *
		mv  `echo $AK_VER`.zip $ZIP_MOVE
		cd $KERNEL_DIR
}


DATE_START=$(date +"%s")

echo -e "${green}"
echo "KBC Kernel Creation Script:"
echo "    _____                         "
echo "   (, /  |              /)   ,    "
echo "     /---| __   _   __ (/_     __ "
echo "  ) /    |_/ (_(_(_/ (_/(___(_(_(_"
echo " ( /                              "
echo " _/                               "
echo

echo "---------------"
echo "Kernel Version:"
echo "---------------"

echo -e "${red}"; echo -e "${blink_red}"; echo "$AK_VER"; echo -e "${restore}";

echo -e "${green}"
echo "-----------------"
echo "Making KBC Kernel:"
echo "-----------------"
echo -e "${restore}"

while read -p "Do you want to clean stuffs (y/n)? " cchoice
do
case "$cchoice" in
	y|Y )
		clean_all
		echo
		echo "All Cleaned now."
		break
		;;
	n|N )
		break
		;;
	* )
		echo
		echo "Invalid try again!"
		echo
		;;
esac
done

echo

while read -p "Do you want to build kernel (y/n)? " dchoice
do
case "$dchoice" in
	y|Y)
                mkdir -p $BIN_DIR
                mkdir -p $OBJ_DIR
                get_ubertc
                get_anykernel2
		make_kernel
                check_compile_error
		make_dtb
		make_modules
		make_zip
		break
		;;
	n|N )
		break
		;;
	* )
		echo
		echo "Invalid try again!"
		echo
		;;
esac
done

echo -e "${green}"
echo "---------------------------------------------------------------------------"
echo "Build Completed in:"
echo "$ZIP_MOVE/$AK_VER"
echo "---------------------------------------------------------------------------"
echo -e "${restore}"

DATE_END=$(date +"%s")
DIFF=$(($DATE_END - $DATE_START))
echo "Time: $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds."
echo
