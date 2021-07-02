#!/bin/bash

VERSION=$1

if [ "$VERSION" == "" ]; then
  read -p "Please specify Viper versoin: " VERSION
fi

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

shopt -s lastpipe
tag=$( git tag -l | grep "release/${VERSION}" )

if [ "$tag" == "" ]; then
  echo "Tag release/$VERSION doesn't exist"
  exit 1
fi

if ssh -x pythonsvc@corlsp003 [ ! -f "/home/pythonsvc/releases/RhoneDs_$VERSION" ]; then
  echo "Release RhoneDs_$VERSION to logging server ..."
  cd ..
  $DIR/deploy.sh "$VERSION"
  if [ $? -ne 0 ]; then
    exit 1
  fi
  cd $DIR || return
fi
