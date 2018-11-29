import time
import datetime
import os
import copy

class DAQBroker():
    def __init__(self, DBInt):
        self.db = DBInt
        self.dets = DBInt.LoadDispatcherStatus()
        self.timeout = 30 # after 30 seconds we consider a host to be timing out
        self.arm_timeout = 30
        self.status_codes = {"IDLE": 0, "ARMING": 1, "ARMED": 2, "RUNNING": 3,
                             "ERROR": 4, "TIMEOUT": 5, "UNDECIDED": 6}
        self.error_retries = 10 # How many times should I retry to fix things
        self.error_retry_spacing = 30 # How many seconds between retries
        
    def Update(self, desired_state, HostStatus):
        # Return a list of commands to execute
        pending_commands = []
        
        for doc in desired_state:

            det = doc['detector']
            
            # Case 1: doc is not active, detector not here, ignore
            if doc['active'] == 'false' and det not in self.dets.keys():                
                continue

            # Case 2: doc says not active, detector is in self.dets.
            # means we just stopped the run manually
            elif doc['active'] == 'false' and det in self.dets.keys():
                
                self.dets[det]['active'] = 'false'
                self.dets[det]['rate'] = 0
                self.dets[det]['buff'] = 0

                # If not running or armed just ignore
                if self.dets[det]['status'] not in [self.status_codes["ARMED"],
                                                    self.status_codes["RUNNING"],
                                                    self.status_codes["ARMING"]]:
                    self.dets[det]['diagnosis'] = 'goal'
                    self.dets[det]['error_retry_count'] = 0
                    continue

                # If no hosts, continue
                if len(self.dets[det]['hosts']) == 0:
                    self.dets[det]['status'] = self.status_codes['IDLE']
                    continue

                pending_commands += self.MakeCommand("stop", doc)

                # We need to remove the 'hosts' so we don't start reporting issues
                # when running TPC runs using the MV and NV hosts
                self.dets[det]['hosts'] = []

            # Case 3: doc says active, detector not in self.dets. You added a new
            # detector or cleared the state database
            elif doc['active'] == 'true' and det not in self.dets.keys():

                # If no hosts, continue
                if len(self.dets[det]['hosts']) == 0:
                    continue  
                
                # create det entry
                # arm command will be issued on next iteration
                det_doc = { "mode": doc['mode'], 'status': self.status_codes["IDLE"],
                            'stop_after': doc['stop_after'], 'active': doc['active'],
                            "user": doc['user'], "comment": doc['comment'], "hosts": [],
                            "diagnosis": "processing", "rate": 0, "buff": 0}
                self.dets[det] = det_doc

                
            # Before proceeding, update all our statuses with HostStatus
            self._update_status(HostStatus)
                
            # Case 4 (notice no ELSE cause we want to execute every time even if we
            # just created the det_doc (why wait a second?)
            if doc['active'] == 'true' and det in self.dets.keys():
                
                # Make sure our det_doc has the right info
                self.dets[det]['active'] = doc['active']
                self.dets[det]['comment'] = doc['comment']
                self.dets[det]['user'] = doc['user']
                self.dets[det]['stop_after'] = doc['stop_after']

                # Even if the state did not change, if the mode changed we want to
                # force a restart because the shifter is trying to change run mode
                force_restart = False
                if self.dets[det]['mode'] != doc['mode']:
                    self.dets[det]['mode'] = doc['mode']
                    force_restart = True
                
                # OK, so we should be running. What could to possibilities be?
                # If IDLE, start the arm command
                if self.dets[det]['status'] == self.status_codes["IDLE"]:

                    self.dets[det]['diagnosis'] = 'processing'                    

                    # Can we even arm this run? As in are all the necessary nodes available?
                    if not self.CheckRunPlausibility(doc['mode'], det):
                        print("Run implausible")
                        continue

                    pending_commands += self.MakeCommand("arm", doc)
                        
                # If ARMED, send the start command
                elif self.dets[det]['status'] == self.status_codes["ARMED"]:

                    self.dets[det]['diagnosis'] = 'processing'
                    # check if we already sent a start command
                    if 'started_at' in self.dets[det] and self.dets[det]['started_at'] != None:
                        continue
                    
                    pending_commands += self.MakeCommand("start", doc)
                    
                    
                # If RUNNING, check how long we've been running and send the
                #             stop command in case we've hit our desired run length
                elif (self.dets[det]['status'] == self.status_codes["RUNNING"] and
                      self.dets[det]['started_at']!=None):
                    # print("CASE 4")
                    self.dets[det]['diagnosis'] = 'goal'
                    self.dets[det]['error_retry_count'] = 0
                    
                    send_stop = False
                    if self.dets[det]['stop_after'] is not None:
                        t = (datetime.datetime.utcnow() -
                             self.dets[det]['started_at']).total_seconds()
                        if t > int(self.dets[det]['stop_after'])*60:
                            send_stop = True
                    if force_restart:
                        send_stop = True

                    if send_stop:
                        pending_commands += self.MakeCommand('stop', doc)
                                                    
                # If ARMING, check if timed out
                elif self.dets[det]['status'] == self.status_codes["ARMING"]:
                    t = (datetime.datetime.utcnow() - self.dets[det]['armed_at']).total_seconds()
                    self.dets[det]['diagnosis'] = 'processing'
                    if t > self.arm_timeout:
                        self.dets[det]['diagnosis'] = 'error'
                        self.dets[det]['status'] = self.status_codes['ERROR']
                        self.dets[det]['number'] = None
                        self.dets[det]['armed_at'] = None

                # If anything else, send one (one!) reset command and throw if it doesn't help
                elif self.dets[det]['status'] in [self.status_codes['ERROR'],
                                               self.status_codes['TIMEOUT'],
                                               self.status_codes['UNDECIDED']]:
                    # print("CASE 5")
                    self.dets[det]['diagnosis'] = 'error'
                    if ("stopped_at" not in self.dets[det].keys()):
                        # Send stop command
                        pending_commands.append({
                            "user": doc['user'],
                            "host": self.dets[det]['hosts'],
                            "command": "stop"
                        })
                        self.dets[det]['stopped_at'] = datetime.datetime.utcnow()
            
                
        # For persistence we update self.dets to the database
        self.db.UpdateDispatcherStatus(self.dets)
        
        return pending_commands

    def GetStatus(self):
        return self.dets 

    def CheckRunPlausibility(self, mode, det):
        '''
        Before issuing an ARM command check that everything is reasonable
        '''
        # Check: is another detector arming? In order to keep run numbers
        #        consecutive we only allow one detector to arm at once.
        #        Only upon run doc insertion (happens at START) or failure
        #        do we allow another to start
        arming = False
        for detector, det_doc in self.dets.items():
            if det_doc['status'] in [self.status_codes["ARMING"],
                                     self.status_codes["ARMED"]]:
                arming = True
        if arming:
            return False
        
        # Check: does the host list for this detector match the hosts
        #        needed for the run? If so, proceed. If not update the
        #        host list now and defer running any commands until the
        #        next iteration so we can properly update the status
        needed_hosts, cc = self.db.GetHostsForMode(mode)
        if (('hosts' not in self.dets[det]) or (len(self.dets[det]['hosts'])==0) or
            (sorted(needed_hosts) != sorted(self.dets[det]['hosts'])) or
            (self.dets[det]['crate_controller'] != cc)):
            self.dets[det]['hosts'] = needed_hosts
            self.dets[det]['crate_controller'] = cc
            print("Updated host list")
            return False

        # Check: does the host list for this detector conflict with hosts listed in
        # any other detector? One process can certainly be part of two different
        # detectors, but not at the same time
        for detector, det_doc in self.dets.items():
            if detector == det or "hosts" not in det_doc.keys() or det_doc['active'] == 'false':
                continue
            for host in det_doc['hosts']:
                if host in self.dets[det]['hosts']:
                    print("Host conflict, can't start detector %s"%det)
                    #self.log.feedback_loop("Host conflict. Can't start detector %s"%det)
                    return False
        return True
        
    def _update_status(self, HostStatus):

        c = self.status_codes
        nowtime = datetime.datetime.utcnow()

        for det, doc in self.dets.items():

            status = -1
            # If the detector is "ARMING" then we give it this flag, otherwise it
            # would always say 'undecided'
            arming = False
            rate = 0
            buff = 0
            if doc['status'] == c["ARMING"] or doc['status'] == c['ARMED']:
                arming = True

            if 'hosts' not in doc or len(doc['hosts'])==0:
                # No hosts, idle by default
                self.dets[det]['status'] = c["IDLE"]
                continue
            for host in doc['hosts']:
                for host_stat in HostStatus:
                    if host == host_stat['host']:
                        rate += host_stat['rate']
                        buff += host_stat['buffer_length']
                        if status == -1:
                            status = host_stat['status']
                            break
                        
                        # Check for host timeout
                        id_timestamp = host_stat['_id'].generation_time
                        if( (nowtime.replace(tzinfo=None) -
                             id_timestamp.replace(tzinfo=None)).total_seconds() > self.timeout):
                            status = c['TIMEOUT']
                            break
                        
                        # Check for warnings or errors
                        if host_stat['status'] == c['ERROR']:
                            status = c["ERROR"]
                            break
                        
                        # Otherwise ensure all the same
                        if (host_stat['status'] != status and
                            status not in [c["ERROR"],c['TIMEOUT']]):
                            if arming:
                                status = c["ARMING"]
                            else: 
                                status = c['UNDECIDED']
            if status in [c['IDLE'], c['ARMED'], c['RUNNING']] and 'stopped_at' in self.dets[det]:
                del self.dets[det]['stopped_at']
            self.dets[det]['status'] = status
            self.dets[det]['rate'] = rate
            self.dets[det]['buffer'] = buff
            
        
    def GetAggregateStatus(self):

        ret = []
        for det, doc in self.dets.items():
            ret.append({
                "status": doc['status'],
                "number": doc['number'],
                "detector": det,
                "rate": doc['rate'],
                "readers": len(doc['hosts']),
                "time": datetime.datetime.utcnow(),
                "buff": doc['buff'],
                "mode": doc['mode']
            })
        return ret

    def MakeCommand(self, command, state_doc):

        pending_commands = []
        det = state_doc['detector']
        doc = state_doc
        
        if command == 'stop':
            # Setting started_at to None means we won't spam
            self.dets[det]['started_at'] = None
            self.dets[det]['diagnosis'] = 'processing'

            # Send stop
            # If crate controller exists, send stop there
            if 'crate_controller' in self.dets[det] and self.dets[det]['crate_controller'] is not None:
                pending_commands.append({
                    "user": doc['user'],
                    "host": [self.dets[det]['crate_controller']],
                    "command": "stop",
                    "detector": det,
                    "createdAt": datetime.datetime.utcnow()  
                })
                
            time.sleep(1)
            
            # Send stop command
            pending_commands.append({
                "user": doc['user'],
                "host": self.dets[det]['hosts'],
                "command": "stop",
                "detector": det,
                "createdAt": datetime.datetime.utcnow()  
            })
            
            if 'number' in self.dets[det] and self.dets[det]['number'] is not None:
                self.db.UpdateEndTime(self.dets[det]['number'])
                self.dets[det]['number'] = None

        elif command == 'arm':
            # Assign a temporary run number. Will become official if this works
            self.dets[det]['number'] = self.db.GetNextRunNumber()
            hlist = copy.deepcopy(self.dets[det]['hosts'])
            if 'crate_controller' in self.dets[det] and self.dets[det]['crate_controller'] != None:
                hlist.append(self.dets[det]['crate_controller'])
            pc = {
                'user': doc['user'],
                'detector': det,
                'host': hlist,
                'mode': doc['mode'],
                'command': 'arm',
                'number': self.dets[det]['number'],
                "createdAt": datetime.datetime.utcnow()  
            }

            options = self.db.GetRunMode(doc['mode'])
            #if options is not None and 'strax_output_path' in options.keys():
            #    run_folder_name = str(self.dets[det]['number']).zfill(8)
            #    pc["options_override"] = {"strax_output_path":
            #                              os.path.join(options['strax_output_path'], run_folder_name),
            #                              "run_identifier": str(self.dets[det]['number'])}
            #else:
            pc['options_override'] = {"run_identifier": str(self.dets[det]['number']).zfill(6)}
            
            pending_commands.append(pc)
            
            # Set the detector status to ARMING and set the armed_at flag
            # and set the other stuff
            self.dets[det]['status'] = self.status_codes["ARMING"]
            self.dets[det]['armed_at'] = datetime.datetime.utcnow()

        elif command == 'start':
            # clear armed_at
            self.dets[det]['armed_at'] = None

            # Add crate controller if there
            hlist = copy.deepcopy(self.dets[det]['hosts'])
            if 'crate_controller' in self.dets[det] and self.dets[det]['crate_controller'] != None:
                hlist.append(self.dets[det]['crate_controller'])

            # send it
            pending_commands.append({
                'user': doc['user'],
                'command': 'start',
                'host': hlist,
                'mode': doc['mode'],
                'number': self.dets[det]['number'],
                "detector": det,
                "createdAt": datetime.datetime.utcnow()
            })

            # update det start time
            self.dets[det]['started_at'] = datetime.datetime.utcnow()
            self.db.InsertRunDoc(self.dets[det], doc)
            
        return pending_commands
