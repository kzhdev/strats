#!/usr/bin/env bash

export SHM_DIR=./shm

function log_warn(){
    echo -e "\e[33m[$(date +%H:%M:%S)][WARN]  $*\e[0m"
}

function log_info(){
    echo -e "[$(date +%H:%M:%S)][INFO]  $*\e[0m"
}

filename_filter="${1}"
project_id=73

if [[ -d ${SHM_DIR} ]]; then
    for shmseg in $(find ${SHM_DIR} -type f | egrep ".*${filename_filter}.*"); do
	    key=$(ftok ${shmseg} ${project_id})
	    if [[ ! -z ${key} ]]; then
		    shmseg_file=$(basename ${shmseg})
		    log_info "Removing shared memory segment ${shmseg_file} [${key}]"
		    ipcrm -M ${key}
		    RC=$((RC+$?))
		    rm -f ${shmseg}
		    [[ $? -ne 0 ]] && log_info "Failed to remove ${shmseg_file} [${key}]"
	    else
		    log_info "Couldn't resolve key id for ${shmseg}"
	    fi;
    done;
else
        log_warn "Shared memory segment directory not found in ${SHM_DIR}"
        RC=$((RC+1))
fi;

exit ${RC}
