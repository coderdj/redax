#!/daq_common/miniconda3/bin/python3
import configparser
import argparse
import threading
import signal
import datetime
import os
from daqnt import get_daq_logger

from MongoConnect import MongoConnect
from DAQController import DAQController, STATUS


class SignalHandler(object):
    def __init__(self):
        self.event = threading.Event()
        signal.signal(signal.SIGINT, self.interrupt)
        signal.signal(signal.SIGTERM, self.interrupt)

    def interrupt(self, *args):
        self.event.set()

def main():

    # Parse command line
    parser = argparse.ArgumentParser(description='Manage the DAQ')
    parser.add_argument('--config', type=str, help='Path to your configuration file',
            default='config_test.ini')
    parser.add_argument('--log', type=str, help='Logging level', default='DEBUG',
            choices=['DEBUG','INFO','WARNING','ERROR','CRITICAL'])
    args = parser.parse_args()
    config = configparser.ConfigParser()
    config.read(args.config)
    logger = get_daq_logger(config['DEFAULT']['LogName'], level = args.log)
    # Declare database object
    MongoConnector = MongoConnect(config, logger)

    # Declare a 'brain' object. This will cache info about the DAQ state in order to
    # solve the want/have decisions. The deciding functions also go here. It needs a
    # mongo connector because it has to pull options files
    DAQControl = DAQController(config, MongoConnector, logger)
    sleep_period = int(config['DEFAULT']['PollFrequency'])
    sh = SignalHandler()

    while(sh.event.is_set() == False):
        # Get most recent goal state from database. Users will update this from the website.
        goal_state = MongoConnector.GetWantedState()
        if goal_state is None:
            continue
        # Get the Super-Detector configuration
        current_config = MongoConnector.GetSuperDetector(goal_state)
        # Get most recent check-in from all connected hosts
        if MongoConnector.GetUpdate(current_config):
            continue
        latest_status = MongoConnector.latest_status

        # Print an update
        for detector in latest_status.keys():
            state = 'ACTIVE' if goal_state[detector]['active'] == 'true' else 'INACTIVE'
            #linked_mode = 'ALL_LINKED' if goal_state['tpc']['link_mv'] == 'true' and goal_state['tpc']['link_nv'] == 'true' else 'NOLINK'
            if goal_state['tpc']['link_mv'] == 'true' and goal_state['tpc']['link_nv'] == 'true':
                linked_mode = 'ALL_LINKED'
            elif goal_state['tpc']['link_mv'] == 'false' and goal_state['tpc']['link_nv'] == 'false':
                linked_mode = 'NO_LINK'
            elif goal_state['tpc']['link_mv'] == 'true' and goal_state['tpc']['link_nv'] == 'false':
                linked_mode = 'MV_TPC_LINKED'
            elif goal_state['tpc']['link_mv'] == 'false' and goal_state['tpc']['link_nv'] == 'true':
                linked_mode = 'NV_TPC_LINKED'
            else:
                linked_mode = 'unclear'

            msg = (f'The {detector} should be {state} and is '
                   f'{latest_status[detector]["status"].name}'
                   f' and is in the linked mode {linked_mode}')
            if latest_status[detector]['number'] != -1:
                msg += f' ({latest_status[detector]["number"]})'
            logger.debug(msg)

        # Decision time. Are we actually in our goal state? If not what should we do?
        DAQControl.SolveProblem(latest_status, goal_state)

        # Time to report back
        MongoConnector.UpdateAggregateStatus()

        sh.event.wait(sleep_period)
    MongoConnector.Quit()
    return


if __name__ == '__main__':
    main()
