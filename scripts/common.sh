_NORM=$'\e[0m'
_RED=$'\e[31m'
_GREEN=$'\e[32m'
_YELLOW=$'\e[33m'
_BLUE=$'\e[34m'
_MAGENTA=$'\e[35m'
_CYAN=$'\e[36m'
_WHITE=$'\e[37m'

colorStr() {
    A="$*"
    A=${A//\%norm%/${_NORM}}
    A=${A//\%red%/${_RED}}
    A=${A//\%green%/${_GREEN}}
    A=${A//\%yellow%/${_YELLOW}}
    A=${A//\%blue%/${_BLUE}}
    A=${A//\%magenta%/${_MAGENTA}}
    A=${A//\%cyan%/${_CYAN}}
    A=${A//\%white%/${_WHITE}}
    echo $A
}

function errorMsg() {
    local MSG=$(colorStr "%red%${*}%norm%")
    echo $MSG >&2
}

function warnMsg() {
    local MSG=$(colorStr "%yellow%${*}%norm%")
    echo $MSG >&2
}
function warningMsg() { warnMsg "$*"; }

function askYN() {
    PROMPT=$(colorStr "$* [%green%y%norm%/%red%n%norm%]%norm%")
    read -n1 -p "$PROMPT" YN
    local YN=$(echo $YN | awk '{print(tolower($1))}')
    echo
    test "$YN" == 'y'
}
