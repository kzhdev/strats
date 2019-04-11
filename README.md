Building Viper

scripts/build-viper.sh [options]

Usage:

./scripts/build-viper.sh
    1. check git HEAD for tag
        a. if tag like 'release/BLA' then version will be "BLA"
        b. if tag like 'test/BLA' then version will be "test_BLA"
    2. version will be "dev"


./scripts/build-viper.sh XYZ
./scripts/build-viper.sh -v XYZ
    Version will be XYZ

./scripts/build-viper.sh -t
    Just test this script, touches a file with the name a real build would produce.
    Note:  This WILL produce templates

Builds against rhonepublic version:  4079bc805e9c201ef5c2f5cd3448d97478639fbe
