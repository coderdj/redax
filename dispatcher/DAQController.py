import datetime
import json
import enum
import pytz
from daqnt import DAQ_STATUS
from .MongoConnect import NO_NEW_RUN

"""
DAQ Controller Brain Class
D. Coderre, 12. Mar. 2019
D. Masson, 06 Apr 2020
S. di Pede, 17 Mar 2021

Brief: This code handles the logic of what the dispatcher does when. It takes in 
aggregated status updates and commands from the mongo connector and decides if
any action needs to be taken to get the DAQ into the target state. It also handles the
resetting of runs (the ~hourly stop/start) during normal operations.
"""


def now():
    return datetime.datetime.now(pytz.utc)
    #return datetime.datetime.utcnow() # wrong?

class DAQController():

    def __init__(self, config, mongo_connector, log, hypervisor):

        self.mongo = mongo_connector
        self.hypervisor = hypervisor
        self.goal_state = {}
        self.latest_status = {}


        # Timeouts. There are a few things that we want to wait for that might take time.
        # The keys for these dicts will be detector identifiers.
        detectors = list(config['MasterDAQConfig'].keys())
        self.last_command = {}
        for k in ['arm', 'start', 'stop']:
            self.last_command[k] = {}
            for d in detectors:
                self.last_command[k][d] = now()
        self.error_stop_count = {d : 0 for d in detectors}
        self.max_arm_cycles = int(config['MaxArmCycles'])
        self.missed_arm_cycles={k:0 for k in config['MasterDAQConfig'].keys()}

        # Timeout properties come from config
        self.timeouts = {
                k.lower() : int(config['%sCommandTimeout' % k])
                for k in ['Arm','Start','Stop']}
        self.stop_retries = int(config['RetryReset'])

        self.log = log
        self.time_between_commands = int(config['TimeBetweenCommands'])
        self.can_force_stop={k:True for k in detectors}

        self.one_detector_arming = False
        self.start_cmd_delay = float(config['StartCmdDelay'])
        self.stop_cmd_delay = float(config['StopCmdDelay'])

    def solve_problem(self, latest_status, goal_state):
        """
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
        A) the detector should be INACTIVE (i.e., IDLE), we stop the detector 
            if the status is in one of the active states
        B) the detector should be ACTIVE (i.e, RUNNING), we issue the necessary 
            commands to put the system in the RUNNING status
        C) we deal separately with the ERROR and TIMEOUT statuses, as in the 
            first time we need to promptly stop the detector, and in the second 
            case we need to handle the timeouts.
        """
        # cache these so other functions can see them
        self.goal_state = goal_state
        self.latest_status = latest_status
        self.one_detector_arming = False

        for det in latest_status.keys():
            if latest_status[det]['status'] == DAQ_STATUS.IDLE:
                self.can_force_stop[det] = True
                self.error_stop_count[det] = 0
            if latest_status[det]['status'] in [DAQ_STATUS.ARMING, DAQ_STATUS.ARMED]:
                self.one_detector_arming = True

        active_states = [DAQ_STATUS.RUNNING, DAQ_STATUS.ARMED, DAQ_STATUS.ARMING, DAQ_STATUS.UNKNOWN]

        for det in latest_status.keys():
            # The detector should be INACTIVE
            if goal_state[det]['active'] == 'false':
                # The detector is not in IDLE, ERROR or TIMEOUT: it needs to be stopped
                if latest_status[det]['status'] in active_states:
                    # Check before if the status is UNKNOWN and it is maybe timing out
                    if latest_status[det]['status'] == DAQ_STATUS.UNKNOWN:
                        self.log.info(f"The status of {det} is unknown, check timeouts")
                        self.check_timeouts(detector=det)
                    # Otherwise stop the detector
                    else:
                        self.log.info(f"Sending stop command to {det}")
                        self.stop_detector_gently(detector=det)
                # Deal separately with the TIMEOUT and ERROR statuses, by stopping the detector if needed
                elif latest_status[det]['status'] == DAQ_STATUS.TIMEOUT:
                    self.log.info(f"The {det} is in timeout, check timeouts")
                    # TODO update
                    self.handle_timeout(detector=det)

                elif latest_status[det]['status'] == DAQ_STATUS.ERROR:
                   self.log.info(f"The {det} has error, sending stop command")
                   self.control_detector(command='stop', detector=det, force=self.can_force_stop[det])
                   self.can_force_stop[det]=False
                else:
                    # the only remaining option is 'idle', which is fine
                    pass

            # The detector should be ACTIVE (RUNNING)
            else: #goal_state[det]['active'] == 'true':
                if latest_status[det]['status'] == DAQ_STATUS.RUNNING:
                    self.log.info(f"The {det} is running")
                    self.check_run_turnover(detector=det)
                    # TODO does this work properly?
                    if latest_status[det]['mode'] != goal_state[det]['mode']:
                        self.control_detector(command='stop', detector=det)
                # ARMED, start the run
                elif latest_status[det]['status'] == DAQ_STATUS.ARMED:
                    self.log.info(f"The {det} is armed, sending start command")
                    self.control_detector(command='start', detector=det)
                # ARMING, check if it is timing out
                elif latest_status[det]['status'] == DAQ_STATUS.ARMING:
                    self.log.info(f"The {det} is arming, check timeouts")
                    self.log.debug(f"Checking the {det} timeouts")
                    self.check_timeouts(detector=det, command='arm')
                # UNKNOWN, check if it is timing out
                elif latest_status[det]['status'] == DAQ_STATUS.UNKNOWN:
                    self.log.info(f"The status of {det} is unknown, check timeouts")
                    self.log.debug(f"Checking the {det} timeouts")
                    self.check_timeouts(detector=det)

                # Maybe the detector is IDLE, we should arm a run
                elif latest_status[det]['status'] == DAQ_STATUS.IDLE:
                    self.log.info(f"The {det} is in idle, sending arm command")
                    self.control_detector(command='arm', detector=det)

                # Deal separately with the TIMEOUT and ERROR statuses, by stopping the detector if needed
                elif latest_status[det]['status'] == DAQ_STATUS.TIMEOUT:
                    self.log.info(f"The {det} is in timeout, check timeouts")
                    self.log.debug("Checking %s timeouts", det)
                    self.handle_timeout(detector=det)

                elif latest_status[det]['status'] == DAQ_STATUS.ERROR:
                    self.log.info(f"The {det} has error, sending stop command")
                    self.control_detector(command='stop', detector=det, force=self.can_force_stop[det])
                    self.can_force_stop[det]=False
                else:
                    # shouldn't be able to get here
                    pass

        return

    def handle_timeout(self, detector):
        """
        Detector already in the TIMEOUT status are directly stopped.
        """
        self.control_detector(command='stop', detector=detector)

        return

    def stop_detector_gently(self, detector):
        """
        Stops the detector, unless we're told to wait for the current
        run to end
        """
        if (
                # Running normally (not arming, error, timeout, etc)
                self.latest_status[detector]['status'] == DAQ_STATUS.RUNNING and
                # We were asked to wait for the current run to stop
                self.goal_state[detector].get('softstop', 'false') == 'true'):
            self.check_run_turnover(detector)
        else:
            self.control_detector(detector=detector, command='stop')

    def control_detector(self, command, detector, force=False):
        """
        Issues the command to the detector if allowed by the timeout
        """
        time_now = now()
        try:
            dt = (time_now - self.last_command[command][detector]).total_seconds()
        except (KeyError, TypeError):
            dt = 2*self.timeouts[command]

        # make sure we don't rush things
        if command == 'start':
            dt_last = (time_now - self.last_command['arm'][detector]).total_seconds()
        elif command == 'arm':
            dt_last = (time_now - self.last_command['stop'][detector]).total_seconds()
        else:
            dt_last = self.time_between_commands*2

        if (dt > self.timeouts[command] and dt_last > self.time_between_commands) or force:
            ls = self.latest_status
            gs = self.goal_state
            if command == 'arm':
                if self.one_detector_arming:
                    self.log.info('Another detector already arming, can\'t arm %s' % detector)
                    # this leads to run number overlaps
                    return
                readers, cc = self.mongo.get_hosts_for_mode(gs[detector]['mode'])
                hosts = (cc, readers)
                delay = 0
                self.one_detector_arming = True
            elif command == 'start':
                readers, cc = self.mongo.get_hosts_for_mode(ls[detector]['mode'])
                hosts = (readers, cc)
                # we can safely short the logic here and buy an extra logic cycle
                self.one_detector_arming = False
                delay = self.start_cmd_delay
                #Reset arming timeout counter 
                self.missed_arm_cycles[detector]=0
            else: # stop
                readers, cc = self.mongo.get_hosts_for_mode(ls[detector]['mode'])
                hosts = (cc, readers)
                if force or ls[detector]['status'] not in [DAQ_STATUS.RUNNING]:
                    delay = 0
                else:
                    delay = self.stop_cmd_delay
                if ls[detector]['status'] in [DAQ_STATUS.ARMING, DAQ_STATUS.ARMED]:
                    # this was the arming detector
                    self.one_detector_arming = False
            self.log.debug(f'Sending {command.upper()} to {detector}')
            if self.mongo.send_command(command, hosts, gs[detector]['user'],
                    detector, gs[detector]['mode'], delay, force):
                # failed
                return
            self.last_command[command][detector] = time_now
            if command == 'start' and self.mongo.insert_run_doc(detector):
                # db having a moment
                return
            if (command == 'stop' and ls[detector]['number'] != -NO_NEW_RUN and
                    self.mongo.set_stop_time(ls[detector]['number'], detector, force)):
                # db having a moment
                return

        else:
            self.log.debug('Can\'t send %s to %s, timeout at %i/%i' % (
                command, detector, dt, self.timeouts[command]))

    def check_timeouts(self, detector, command=None):
        """ 
        This one is invoked if we think we need to change states. Either a stop command needs
        to be sent, or we've detected an anomaly and want to decide what to do. 
        Basically this function decides:
          - We are not in any timeouts: send the normal stop command
          - We are waiting for something: do nothing
          - We were waiting for something but it took too long: attempt reset
        """

        time_now = now()

        #First check how often we have been timing out, if it happened to often
        # something bad happened and we start from scratch again
        if self.missed_arm_cycles[detector]>self.max_arm_cycles and detector=='tpc':
            self.hypervisor.tactical_nuclear_option()
            return

        if command is None: # not specified, we figure out it here
            command_times = [(cmd,doc[detector]) for cmd,doc in self.last_command.items()]
            command = sorted(command_times, key=lambda x : x[1])[-1][0]
            self.log.debug(f'Most recent command for {detector} is {command}')
        else:
            self.log.debug(f'Checking {command} timeout for {detector}')

        dt = (time_now - self.last_command[command][detector]).total_seconds()

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
                    self.mongo.log_error(
                                        ("Dispatcher control loop detects a timeout that STOP " +
                                         "can't solve"),
                                        'ERROR',
                                        "STOP_TIMEOUT")
                    # also invoke the nuclear option
                    self.hypervisor.tactical_nuclear_option()
                    self.error_stop_count[detector] = 0
                else:
                    self.control_detector(detector=detector, command='stop')
                    self.log.debug(f'Working on a stop counter for {detector}')
                    self.error_stop_count[detector] += 1
            else:
                self.mongo.log_error(
                        ('%s took more than %i seconds to %s, indicating a possible timeout or error' %
                            (detector, self.timeouts[command], command)),
                        'ERROR',
                        '%s_TIMEOUT' % command.upper())
                #Keep track of how often the arming sequence times out
                self.missed_arm_cycles[detector] += 1
                self.control_detector(detector=detector, command='stop')

        return

    def throw_error(self):
        """
        Throw a general error that the DAQ is stuck
        """
        self.mongo.log_error(
                            "Dispatcher control loop can't get DAQ out of stuck state",
                            'ERROR',
                            "GENERAL_ERROR")

    def check_run_turnover(self, detector):
        """
        During normal operation we want to run for a certain number of minutes, then
        automatically stop and restart the run. No biggie. We check the time here
        to see if it's something we have to do.
        """

        number = self.latest_status[detector]['number']
        start_time = self.mongo.get_run_start(number)
        if start_time is None:
            self.log.debug(f'No start time for {number}?')
            return
        time_now = now()
        run_length = int(self.goal_state[detector]['stop_after'])*60
        run_duration = (time_now - start_time).total_seconds()
        self.log.debug('Checking run turnover for %s: %i/%i' % (detector, run_duration, run_length))
        if run_duration > run_length:
            self.log.info('Stopping run for %s' % detector)
            self.control_detector(detector=detector, command='stop')

