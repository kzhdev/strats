#!/bin/bash

echo ""
echo "********** Deploy Viper to Sim **********"
echo ""

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

SOURCE="RhoneDs"

if [ ! -f "$DIR/../bin/$SOURCE" ] && [ "$1" != "" ] && [ -f "$DIR/../bin/$1" ]; then
  SOURCE=$1
fi

while [ ! -f "$DIR/../bin/$SOURCE" ]; do
  # RhonesDs does not exit, it might be release build
  read -p "$SOURCE does not exit. Which binary to deploy? " SOURCE
done

if [ "$1" != "" ]; then
    DEST=$1
else
    DEST=$SOURCE
fi

ssh pythonsvc@corlsd006 "source /home/pythonsvc/sim/scripts/common.sh;killAllRhone"

echo "Copy $SOURCE to corlsd006:/home/pythonsvc/sim/bin/$1..."
scp $DIR/../bin/$SOURCE pythonsvc@corlsd006:/home/pythonsvc/sim/bin/$DEST

echo "Set current link"
ssh pythonsvc@corlsd006 "cd sim/bin;ln -sf ${DEST} RhoneDs_Viper;ln -sf ${DEST} RhoneDs_NonViper"

echo "Start SIM"

ssh pythonsvc@corlsd006 "sim/scripts/start_sim.sh"

