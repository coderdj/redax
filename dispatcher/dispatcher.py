import time
import configparser
import argparse
import datetime
import logging
import logging.handlers

from MongoConnect import MongoConnect
from DAQController import DAQController

# Parse command line
parser = argparse.ArgumentParser(description='Manage the DAQ')
parser.add_argument('--config', type=str, help='Path to your configuration file',
        default='config.ini')
parser.add_argument('--log', type=str, help='Logging level', default='DEBUG',
        choices=['DEBUG','INFO','WARNING','ERROR','CRITICAL'])
args = parser.parse_args()
config = configparser.ConfigParser()
config.read(args.config)
logger = logging.getLogger('main')
f = logging.Formatter(fmt='%(asctime)s | %(levelname)s | %(message)s')
h = logging.StreamHandler()
h.setFormatter(f)
logger.addHandler(h)
h = logging.handlers.TimedRotatingFileHandler(
    datetime.date.today().isoformat()+'.log',when='midnight', utc=True,backupCount=7)
h.setFormatter(f)
logger.addHandler(h)
logger.setLevel(getattr(logging, args.log))
print("Config arm timeout: %i"%int(config["DEFAULT"]["ArmCommandTimeout"]))
# Declare database object
MongoConnector = MongoConnect(config, logger)
state_codes = ["IDLE", "ARMING", "ARMED", "RUNNING", "ERROR", "TIMEOUT", "UNDECIDED"]
pending_commands = []

# Declare a 'brain' object. This will cache info about the DAQ state in order to
# solve the want/have decisions. The deciding functions also go here. It needs a
# mongo connector because it has to pull options files
DAQControl = DAQController(config, MongoConnector, logger)

while(1):

    # Get most recent check-in from all connected hosts
    MongoConnector.GetUpdate()
    latest_status = MongoConnector.latest_status

    # Get most recent goal state from database. Users will update this from the website.
    goal_state = MongoConnector.GetWantedState()

    # Decision time. Are we actually in our goal state? If not what should we do?
    DAQControl.SolveProblem(latest_status, goal_state)
    #MongoConnector.ProcessCommands()

    # Time to report back
    MongoConnector.UpdateAggregateStatus()

    # Print an update
    for detector in latest_status.keys():
        if goal_state[detector]['active'] == 'false':
            logger.debug("Detector %s INACTIVE"%detector)
        else:
            logger.debug("Detector %s should be ACTIVE and is %s"%(
                detector, state_codes[latest_status[detector]['status']]))
    time.sleep(int(config["DEFAULT"]['PollFrequency']))
