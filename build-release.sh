#!/bin/bash

KERNEL_DIR=$PWD

RAMDISK_NAME=sc06d_boot_ramdisk

if [ -z ../$RAMDISK_NAME ]; then
  echo "error: $RAMDISK_NAME directory not found"
  exit -1
fi

cd ../$RAMDISK_NAME
if [ ! -n "`git status | grep clean`" ]; then
  echo "error: $RAMDISK_NAME is not clean"
  exit -1
fi
git checkout aosp-jb
RAMDISK_DIR=$PWD

cd $KERNEL_DIR

read -p "select build type? [(r)elease/(n)ightly] " BUILD_TYPE
if [ "$BUILD_TYPE" = 'release' -o "$BUILD_TYPE" = 'r' ]; then
  export RELEASE_BUILD=y
else
  unset RELEASE_BUILD
fi

# create release dir＿
RELEASE_DIR=../release/`date +%Y%m%d`
if [ ! -d $RELEASE_DIR ]; then
  mkdir -p $RELEASE_DIR
fi

## build for samsung boot.img
#cd $KERNEL_DIR
#bash ./build-bootimg-samsung.sh a $1
#if [ $? != 0 ]; then
#  echo 'error: samsung boot.img build fail'
#  exit -1
#fi
#cp -v ./out/SAM/bin/SC06D* $RELEASE_DIR/


# build for aosp boot.img
cd $KERNEL_DIR
bash ./build-bootimg-aosp.sh a $1
if [ $? != 0 ]; then
  echo 'error: aosp boot.img build fail'
  exit -1
fi
cp -v ./out/AOSP/bin/SC06D* $RELEASE_DIR/

git log > $RELEASE_DIR/commitlog.txt

