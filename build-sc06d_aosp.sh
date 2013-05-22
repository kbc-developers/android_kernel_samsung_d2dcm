#!/bin/bash

export BUILD_TARGET=AOSP
. sc06d.config

time ./_build-bootimg.sh $1
