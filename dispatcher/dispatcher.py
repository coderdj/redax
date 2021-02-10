import configparser
import argparse
import logging
import logging.handlers
import threading
import signal
import datetime
import os

from MongoConnect import MongoConnect
from DAQController import DAQController, STATUS


class SignalHandler(object):
    def __init__(self):
        self.event = threading.Event()
        signal.signal(signal.SIGINT, self.interrupt)
        signal.signal(signal.SIGTERM, self.interrupt)

    def interrupt(self, *args):
        self.event.set()

class LogHandler(logging.Handler):
    def __init__(self, logdir='/live_data/redax_logs/', retention=0):
        logging.Handler.__init__(self)
        self.today = datetime.date.today()
        self.logdir = logdir
        self.retention = retention
        self.Rotate(self.today)
        self.count = 0

    def close(self):
        if not self.f.closed:
            self.f.flush()
            self.f.close()

    def __del__(self):
        self.close()

    def emit(self, record):
        msg_today = datetime.date.fromtimestamp(record.created)
        msg_datetime = datetime.datetime.fromtimestamp(record.created)
        if msg_today != self.today:
            self.Rotate(msg_today)
        m = self.FormattedMessage(msg_datetime, record.levelname, record.msg)
        self.f.write(m)
        print(m[:-1]) # strip \n
        self.count += 1
        if self.count > 2:
            self.f.flush()
            self.count = 0

    def Rotate(self, when):
        if hasattr(self, 'f'):
            self.f.close()
        self.f = open(self.FullFilename(when), 'a')
        if self.retention == 0:
            return
        last_file = when - datetime.timedelta(days=self.retention)
        if os.path.exists(self.FullFilename(last_file)):
            os.remove(self.FullFilename(last_file))
            m=self.FormattedMessage(datetime.datetime.utcnow(), "init", "Deleting " + self.Filename(last_file))
        else:
            m=self.FormattedMessage(datetime.datetime.utcnow(), "init", "No older file to delete :(")
        self.f.write(m)
        self.today = datetime.date.today()

    def FullFilename(self, when):
        return os.path.join(self.logdir, self.Filename(when))

    def Filename(self, when):
        return f"{when.year:04d}{when.month:02d}{when.day:02d}_dispatcher.log"

    def FormattedMessage(self, when, level, msg):
        return f"{when.isoformat()} | [{str(level).upper()}] | {msg}\n"


def main():

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
    logger.addHandler(LogHandler())
    logger.setLevel(getattr(logging, args.log))
    # Declare database object
    MongoConnector = MongoConnect(config, logger)

    # Declare a 'brain' object. This will cache info about the DAQ state in order to
    # solve the want/have decisions. The deciding functions also go here. It needs a
    # mongo connector because it has to pull options files
    DAQControl = DAQController(config, MongoConnector, logger)
    sleep_period = int(config['DEFAULT']['PollFrequency'])
    sh = SignalHandler()

    while(sh.event.is_set() == False):
        # Get most recent check-in from all connected hosts
        if MongoConnector.GetUpdate():
            continue
        latest_status = MongoConnector.latest_status

        # Get most recent goal state from database. Users will update this from the website.
        goal_state = MongoConnector.GetWantedState()
        if goal_state is None:
            continue

        # Print an update
        for detector in latest_status.keys():
            logger.debug("Detector %s should be %sACTIVE and is %s"%(
                    detector, '' if goal_state[detector]['active'] == 'true' else 'IN',
                    latest_status[detector]['status'].name))

        # Decision time. Are we actually in our goal state? If not what should we do?
        DAQControl.SolveProblem(latest_status, goal_state)

        # Time to report back
        MongoConnector.UpdateAggregateStatus()

        sh.event.wait(sleep_period)
    MongoConnector.Quit()
    return


if __name__ == '__main__':
    main()
