#!/bin/bash
source scripts/common.sh

function main() {
    DESTHOST=corlsp003

    if [ $# -ne 1 ]; then
        errorMsg "Must specify a template ID to deploy"
        echo; eval false; return
    fi

    TEMPLATE=$1
    VERSION=$1
    RHONEEXE=RhoneDs_${VERSION}

    if [ ! -d templates/$TEMPLATE ]; then
        errorMsg "Template not valid(?).  Directory doesn't exist:  %blue%templates/$TEMPLATE%norm%"
        echo; eval false; return
    fi
    if [ ! -f bin/$RHONEEXE ]; then
        errorMsg "bin/RhoneDs_$VERSION not found."
        echo; eval false; return
    fi

    #
    # ssh command result is 1*A + 2*B
    # a=1 if releases/templates/## directory doesn't exist
    # b=2 if releases/RhoneDs_#.## doesn't exist
    #
    ssh -x pythonsvc@$DESTHOST -i $HOME/.ssh/id_rsa_pythonsvc "test ! -d releases/tempaltes/$TEMPLATE; A=\$?; test ! -f releases/$RHONEEXE; B=\$?; exit \$((A+B*2))"
    RES=$?
    TEMPLATE_EXISTS=$((RES%2))
    EXE_EXISTS=$((RES/2))
    local ASK
    echo "$RES, $TEMPLATE_EXISTS $EXE_EXISTS"
    if [ $TEMPLATE_EXISTS -ne 0 ]; then
        warningMsg "releases/templates/$TEMPLATE exists on $DESTHOST"
        ASK=Y
    fi
    if [ $EXE_EXISTS -ne 0 ]; then
        warningMsg "releases/$RHONEEXE already exists on $DESTHOST"
        ASK=Y
    fi

    if [ -v ASK ]; then
        if ! askYN "%blue%Do you want to overwrite?%norm%"; then
            echo; eval false; return
        fi
    fi

    if ssh -x pythonsvc@$DESTHOST -i $HOME/.ssh/id_rsa_pythonsvc "test -d tempaltes/$TEMPLATE"; then
        warningMsg "Template $TEMPLATE exists on $DESTHOST"
        if ! askYN "Do you want to %blue%overwrite%norm%?"; then
            echo; eval false; return
        fi
    fi

    scp -r -i $HOME/.ssh/id_rsa_pythonsvc \
        templates/$TEMPLATE \
        pythonsvc@$DESTHOST:releases/templates/$TEMPLATE/.
    scp -r -i $HOME/.ssh/id_rsa_pythonsvc \
        bin/$RHONEEXE \
        pythonsvc@$DESTHOST:releases/$RHONEEXE
}

main "$@"

