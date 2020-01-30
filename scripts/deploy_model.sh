#!/bin/bash

#
# This script copies the specified model csv file to sim server, generate secDb files
# and restart Viper if it instructed to.
#

echo

read -p "Which model do you want to deploy? (Type the model file name without .csv) " model
read -p "Is it a cross model? [y/n] " cross

if [ "${cross,,}" != "y" ]; then
  read -p "For which exchange? [ice/cme] " exchange
  if [ "$exchange" != "ice" ] && [ "$exchange" != "cme" ]; then
    echo "Invalid exchange. Valid exchanges are [ice, cme]"
    exit 1
  fi
else
  exchange=ice
fi

read -p "Restart Viper after deploy? [y/n] " restart

if [ "${restart,,}" == "y" ]; then
    viper=$(ssh pythonsvc@corlsd006 "readlink sim/bin/RhoneDs_Viper")
    read -p "Viper currently set to $viper, is this the right binary? [y/n] " right
    if [ "${right,,}" != "y" ]; then
        read -p "Which binary to run? " viper
    else
	viper=""
    fi
fi	

echo
if [ "${cross^}" == "Y" ]; then
  echo "Deploy model $model $exchange cross ..."
else
  echo "Deploy model $model $exchange ..."
fi

if [ "$viper" != "" ]; then
  echo "Viper: $viper"
fi

exit 0

ssh pythonsvc@corlsd006 "sim/scripts/deploy_model.sh ${model} ${exchange} ${cross} ${restart} $viper"
