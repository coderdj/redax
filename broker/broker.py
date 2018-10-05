import time
import configparser
import argparse
import socket

from DBInterface import DBInterface
from DAQBroker import DAQBroker

# Parse command line
parser = argparse.ArgumentParser(description='Manage the DAQ')
parser.add_argument('--config', type=str, help='Path to your configuration file')
parser.add_argument('--num', type=str, help='Unique ID for this host (int)')
args = parser.parse_args()
config = configparser.ConfigParser()
config.read(args.config)
HOSTNAME = socket.gethostname() + "_broker_" + args.num

# Declare database object
DBInt = DBInterface(config, HOSTNAME)
DBroke = DAQBroker(DBInt)

'''

broker

monitor command DB

monitor DAQ global state

implement commands


'''
state_codes = ["IDLE", "ARMING", "ARMED", "RUNNING", "ERROR", "TIMEOUT", "UNDECIDED"]
pending_commands = []

while(1):

    # Get most recent check-in from all connected hosts
    HostStatus = DBInt.GetHostStatus()
    
    # We only check for state changes once our current command queue
    # is exhausted. If the commands time out we send a reset command
    if len(pending_commands) == 0:
        desired_state = DBInt.GetState()
        pending_commands = DBroke.Update(desired_state, HostStatus)

    # Process one command per iteration
    if len(pending_commands) > 0:
        DBInt.SendCommand(pending_commands[0])
        print("COMMAND IN: %s"%pending_commands[0]['command'])
        print(pending_commands[0])
        print("\n")
        pending_commands.pop(0)
            

    # Update global status
    DAQStatus = DBroke.GetStatus()
    for det, doc in DAQStatus.items():
        s = 'active'
        if doc['active'] != 'true':
            s = 'inactive'
        print("Detector %s in %s state (%s/%s)"%(det, doc['diagnosis'],
                                                 state_codes[doc['status']], s))

    astat = DBroke.GetAggregateStatus()
    if len(astat) > 0:
        DBInt.InsertAggregateStatus(astat)
    # print(DAQStatus)

    time.sleep(1)
