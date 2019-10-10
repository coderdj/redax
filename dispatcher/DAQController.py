import datetime
import os
import json
'''
DAQ Controller Brain Class
D. Coderre, 12. Mar. 2019

Brief: This code handles the logic of what the dispatcher does when. It takes in 
aggregated status updates and commands from the mongo connector and decides if
any action needs to be taken to get the DAQ into the target state. It also handles the
resetting of runs (the ~hourly stop/start) during normal operations.
'''

class DAQController():

    def __init__(self, config, mongo_connector):

        self.mongo = mongo_connector
        self.goal_state = {}
        self.latest_status = {}

        # Handy lookup table so code is more readable
        self.st = {
            "IDLE": 0,
            "ARMING": 1,
            "ARMED": 2,
            "RUNNING": 3,
            "ERROR": 4,
            "TIMEOUT": 5,
            "UNDECIDED": 6
        }

        # Timeouts. There are a few things that we want to wait for that might take time.
        # The keys for these dicts will be detector identifiers.
        self.arm_command_sent = {}
        self.start_command_sent = {}
        self.stop_command_sent = {}
        self.error_stop_count = {}

        # Timeout properties come from config
        self.arm_timeout = int(config['DEFAULT']['ArmCommandTimeout'])
        self.start_timeout = int(config['DEFAULT']['StartCommandTimeout'])
        self.stop_timeout = int(config['DEFAULT']['StopCommandTimeout'])
        self.stop_retries = int(config['DEFAULT']['RetryReset'])
        self.error_thrown = False
        
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

        The way that works is this. We do everything iteratively. Like if we see that
        some detector needs to be stopped in order to proceed we issue the stop command
        then move on. Everything is then re-evaluated once that command runs through.
        
        I wrote this very verbosely since it's got quite a few different possibilities and
        after rewriting once I am convinced longer, clearer code is better than terse, efficient
        code for this particular function. Also I'm hardcoding the detector names. 
        '''

        # cache these so other functions can see them
        self.goal_state = goal_state
        self.latest_status = latest_status

        # This check will reset our timeout timers as needed
        for det in latest_status.keys():
            # If we are IDLE we gonna assume any previous 'START, STOP, ARM' commands invalid
            if latest_status[det]['status'] == self.st['IDLE']:
                if det in self.stop_command_sent.keys():
                    self.stop_command_sent[det] = None
                if det in self.error_stop_count.keys():
                    self.error_stop_count[det] = 0
            # If we are ARMED we will assume any previous ARM commands worked
            if latest_status[det]['status'] == self.st['ARMED']:
                if det in self.arm_command_sent.keys():
                    self.arm_command_sent[det] = None
            # If we are RUNNING we also assume the previous RUN command worked
            if latest_status[det]['status'] == self.st['RUNNING']:
                if det in self.start_command_sent.keys():
                    self.start_command_sent[det] = None
         
        
        '''
        CASE 1: DETECTORS ARE INACTIVE
        
        In our case 'inactive' means 'stopped'. An inactive detector is in its goal state as 
        long as it isn't doing anything, i.e. not ARMING, ARMED, or RUNNING. We don't care if 
        it's idle, or in error, or if there's no status at all. We will care about that later
        if we try to activate it.
        '''
        # 1a - deal with TPC and also with MV and NV, but only if they're linked
        active_states = [self.st['ARMING'], self.st['ARMED'], self.st['RUNNING'], self.st['UNDECIDED'],
                         self.st['ERROR'], self.st['TIMEOUT']]
        if goal_state['tpc']['active'] == 'false':

            # Send stop command if we have to
            if (
                    # TPC not in Idle, error, timeout
                    (latest_status['tpc']['status'] in active_states) or
                    # MV linked and not in Idle, error, timeout
                    (latest_status['muon_veto']['status']  in active_states and
                     goal_state['tpc']['link_mv'] == 'true') or
                    # NV linked and not in Idle, error, timeout
                    (latest_status['neutron_veto']['status']  in active_states and
                     goal_state['tpc']['link_nv'] == 'true')
            ):
                self.CheckTimeouts('tpc')

        # 1b - deal with MV but only if MV not linked to TPC
        if goal_state['tpc']['link_mv'] == 'false' and goal_state['muon_veto']['active'] == 'false':
            if latest_status['muon_veto']['status']  in active_states:
                self.CheckTimeouts('muon_veto')
        # 1c - deal with NV but only if NV not linked to TPC
        if goal_state['tpc']['link_nv'] == 'false' and goal_state['neutron_veto']['active'] == 'false':
            if latest_status['neutron_veto']['status']  in active_states:
                self.CheckTimeouts('neutron_veto')

        '''
        CASE 2: DETECTORS ARE ACTIVE
        
        This will be more complicated.
        There are now 4 possibilities (each with sub-possibilities and each for different
        combinations of linked or unlinked detectors):
         1. The detectors were already running. Here we have to check if the run needs to
            be reset but otherwise maybe we can just do nothing.
         2. The detectors were not already running. We have to start them.
         3. The detectors are in some failed state. We should periodically complain
         4. The detectors are in some in-between state (i.e. ARMING) and we just need to
            wait for some seconds to allow time for the thing to sort itself out.
        '''
        # 2a - again we consider the TPC first, as well as the cases where the NV/MV are linked
        if goal_state['tpc']['active'] == 'true':            

            # Maybe we have nothing to do except check the run turnover
            if (
                    # TPC running!
                    (latest_status['tpc']['status'] == self.st['RUNNING']) and
                    # MV either unlinked or running
                    (latest_status['muon_veto']['status'] == self.st['RUNNING'] or
                     goal_state['tpc']['link_mv'] == 'false') and
                    # NV either unlinked or running
                    (latest_status['neutron_veto']['status'] == self.st['RUNNING'] or
                     goal_state['tpc']['link_nv'] == 'false')
            ):
                print("Checking run turnover TPC")
                self.CheckRunTurnover('tpc')

            # Maybe we're already ARMED and should start a run
            elif (
                    # TPC ARMED
                    (latest_status['tpc']['status'] == self.st['ARMED']) and
                    # MV ARMED or UNLINKED
                    (latest_status['muon_veto']['status'] == self.st['ARMED'] or
                     goal_state['tpc']['link_mv'] == 'false') and
                    # NV ARMED or UNLINKED
                    (latest_status['neutron_veto']['status'] == self.st['ARMED'] or
                     goal_state['tpc']['link_nv'] == 'false')):
                print("Starting TPC")
                self.StartDetector('tpc')
                
            # Maybe we're IDLE and should arm a run
            elif (
                    # TPC IDLE
                    (latest_status['tpc']['status'] == self.st['IDLE']) and
                    # MV IDLE or UNLINKED
                    (latest_status['muon_veto']['status'] == self.st['IDLE'] or
                     goal_state['tpc']['link_mv'] == 'false') and
                    # NV IDLE or UNLINKED
                    (latest_status['neutron_veto']['status'] == self.st['IDLE'] or
                     goal_state['tpc']['link_nv'] == 'false')):
                print("Arming TPC")
                self.ArmDetector('tpc')
                
            # Maybe someone is either in error or timing out or we're in some weird mixed state
            # I think this can just be an 'else' because if we're not in some state we're happy
            # with we should probably check if a reset is in order.
            # Note that this will be triggered nearly every run during ARMING so it's not a
            # big deal
            else:
                print("Checking timeouts")
                self.CheckTimeouts('tpc')
                
        # 2b, 2c. In case the MV and/or NV are UNLINKED and ACTIVE we can treat them
        # in basically the same way.
        for detector in ['muon_veto', 'neutron_veto']:
            linked = goal_state['tpc']['link_mv']
            if detector == 'neutron_veto':
                linked = goal_state['tpc']['link_nv']

            # Active/unlinked. You your own detector now.
            if (goal_state[detector]['active'] == 'true' and linked == 'false'):

                # Same logic as before but simpler cause we don't have to check for links
                if latest_status[detector]['status'] == self.st['RUNNING']:
                    self.CheckRunTurnover(detector)
                elif latest_status[detector]['status'] == self.st['ARMED']:
                    self.StartDetector(detector)
                elif latest_status[detector]['status'] == self.st['IDLE']:
                    self.ArmDetector(detector)                    
                else:
                    self.CheckTimeouts(detector)

        return
                

    def ArmDetector(self, detector):
        '''
        Arm the detector given. The arm parameters are stored in the self.goal_state object
        '''
        if (detector not in self.arm_command_sent.keys() or
            self.arm_command_sent[detector] is None):
            run_mode = self.goal_state[detector]['mode']
            host_list, cc = self.mongo.GetHostsForMode(run_mode)
            print("Crate controller: %s"%cc)
            for c in cc:
                host_list.append(c)
            self.mongo.SendCommand("arm", host_list, self.goal_state[detector]['user'],
                                   detector, self.goal_state[detector]['mode'])
            self.arm_command_sent[detector] = datetime.datetime.utcnow()
        else:
            # If an arm command has been sent, have a look if it timed out
            self.CheckTimeouts(detector)    
        return

    def StartDetector(self, detector):
        '''
        Start the detector given. If there is a crate controller involved then this is a 
        two-stage process. 
        '''
        if (detector not in self.start_command_sent.keys() or
            self.start_command_sent[detector] is None):
            run_mode = self.goal_state[detector]['mode']
            host_list, cc = self.mongo.GetHostsForMode(run_mode)
            run = self.mongo.InsertRunDoc(detector, self.goal_state)
            self.mongo.SendCommand("start", host_list, self.goal_state[detector]['user'],
                                   detector, self.goal_state[detector]['mode'])
            self.mongo.SendCommand("start", cc, self.goal_state[detector]['user'],
                                   detector, self.goal_state[detector]['mode'], 5)
            self.start_command_sent[detector] = datetime.datetime.utcnow()
        else:
            self.CheckTimeouts(detector)
        return

    def StopDetector(self, detector):
        '''
        Stop the detector given. If there is a crate controller involved then this is a
        two-stage process.
        '''

        # Little different than before. We want to stop all associated readers and ccs,
        # but we can't get them from the run mode (this may have changed) and will get them
        # from the config of the dispatcher, which we cache.
        readers, cc = self.mongo.GetConfiguredNodes(detector, self.goal_state['tpc']['link_mv'],
                                                    self.goal_state['tpc']['link_nv'])

        # Set all timeouts to nothing
        self.arm_command_sent[detector] = None
        self.start_command_sent[detector] = None
        
        # We do not need to check the 'stop_command_sent' because this function is
        # exclusively called through the CheckTimeouts wrapper
        self.mongo.SendCommand("stop", cc, self.goal_state[detector]['user'],
                               detector, self.goal_state[detector]['mode'])
        self.mongo.SendCommand("stop", readers, self.goal_state[detector]['user'],
                               detector, self.goal_state[detector]['mode'], 5)
        try:
            self.mongo.SetStopTime(self.latest_status[detector]['number'])
        except Exception as E:
            print(E)
            print("Wanted to stop run but no associated number")
        return
            
    def CheckTimeouts(self, detector):
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
        
        # Case 1: maybe we sent a stop, arm, or start command and are still in the timeout
        if ((detector in self.stop_command_sent.keys() and self.stop_command_sent[detector] != None and
             (nowtime - self.stop_command_sent[detector]).total_seconds() <= self.stop_timeout) or
            (detector in self.arm_command_sent.keys() and self.arm_command_sent[detector] != None and
             (nowtime - self.arm_command_sent[detector]).total_seconds() <= self.arm_timeout) or
            (detector in self.start_command_sent.keys() and self.start_command_sent[detector]!= None and
             (nowtime - self.start_command_sent[detector]).total_seconds() <= self.start_timeout)):
            # We're in a normal waiting period. Return later if still a problem
            self.error_stop_count[detector] = 0
            print("CheckTimeouts detected that we are within the configured timeout period for a " +
                  "command. command_sent structs follow in order arm/start/stop:")
            print(self.arm_command_sent)
            print(self.start_command_sent)
            print(self.stop_command_sent)
            return

        # Case 2: we're not timing out at all, send the stop command
        if (detector not in self.stop_command_sent.keys() or self.stop_command_sent[detector] is None):
            sendstop = True
            self.stop_command_sent[detector] = nowtime
            self.error_stop_count[detector] = 0
            
        # make sure this detector in self.error_stop_count
        if detector not in self.error_stop_count.keys() or self.error_stop_count[detector] is None:
            self.error_stop_count[detector] = 0

        # Case 3: Something timed out. We'll clear the previous command sent and start a
        # new timeout based on our current stop command
        # 3a: ARM timeout
        if (detector in self.arm_command_sent.keys() and self.arm_command_sent[detector] != None and
            (nowtime-self.arm_command_sent[detector]).total_seconds() > self.arm_timeout):
            self.arm_command_sent[detector] = None
            sendstop = True
            self.stop_command_sent[detector] = nowtime
            self.mongo.LogError("dispatcher",
                                "Took more than %i seconds to arm, indicating a possible timeout"%
                                self.arm_timeout,
                                "WARNING", "ARM_TIMEOUT")
        # 3b: START timeout
        elif (detector in self.start_command_sent.keys() and self.start_command_sent[detector]!=None and
              (nowtime-self.start_command_sent[detector]).total_seconds() > self.start_timeout):
            self.start_command_sent[detector] = None
            sendstop = True
            self.stop_command_sent[detector] = nowtime
            self.mongo.LogError("dispatcher",
                                "Took more than %i seconds to start, indicating a possible timeout"%
                                self.start_timeout,
                                self.st["WARNING"], "START_TIMEOUT")
        # 3c: STOP timeout. And this is where the thing can get stuck so we gotta toss an
        # error if it goes on too long
        elif (detector in self.stop_command_sent.keys() and self.stop_command_sent[detector] != None and
              ( (nowtime - self.stop_command_sent[detector]).total_seconds() >
                (self.stop_timeout + (self.error_stop_count[detector]*self.stop_timeout)))):

            # If error_stop_count is already at the maximum we throw a ERROR then do nothing
            if self.error_stop_count[detector] >= self.stop_retries and self.error_thrown == False:
                self.mongo.LogError("dispatcher",
                                    ("Dispatcher control loop detects a timeout that is not solved " +
                                     "with a STOP command"), 
                                    'ERROR',
                                    "STOP_TIMEOUT")
                sendstop = False
            elif self.error_stop_count[detector] < self.stop_retries:
                sendstop = True
                self.error_stop_count[detector] += 1

        if sendstop:
            print("SENT STOP")
            self.StopDetector(detector)

        return

    
    def ThrowError(self):
        '''
        Throw a general error that the DAQ is stuck
        '''
        self.mongo.LogError("dispatcher",
                            "Dispatcher control loop can't get DAQ out of stuck state",
                            'ERROR',
                            "GENERAL_ERROR")

    def CheckRunTurnover(self, detector):
        '''
        During normal operation we want to run for a certain number of minutes, then
        automatically stop and restart the run. No biggie. We check the time here
        to see if it's something we have to do.
        '''
        try:
            number = self.latest_status[detector]['number']
        except:
            # dirty workaround just in case there was a dispatcher crash
            self.latest_status[detector]['number'] = self.mongo.GetNextRunNumber() - 1
            number = self.latest_status[detector]['number']
        start_time = self.mongo.GetRunStart(number)
        nowtime = datetime.datetime.utcnow()
        run_length = int(self.goal_state[detector]['stop_after'])*60
        if (nowtime-start_time).total_seconds() > run_length:
            self.StopDetector(detector)
