#!/bin/bash

# SCREEN_NAMES are the names of detached screens
SCREEN_NAMES=("reader0" "reader1" "sysmon" "broker" "api")

# DIRS are the running directories of your processes
DIRS=("/home/coderre/ldaq"
      "/home/coderre/ldaq"
      "/home/coderre/ldaq/monitor"
      "/home/coderre/ldaq/broker"
      "/home/coderre/api")

# EXEC is the line to execute
EXEC=("gdb -ex run --args ./main 0 mongodb://dax:EedIcigjordIam3@ds263172.mlab.com:63172/dax"
      "gdb -ex run --args ./main 1 mongodb://dax:EedIcigjordIam3@ds263172.mlab.com:63172/dax"
      "python monitor.py"
      "broker.py --num=0 --config=options.ini"
      "python run.py")

if [[ ${#SCREEN_NAMES[@]} != ${#DIRS[@]} || ${#DIRS[@]} != ${#EXEC[@]} ]]
then
   echo "Configuration is bad, you need to have the same number of args for the input arrays."
   exit 1
fi
for ((i=0;i<${#SCREEN_NAMES[@]};i++)) #i in {0..${#SCREEN_NAMES[@]}}
do
    screen -X -S ${SCREEN_NAMES[i]} quit
    screen -S ${SCREEN_NAMES[i]} -d -m bash -c "cd ${DIRS[i]} && ${EXEC[i]}; exec bash"
    echo "Started screen ${SCREEN_NAMES[i]}"
done

exit 0

