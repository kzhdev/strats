#!/usr/bin/env bash
. /opt/rh/devtoolset-4/enable
SCRIPT_DIR=$(realpath $(dirname ${0}))
BIN_DIR=${SCRIPT_DIR}/bin
BUILD_DIR=${SCRIPT_DIR}/test/build
RC=0

####### Set default location of library to build against #############

# Take default RHONELIB_DIR from RhoneOM build
DEFAULT_RHONELIB_DIR=$(grep "DEFAULT_RHONELIB_DIR=" ${SCRIPT_DIR}/build.sh|cut -d'=' -f2|sed 's/"//g')

######################################################################



if [[ ${bamboo_RHONELIB} ]]; then
        if [[ -d ${bamboo_RHONELIB} ]]; then
            # If full path specified, take as is
            export RHONELIB_DIR=${bamboo_RHONELIB}
        else
            # If just version is specified, attempt to resole, checking for Production version first
            export RHONELIB_DIR=$(find /opt/rhone/.rhone/rhonecore -maxdepth 1 -mindepth 1 -type d|grep Production_${bamboo_RHONELIB}|sort|tail -1)
            if [[ -z ${RHONELIB_DIR} ]]; then
                export RHONELIB_DIR=$(find /opt/rhone/.rhone/rhonecore -maxdepth 1 -mindepth 1 -type d|grep _${bamboo_RHONELIB}|sort|tail -1)
            fi;
        fi;
        if [[ -z ${RHONELIB_DIR} ]]; then
            echo "[ERROR] Failed to resolve Bamboo RHONELIB=${bamboo_RHONELIB} to existing directory"
            exit 1
        fi
fi;

# Apply default where values were not specified through environment
[[ -z ${RHONELIB_DIR} ]] && export RHONELIB_DIR=${DEFAULT_RHONELIB_DIR} && echo "[INFO] Using default RHONELIB_DIR"

# Check to make sure RHONELIB_DIR exists
export RHONELIB_DIR=$(realpath ${RHONELIB_DIR})
if [[ ! -d ${RHONELIB_DIR} ]]; then
	echo "[ERROR] RhoneLib directory ${RHONELIB_DIR} does not exist!"
    exit 1
fi;

# 
echo "[INFO] RHONELIB_DIR=${RHONELIB_DIR}"

[[ -d ${BIN_DIR} ]] && rm -rf ${BIN_DIR}
[[ -d ${BUILD_DIR} ]] && rm -rf ${BUILD_DIR}

mkdir -p ${BUILD_DIR}
pushd ${BUILD_DIR}
cmake -DCMAKE_BUILD_TYPE=Debug --target RhoneDsTest ../
RC=$((RC+$?))
make -j`nproc`
RC=$((RC+$?))
popd
rm -rf ${BUILD_DIR}
exit ${RC}
