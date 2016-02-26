#!/bin/bash

# Bash Color
green='\033[01;32m'
red='\033[01;31m'
blink_red='\033[05;31m'
restore='\033[0m'

# Path
TOOLCHAIN_DIR=${HOME}/toolchains
TOOLCHAIN_VER=arm-eabi-4.9
AK2_BRANCH="d2"
ANYKERNEL_DIR="AnyKernel2"
KERNEL_DIR=$PWD

# Functions
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
                if [ ! -d $KERNEL_DIR/AnyKernel2 ]; then
                  echo -e "${green}"
                  echo ""
                  echo "=====> Get AnyKernel2"
                  echo -e "${restore}"
		  cd $KERNEL_DIR
                  git clone -b $AK2_BRANCH git@github.com:lawnn/AnyKernel2.git
                fi
}

function update_ubertc {
                echo -e "${green}"
                echo ""
                echo "=====> Update ToolChain"
                echo -e "${restore}"
                cd $TOOLCHAIN_DIR
                repo sync
                cd $KERNEL_DIR
}

function fetch_anykernel2 {
                echo -e "${green}"
                echo ""
                echo "=====> Update AnyKernel2"
                echo -e "${restore}"
                if [ -d $KERNEL_DIR/AnyKernel2 ]; then
		  cd $ANYKERNEL_DIR
                  rm -rf $KERNEL
		  rm -rf $DTBIMAGE
		  git reset --hard > /dev/null 2>&1
                  git clean -f -d > /dev/null 2>&1
                  git fetch git@github.com:lawnn/AnyKernel2.git $AK2_BRANCH
                  git merge FETCH_HEAD
                  git add-A
                  git commit -a
                  cd $KERNEL_DIR
                fi
}

while read -p "Update Buildtool? (y/n)? " cchoice
do
case "$cchoice" in
	y|Y )
		get_ubertc
                get_anykernel2
                update_ubertc
                fetch_anykernel2
		echo
		echo "Update BuildTools."
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
