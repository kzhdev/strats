#!/bin/bash
source util/common.sh

echo Script name: $0
echo $# arguments

function main() {

    if [ $# -ne 1 ] 
    then 
         errorMsg "Missing: TEMPLATES_DIR"
         echo; eval false; return
    fi

    if [ ! -f .this_is_viper_root ]; then
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
    fi


    TEMPLATE=$1

    #
    # build in 'viper/prod-build'
    # write template to 'viper/templates/$TEMPLATE'
    #

    rm -rf prod-build
    mkdir prod-build
    mkdir -p templates/${TEMPLATE}
    TEMPLATE_FILE="templates/${TEMPLATE}/templates.txt"

    if [ -f $TEMPLATE_FILE ]; then
        if ! askYN "$TEMPLATE_FILE exists - clobber? "; then
            errorMsg "Aborting"
            echo; eval false; return
        fi
    fi
    rm -rf $TEMPLATE_FILE
    rm -rf bin/*

    python util/generateTemplates.py $TEMPLATE_FILE

    (
        cd prod-build
        echo "========================================================"
        echo "Building in $CWD"
        echo "========================================================"
        cmake -DCMAKE_BUILD_TYPE=Release ..
        make -j16
    )

    if [ $GIT_STATUS -eq 0 ] || askYN "Do you want to reset? "; then
        git checkout -- .
    fi

    if [ ! -f bin/RhoneDs ]; then
        errorMsg "Did not produce target bin/RhoneDs!"
        echo; echo; eval false; return
    else
        mv bin/RhoneDs bin/RhoneDs_1.$TEMPLATE
    fi
}

main $*
