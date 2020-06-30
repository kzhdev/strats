#!/bin/bash

VERSION=$1

if [ "$VERSION" == "" ]; then
  read -p "Please specify Viper versoin: " VERSION
fi

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if ssh -x pythonsvc@corlsp003 [ ! -f "/home/pythonsvc/releases/RhoneDs_$VERSION" ]; then
  echo "Release RhoneDs_$VERSION to logging server ..."
  cd ..
  $DIR/deploy.sh "$VERSION"
  if [ $? -ne 0 ]; then
    exit 1
  fi
  cd $DIR || return
fi

function choose_server() {
  declare -A servers
  servers[BACK_CRUDE]="icelsp023"
  servers[BRENT]="icelsp035"
  servers[BTC]="cmelsp042"
  servers[FED_FUNDS]="cmelsp044"
  servers[HOGO_ARB]="icelsp020"
  servers[HOGO_TERM]="cmelsp040"
  servers[US_RATE]="cmelsp043"
  servers[WTI]="cmelsp034"

  MAX_IDX=${#servers[@]}
  IDX=1
  for server in "${!servers[@]}"; do
    echo "  $IDX. $server"
    IDX=$((IDX+1))
  done

  IDX=0
  while [ $IDX -lt 1 ] || [ $IDX -gt $MAX_IDX ]; do
    read -p "Which server [1-$MAX_IDX]: " IDX
    if [[ ! $IDX =~ ^[0-9]+$ ]]; then
      IDX=0
    fi
  done
  IDX=$((IDX-1))

  KEYS=("${!servers[@]}")
  SELECTED_SERVER=${servers[${KEYS[$IDX]}]}
}

choose_server

echo "Deploy RhoneDs_$VERSION to" ${KEYS[$IDX]} - $SELECTED_SERVER
ssh -x pythonsvc@$SELECTED_SERVER "
  if [ ! -f '/home/pythonsvc/RhoneDs_$VERSION' ]; then
    scp pythonsvc@corlsp003:/home/pythonsvc/releases/RhoneDs_$VERSION .
  fi
  if [ '`readlink /home/pythonsvc/RhoneDs`' != 'RhoneDs_$VERSION' ]; then
    ln -sf RhoneDs_$VERSION RhoneDs
    git add RhoneDs
    git commit -m 'Update to Viper $VERSION'
    git push
  fi
"
