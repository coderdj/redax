import time
import configparser
import argparse
import datetime

from MongoConnect import MongoConnect
from DAQController import DAQController

# Parse command line
parser = argparse.ArgumentParser(description='Manage the DAQ')
parser.add_argument('--config', type=str, help='Path to your configuration file')
args = parser.parse_args()
config = configparser.ConfigParser()
cfile = args.config
if cfile is None:
    cfile = 'config.ini'
config.read(cfile)

# Declare database object
MongoConnector = MongoConnect(config)
state_codes = ["IDLE", "ARMING", "ARMED", "RUNNING", "ERROR", "TIMEOUT", "UNDECIDED"]
pending_commands = []

# Declare a 'brain' object. This will cache info about the DAQ state in order to
# solve the want/have decisions. The deciding functions also go here. It needs a
# mongo connector because it has to pull options files
DAQControl = DAQController(config, MongoConnector)

while(1):

    # Get most recent check-in from all connected hosts
    MongoConnector.GetUpdate()
    latest_status = MongoConnector.latest_status

    # Get most recent goal state from database. Users will update this from the website.
    goal_state = MongoConnector.GetWantedState()
    

    # Decision time. Are we actually in our goal state? If not what should we do?
    DAQControl.SolveProblem(latest_status, goal_state)
    MongoConnector.ProcessCommands()

    # Time to report back
    MongoConnector.UpdateAggregateStatus()
    
    # Print an update
    print("Update %s"%datetime.datetime.utcnow())
    for detector in latest_status.keys():
        if goal_state[detector]['active'] == 'false':
            print("Detector %s INACTIVE"%detector)
            continue
        print("Detector %s should be ACTIVE and is %s"%(
            detector, state_codes[latest_status[detector]['status']]))    
    time.sleep(2)
