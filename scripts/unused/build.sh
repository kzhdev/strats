#!/usr/bin/env bash
. /opt/rh/devtoolset-4/enable
SCRIPT_DIR=$(realpath $(dirname ${0}))
export BUILD_DIR=${SCRIPT_DIR}/build
[[ -d ${SCRIPT_DIR}/bin/ ]] && rm -rf ${SCRIPT_DIR}/bin/
RC=0

####### Set default location of library to build against #############

DEFAULT_ENVIRONMENT="PROD"
DEFAULT_RHONE_RELEASE="DEBUG RELEASE"
DEFAULT_RHONELIB_DIR="/opt/rhone/lib/31.3"

######################################################################

function run_build(){
    CMAKE_OPTS=$@
	echo "[INFO] CMAKE_OPTS=${CMAKE_OPTS}"
	[[ -d ${BUILD_DIR} ]] && rm -rf ${BUILD_DIR}
	mkdir -p ${BUILD_DIR}
	pushd ${BUILD_DIR}

	cmake ${CMAKE_OPTS} ../
	RC=$((RC+$?))
	make -j`nproc`
	RC=$((RC+$?))
	popd
	rm -rf ${BUILD_DIR}
}

if [[ ${bamboo_RHONELIB} ]]; then
        if [[ -d ${bamboo_RHONELIB} ]]; then
            # If full path specified, take as is
            export RHONELIB_DIR=${bamboo_RHONELIB}
        elif [[ -e /opt/rhone/lib/${bamboo_RHONELIB} ]]; then
            export RHONELIB_DIR=$(realpath /opt/rhone/lib/${bamboo_RHONELIB})
        fi;
        if [[ -z ${RHONELIB_DIR} ]]; then
            echo "[ERROR] Failed to resolve Bamboo RHONELIB=${bamboo_RHONELIB} to existing directory"
            exit 1
        fi
fi;

# Apply default where values were not specified through environment
[[ -z ${RHONELIB_DIR} ]] && export RHONELIB_DIR=${DEFAULT_RHONELIB_DIR} && echo "[INFO] Using default RHONELIB_DIR"
[[ -z ${ENVIRONMENT} ]] && export ENVIRONMENT=${DEFAULT_ENVIRONMENT} && echo "[INFO] Using default ENVIRONMENT"
[[ -z ${RHONE_RELEASE} ]] && export RHONE_RELEASE=${DEFAULT_RHONE_RELEASE} && echo "[INFO] Using default RHONE_RELEASE"

echo "[INFO] RHONELIB_DIR=${RHONELIB_DIR}"
echo "[INFO] ENVIRONMENT=${ENVIRONMENT}"
echo "[INFO] RELEASE=${RHONE_RELEASE}"

if [[ ${RHONE_RELEASE} =~ RELEASE ]]; then
    # Run RELEASE build
    echo "[INFO] Creating RELEASE binaries"
    run_build
fi;

if [[ ${RHONE_RELEASE} =~ DEBUG ]]; then
    # Run DEBUG build
    echo "[INFO] Creating DEBUG binaries"
    run_build "-DCMAKE_BUILD_TYPE=Debug"
fi;

exit ${RC}

