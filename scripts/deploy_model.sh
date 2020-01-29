#!/bin/bash

echo
echo "********** Deploy Model **********"
echo

read -p "Which model do you want to deploy? " model
read -p "For which exchange? [ice/cme] " exchange

if [ "$exchange" != "ice" ] && [ "$exchange" != "cme" ]; then
  echo "Invalid exchange. Valid exchanges are [ice, cme]"
  exit 1
fi

if [ "$exchange" == "ice" ]; then
  read -p "Is it a cross model? [y/n] " cross
else
  cross=n
fi

read -p "Restart Viper after deploy? [y/n] " restart

echo
if [ "${cross^}" == "Y" ]; then
  echo "Deploy model $model $exchange cross ..."
else
  echo "Deploy model $model $exchange ..."
fi

ssh pythonsvc@corlsd006 "sim/scripts/deploy_model.sh ${model} ${exchange} ${cross} ${restart}"