#!/bin/bash

########################################################################
# Copyright (C) 2011 CEGO ApS                                          #
# Written by Robert Larsen <robert@komogvind.dk> for CEGO ApS          #
#                                                                      #
# This program is free software: you can redistribute it and/or modify #
# it under the terms of the GNU General Public License as published by #
# the Free Software Foundation, either version 3 of the License, or    #
# (at your option) any later version.                                  #
#                                                                      #
# This program is distributed in the hope that it will be useful,      #
# but WITHOUT ANY WARRANTY; without even the implied warranty of       #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        #
# GNU General Public License for more details.                         #
#                                                                      #
# You should have received a copy of the GNU General Public License    #
# along with this program.  If not, see <http://www.gnu.org/licenses/>.#
########################################################################

# "Host;Pings;Total;Max;Min"
declare -a HOSTS
SUPERPING="./superping"
NUM_PINGS=100

function add_host(){
    HOSTS[${#HOSTS[*]}]="$1;0;0;0;10000000"
}

function print_host(){
    host=$(echo "$1"|awk -F ';' '{print $1}')
    pings=$(echo "$1"|awk -F ';' '{print $2}')
    total=$(echo "$1"|awk -F ';' '{print $3}')
    max=$(echo "$1"|awk -F ';' '{print $4}')
    min=$(echo "$1"|awk -F ';' '{print $5}')
    if [ "$pings" != "0" ]; then
        avg=$((total/pings))
    else
        avg="0"
    fi

    echo "$host - Successfull pings: $pings Avg: $avg Max: $max Min: $min"
}

function print_hosts(){
    for ((i=0; $i<${#HOSTS[*]}; i=$((i+1)) )); do
        print_host ${HOSTS[$i]}
    done
}

function ping_host(){
    host=$(echo "$1"|awk -F ';' '{print $1}')
    pings=$(echo "$1"|awk -F ';' '{print $2}')
    total=$(echo "$1"|awk -F ';' '{print $3}')
    max=$(echo "$1"|awk -F ';' '{print $4}')
    min=$(echo "$1"|awk -F ';' '{print $5}')

    ping_time=$($SUPERPING -p $host)
    if [ "$?" -eq "0" ]; then
        pings=$((pings+1))
        total=$((total+ping_time))
        if test $max -lt $ping_time; then
            max=$ping_time
        fi
        if test $min -gt $ping_time; then
            min=$ping_time
        fi
    fi
    echo "$host;$pings;$total;$max;$min"
}

function ping_hosts(){
    for ((i=0; $i<${#HOSTS[*]}; i=$((i+1)) )); do
        host_info=${HOSTS[$i]}
        HOSTS[$i]=$(ping_host $host_info)
    done
}

function make_rounds(){
    for ((count=0; $count<$NUM_PINGS; count=$((count+1)) )); do
        ping_hosts
    done
}

#First add hosts
while ! test -z $1; do
    add_host $1
    shift
done

make_rounds
print_hosts
