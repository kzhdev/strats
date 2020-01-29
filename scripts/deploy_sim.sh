#!/bin/bash

echo ""
echo "********** Deploy Viper to Sim **********"
echo ""

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ "$1" != "" ]; then
    DEST=$1
else
    DEST="RhoneDs"
fi

echo "Copy RhoneDs to corlsd006:/home/pythonsvc/sim/bin/$1..."
scp $DIR/../bin/RhoneDs pythonsvc@corlsd006:/home/pythonsvc/sim/bin/$DEST

echo "Set current link"
ssh pythonsvc@corlsd006 "cd sim/bin;ln -sf ${DEST} RhoneDs_Viper;ln -sf ${DEST} RhoneDs_NonViper"

echo "Start Viper"

ssh pythonsvc@corlsd006 "sim/scripts/start_viper.sh"

echo "Done"
