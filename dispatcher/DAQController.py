import datetime
import os
import json
import enum
'''
DAQ Controller Brain Class
D. Coderre, 12. Mar. 2019
D. Masson, 06 Apr 2020

Brief: This code handles the logic of what the dispatcher does when. It takes in 
aggregated status updates and commands from the mongo connector and decides if
any action needs to be taken to get the DAQ into the target state. It also handles the
resetting of runs (the ~hourly stop/start) during normal operations.
'''

class STATUS(enum.Enum):
    IDLE = 0
    ARMING = 1
    ARMED = 2
    RUNNING = 3
    ERROR = 4
    TIMEOUT = 5
    UNKNOWN = 6


class DAQController():

    def __init__(self, config, mongo_connector, log):

        self.mongo = mongo_connector
        self.goal_state = {}
        self.latest_status = {}

        # Timeouts. There are a few things that we want to wait for that might take time.
        # The keys for these dicts will be detector identifiers.
        detectors = list(json.loads(config['DEFAULT']['MasterDAQConfig']).keys())
        self.last_command = {}
        for k in ['arm', 'start', 'stop']:
            self.last_command[k] = {}
            for d in detectors:
                self.last_command[k][d] = datetime.datetime.utcnow()
        self.error_stop_count = {d : 0 for d in detectors}

        # Timeout properties come from config
        self.timeouts = {
                k.lower() : int(config['DEFAULT']['%sCommandTimeout' % k])
                for k in ['Arm','Start','Stop']}
        self.stop_retries = int(config['DEFAULT']['RetryReset'])

        self.log = log
        self.time_between_commands = int(config['DEFAULT']['TimeBetweenCommands'])
        self.can_force_stop={k:True for k in detectors}
        
        self.one_detector_arming = False

    def SolveProblem(self, latest_status, goal_state):
        '''
        This is sort of the whole thing that all the other code is supporting
        We get the status from the DAQ and the command from the user
        Then one of three things can happen:
             1) The status agrees with the command. We're in the goal state and happy.
             2) The status differs from the command. We issue the necessary commands
                to put the system into the goal state
             3) The status and goal are irreconcilable. We complain with an error
                because we can't find any way to put the system into the goal state.
                This could be because a key component is either in error or not present
                at all. Or it could be because we are trying to, for example, start a calibration
                run in the neutron veto but it is already running a combined run and
                therefore unavailable. The frontend should prevent many of these cases though.

        The way that works is this:
        A) the detector should be INACTIVE (i.e., IDLE), we stop the detector if the status is in one of the active states
        B) the detector should be ACTIVE (i.e, RUNNING), we issue the necessary commands to put the system in the RUNNING status
        C) we deal separately with the ERROR and TIMEOUT statuses, as in the first time we need to promptly stop the detector, and in the second case we need to handle the timeouts.
        '''

        # cache these so other functions can see them
        self.goal_state = goal_state
        self.latest_status = latest_status
        self.one_detector_arming = False

        for det in latest_status.keys():
            if latest_status[det]['status'] == STATUS.IDLE:
                self.can_force_stop[det] = True
                self.error_stop_count[det] = 0
            if latest_status[det]['status'] in [STATUS.ARMING, STATUS.ARMED]:
                self.one_detector_arming = True

        '''
        CASE 1: DETECTORS ARE INACTIVE (IDLE)
        'Inactive' means 'stopped'. An inactive detector is in its goal state as long as it isn't doing anything, i.e. it is in neither of the active_states.
        '''
        active_states = [STATUS.RUNNING, STATUS.ARMED, STATUS.ARMING, STATUS.UNKNOWN]

        for det in latest_status.keys():
            # The detector should be INACTIVE
            if goal_state[det]['active'] == 'false':
                # The detector is not in IDLE, ERROR or TIMEOUT: it needs to be stopped
                if latest_status[det]['status'] in active_states:
                    # Check before if the status is UNKNOWN and it is maybe timing out
                    if latest_status[det]['status'] == STATUS.UNKNOWN:
                        self.log.info(f"The status of {det} is unknown, check timeouts")
                        self.log.debug("Checking %s timeouts", det)
                        self.CheckTimeouts(detector=det, command='')
                    # Otherwise stop the detector
                    else:
                        self.log.info(f"Sending stop command to {det}")
                        self.StopDetectorGently(detector=det)
               # Deal separately with the TIMEOUT and ERROR statuses, by stopping the detector if needed
                elif latest_status[det]['status'] == STATUS.TIMEOUT:
                    self.log.info(f"The {det} is in timeout, check timeouts")
                    self.log.debug("Checking %s timeouts", det)
                    self.HandleTimeout(detector=det)

                elif latest_status[det]['status'] == STATUS.ERROR:
                   self.log.info(f"The {det} has error, sending stop command")
                   self.ControlDetector(command='stop', detector=det, force=self.can_force_stop[det])
                   self.can_force_stop[det]=False
            '''
                CASE 2: DETECTORS ARE ACTIVE (RUNNING)
                There are now 4 possibilities:
            	1. the detectors are already running, we check if the run needs to be reset, otherwise do nothing
            	2. the detectors are in some in-between state (i.e. ARMING, UNKNOWN), we check if they are timing out and wait for some seconds to allow time for the thing to sort itself out
            	3. the detector are not running (IDLE), we need to start them
            	4. the detectors are in some failed state (ERROR) or in TIMEOUT, we need to stop them
            '''
            # The detector should be ACTIVE (RUNNING)
            if goal_state[det]['active'] == 'true':
                if latest_status[det]['status'] == STATUS.RUNNING:
                    self.log.info(f"The {det} is running")
                    self.CheckRunTurnover(detector=det)
                # ARMED, start the run
                elif latest_status[det]['status'] == STATUS.ARMED:
                    self.log.info(f"The {det} is armed, sending start command")
                    self.ControlDetector(command='start', detector=det)
                # ARMING, check if it is timing out
                elif latest_status[det]['status'] == STATUS.ARMING:
                    self.log.info(f"The {det} is arming, check timeouts")
                    self.log.debug("Checking %s timeouts", det)
                    self.CheckTimeouts(detector=det, command='arm')
                # UNKNOWN, check if it is timing out
                elif latest_status[det]['status'] == STATUS.UNKNOWN:
                    self.log.info(f"The status of {det} is unknown, check timeouts")
                    self.log.debug("Checking %s timeouts", det)
                    self.CheckTimeouts(detector=det, command='')
                    
                # Maybe the detector is IDLE, we should arm a run
                elif latest_status[det]['status'] == STATUS.IDLE:
                    self.log.info(f"The {det} is in idle, sending arm command")
                    self.ControlDetector(command='arm', detector=det)

                # Deal separately with the TIMEOUT and ERROR statuses, by stopping the detector if needed
                elif latest_status[det]['status'] == STATUS.TIMEOUT:
                    self.log.info(f"The {det} is in timeout, check timeouts")
                    self.log.debug("Checking %s timeouts", det)
                    self.HandleTimeout(detector=det)
                
                elif latest_status[det]['status'] == STATUS.ERROR:
                    self.log.info(f"The {det} has error, sending stop command")
                    self.ControlDetector(command='stop', detector=det, force=self.can_force_stop[det])
                    self.can_force_stop[det]=False
                    
        return
    
    def HandleTimeout(self, detector):
        '''
        Detector already in the TIMEOUT status are directly stopped.
        '''
        self.ControlDetector(command='stop', detector=detector)
        
        return


    def StopDetectorGently(self, detector):
        '''
        Stops the detector, unless we're told to wait for the current
        run to end
        '''
        if (
                # Running normally (not arming, error, timeout, etc)
                self.latest_status[detector]['status'] == STATUS.RUNNING and
                # We were asked to wait for the current run to stop
                self.goal_state[detector].get('softstop', 'false') == 'true'):
            self.CheckRunTurnover(detector)
        else:
            self.ControlDetector(detector=detector, command='stop')

    def ControlDetector(self, command, detector, force=False):
        '''
        Issues the command to the detector if allowed by the timeout
        '''
        now = datetime.datetime.utcnow()
        try:
            dt = (now - self.last_command[command][detector]).total_seconds()
        except (KeyError, TypeError):
            dt = 2*self.timeouts[command]

        # make sure we don't rush things
        if command == 'start':
            dt_last = (now - self.last_command['arm'][detector]).total_seconds()
        elif command == 'arm':
            dt_last = (now - self.last_command['stop'][detector]).total_seconds()
        else:
            dt_last = self.time_between_commands*2

        if (dt > self.timeouts[command] and dt_last > self.time_between_commands) or force:
            run_mode = self.goal_state[detector]['mode']
            if command == 'arm':
                if self.one_detector_arming:
                    self.log.info('Another detector already arming, can\'t arm %s' % detector)
                    # this leads to run number overlaps
                    return
                readers, cc = self.mongo.GetHostsForMode(run_mode)
                hosts = (cc, readers)
                delay = 0
                self.one_detector_arming = True
            elif command == 'start':
                readers, cc = self.mongo.GetHostsForMode(run_mode)
                hosts = (readers, cc) # we want the cc to delay by 1s
                # we can safely short the logic here and buy an extra logic cycle
                self.one_detector_arming = False
                delay = 1.5
            else: # stop
                readers, cc = self.mongo.GetConfiguredNodes(detector,
                    self.goal_state['tpc']['link_mv'], self.goal_state['tpc']['link_nv'])
                hosts = (cc, readers)
                delay = 5 if not force else 0
                # TODO smart delay?
                if self.latest_status[detector]['status'] in [STATUS.ARMING, STATUS.ARMED]:
                    # this was the arming detector
                    self.one_detector_arming = False
            self.log.debug('Sending %s to %s' % (command.upper(), detector))
            if self.mongo.SendCommand(command, hosts, self.goal_state[detector]['user'],
                    detector, self.goal_state[detector]['mode'], delay):
                # failed
                return
            self.last_command[command][detector] = now
            if command == 'start' and self.mongo.InsertRunDoc(detector, self.goal_state):
                # db having a moment
                return
            if (command == 'stop' and 'number' in self.latest_status[detector] and 
                    self.mongo.SetStopTime(self.latest_status[detector]['number'], detector, force)):
                # db having a moment
                return

        else:
            self.log.debug('Can\'t send %s to %s, timeout at %i/%i' % (
                command, detector, dt, self.timeouts[command]))

    def CheckTimeouts(self, detector, command = None):
        ''' 
        This one is invoked if we think we need to change states. Either a stop command needs
        to be sent, or we've detected an anomaly and want to decide what to do. 
        Basically this function decides:
          - We are not in any timeouts: send the normal stop command
          - We are waiting for something: do nothing
          - We were waiting for something but it took too long: attempt reset
        '''

        sendstop = False
        nowtime = datetime.datetime.utcnow()

        if command is None: # not specified, we figure out it here
            command_times = [(cmd,doc[detector]) for cmd,doc in self.last_command.items()]
            command = sorted(command_times, key=lambda x : x[1])[-1][0]
            self.log.debug('Most recent command for %s is %s' % (detector, command))
        else:
            self.log.debug('Checking %s timeout for %s' % (command, detector))

        dt = (nowtime - self.last_command[command][detector]).total_seconds()

        local_timeouts = dict(self.timeouts.items())
        local_timeouts['stop'] = self.timeouts['stop']*(self.error_stop_count[detector]+1)

        if dt < local_timeouts[command]:
            self.log.debug('%i is within the %i second timeout for a %s command' %
                    (dt, local_timeouts[command], command))
        else:
            # timing out, maybe send stop?
            if command == 'stop':
                if self.error_stop_count[detector] >= self.stop_retries:
                    # failed too many times, issue error
                    self.mongo.LogError(
                                        ("Dispatcher control loop detects a timeout that STOP " +
                                         "can't solve"),
                                        'ERROR',
                                        "STOP_TIMEOUT")
                    self.error_stop_count[detector] = 0
                else:
                    self.ControlDetector(detector=detector, command='stop')
                    self.log.debug('Working on a stop counter for %s' % detector)
                    self.error_stop_count[detector] += 1
            else:
                self.mongo.LogError(
                        ('%s took more than %i seconds to %s, indicating a possible timeout or error' %
                            (detector, self.timeouts[command], command)),
                        'ERROR',
                        '%s_TIMEOUT' % command.upper())
                self.ControlDetector(detector=detector, command='stop')

        return


    def ThrowError(self):
        '''
        Throw a general error that the DAQ is stuck
        '''
        self.mongo.LogError(
                            "Dispatcher control loop can't get DAQ out of stuck state",
                            'ERROR',
                            "GENERAL_ERROR")

    def CheckRunTurnover(self, detector):
        '''
        During normal operation we want to run for a certain number of minutes, then
        automatically stop and restart the run. No biggie. We check the time here
        to see if it's something we have to do.
        '''
        # If no stop after configured, return
        try:
            _ = int(self.goal_state[detector]['stop_after'])
        except Exception as e:
            self.log.info('No run duration specified for %s? (%s)' % (detector, e))
            return

        try:
            number = self.latest_status[detector]['number']
        except:
            # dirty workaround just in case there was a dispatcher crash
            number = self.latest_status[detector]['number'] = self.mongo.GetNextRunNumber() - 1
            if number == -2:  # db issue
                return
        start_time = self.mongo.GetRunStart(number)
        if start_time is None:
            return
        nowtime = datetime.datetime.utcnow()
        run_length = int(self.goal_state[detector]['stop_after'])*60
        run_duration = (nowtime - start_time).total_seconds()
        self.log.debug('Checking run turnover for %s: %i/%i' % (detector, run_duration, run_length))
        if run_duration > run_length:
            self.log.info('Stopping run for %s' % detector)
            self.ControlDetector(detector=detector, command='stop')

