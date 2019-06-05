#!/bin/bash
#
# File:   protorun.sh
#

#LS="ls"
#LS_FLAGS="-al"

#$LS $LS_FLAGS $HOME

SRC_DIR="."
DST_DIR="."

#ls $SRC_DIR

protoc -I=$SRC_DIR --cpp_out=$DST_DIR $SRC_DIR/python.proto
