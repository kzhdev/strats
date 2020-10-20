#!/bin/bash
source scripts/common.sh

export CC=/opt/rh/devtoolset-9/root/bin/cc
export CXX=/opt/rh/devtoolset-9/root/bin/c++

echo `which gmake`

echo Script: $0
SCRIPT_NAME=$(basename $0)
SCRIPT_DIR=$(dirname $0)
PROJECT_ROOT=$(cd $SCRIPT_DIR; git rev-parse --show-toplevel)
RHONEPUBLIC_ROOT=$(realpath $PROJECT_ROOT/../rhonepublic)

unset TESTMODE
unset VERSION
unset COPY_TO_NAME

echo "Viper root: $PROJECT_ROOT"

function parseArgs() {
    if [ $# -gt 0 ]; then
        while [ $# -gt 0 ]; do
            if [[ $1 =~ ^-v ]]; then
                shift
                if [[ $# -eq 0 ]]; then
                    errorMsg "Usage: "
                    errorMsg "   $SCRIPT_NAME"
                    errorMsg "   $SCRIPT_NAME -v <version>"
                    exit 1
                fi
                VERSION="$1"
            elif [ $1 == "-t" ]; then
                TESTMODE=1
            else
                VERSION="$1"
            fi
            shift
        done
    fi
}

function getVersionInfo() {
#
# Ensures the following variables are set:
#
# GITHASH
# RHONEPUBLIC_GITHASH
# VERSION
# EXE_NAME
# TEMPLATE_DIR
#

    if ! GITHASH=$(cd $PROJECT_ROOT; git rev-parse HEAD 2>/dev/null) ; then
        errorMsg "Not a git repository ($PROJECT_ROOT)"
        GITHASH="dev"
        RHONEPUBLIC_GITHASH="dev"
        unset EXE_NAME
        if [ ! -v VERSION ]; then
            VERSION="dev"
        fi
        TEMPLATE_DIR=$PROJECT_ROOT/templates/$VERSION
        return
    fi

    if ! RHONEPUBLIC_GITHASH=$(cd $RHONEPUBLIC_ROOT; git rev-parse HEAD 2>/dev/null); then
        RHONEPUBLIC_GITHASH="unknown"
        errorMsg "rhonepublic not a git repository ($RHONEPUBLIC_ROOT)"
    fi

    # GITHASH and RHONEPUBLIC_GITHASH are now set

    if [ ! -v VERSION ]; then

        # No version specified from command line, try getting it from git tag

        if TAG=$(cd $PROJECT_ROOT; git describe --exact-match --tags HEAD 2>/dev/null); then
            if [[ $TAG =~ (test|release)/.+ ]]; then
                VERSION=$(echo $TAG | sed -e "s,release/,," -e "s,test/,test_,")
            else
                unset VERSION
                errorMsg "***************************************"
                errorMsg "There is a git tag ($TAG) but not of the format release/<version> or test/<version>"
                errorMsg "If you want to build, fix this or at least use the -v option."
                errorMsg "***************************************"
                eval false
            fi
        else
            warningMsg "No version specified via option -v, and git HEAD is not tagged.  Building 'dev' version."
            unset VERSION
        fi
    fi

    if [ -v VERSION ]; then
        EXE_NAME="RhoneDs_${VERSION}"
    else
        unset EXE_NAME
        VERSION="dev"
    fi

    TEMPLATE_DIR=$PROJECT_ROOT/templates/$VERSION
}


function main() {
    parseArgs $*
    if ! getVersionInfo; then
        eval false; return
    fi

    echo "Version:                                     $VERSION"
    echo "viper hash:                                  $GITHASH"
    echo "rhonepublic hash:                            $RHONEPUBLIC_GITHASH"
    echo "Git tag:                                     $TAG"
    echo "Template directory:                          $TEMPLATE_DIR"
    if [ ! -v EXE_NAME ]; then EXE_NAME=RhoneDs; fi
    echo "Produced exe:                                $EXE_NAME"
    echo

    cd $PROJECT_ROOT

    if [ ! -f scripts/.viper ]; then
        errorMsg "Must be run from viper repo root directory"
        echo; eval false; return
    fi

    GIT_STATUS=$(git status -s | wc -l)
    if [ ${GIT_STATUS} -ne 0 ]; then
        git status
        echo
        if ! askYN "Not in a clean git state, do you really want to proceed?"; then
            echo; eval false; return
        fi
        GITHASH="DIRTY:${GITHASH}"
    fi

    if [ -v RHONEPUBLIC_GITHASH ]; then
        RP_GIT_STATUS=$(cd $RHONEPUBLIC_ROOT; git status -s | wc -l)
        if [ ${RP_GIT_STATUS} -ne 0 ]; then
            git status
            echo
            if ! askYN "** RHONEPUBLIC ** Not in a clean git state, do you really want to proceed?"; then
                echo; eval false; return
            fi
            RHONEPUBLIC_GITHASH="DIRTY:${RHONEPUBLIC_GITHASH}"
        fi
    fi


    #
    # If TEMPLATE_DIR is specified,
    #
    if [ -v TEMPLATE_DIR ]; then
        mkdir -p ${TEMPLATE_DIR}
        TEMPLATE_FILE="${TEMPLATE_DIR}/templates.txt"

        ls -l ${TEMPLATES_DIR}
        if [ $VERSION != "dev" ] && [ -f $TEMPLATE_FILE ]; then
            if ! askYN "$TEMPLATE_FILE exists - clobber? "; then
                errorMsg "Aborting"
                echo; eval false; return
            fi
        fi
        rm -f $TEMPLATE_FILE
        python scripts/generateTemplates.py $TEMPLATE_FILE
    fi

    #
    # Build in 'viper/prod-build'
    #

    (
        rm -rf prod-build
        mkdir prod-build
        rm -rf bin/*

        cd prod-build
        echo "========================================================"
        echo "Building in $PWD..."
        echo "========================================================"
        if [ -v TESTMODE  ]; then
            # Fake the build
            touch ../bin/${EXE_NAME}
        else
            cmake -DCMAKE_BUILD_TYPE=Release \
                -DVIPER_GITHASH=${GITHASH} \
                -DVIPER_VERSION=${VERSION} \
                -DRHONEPUBLIC_GITHASH=${RHONEPUBLIC_GITHASH} \
                ..
            # make -j$(expr $(grep -c ^processor /proc/cpuinfo) / 2)
            cmake --build . --config Release --target ${EXE_NAME}_${VERSION} -j$(expr $(grep -c ^processor /proc/cpuinfo) / 2)
        fi
    )

    if [ $GIT_STATUS -eq 0 ] || askYN "Do you want to reset src/*? "; then
        git checkout -- src/
    fi

    if [ ! -f bin/${EXE_NAME} ]; then
        errorMsg "Did not produce target bin/${EXE_NAME} !"
        echo; echo; eval false; return
    fi
}


main $*
