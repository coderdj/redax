import time
import datetime

class DAQBroker():
    def __init__(self, DBInt):
        self.db = DBInt
        self.dets = DBInt.LoadDispatcherStatus()
        self.timeout = 30 # after 30 seconds we consider a host to be timing out
        self.arm_timeout = 30
        self.status_codes = {"IDLE": 0, "ARMING": 1, "ARMED": 2, "RUNNING": 3,
                             "ERROR": 4, "TIMEOUT": 5, "UNDECIDED": 6}
        
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
                
                # If not running or armed just ignore
                if self.dets[det]['status'] not in [self.status_codes["ARMED"],
                                                    self.status_codes["RUNNING"],
                                                    self.status_codes["ARMING"]]:
                    self.dets[det]['diagnosis'] = 'goal'                        
                    continue

                self.dets[det]['diagnosis'] = 'processing'
                
                # If crate controller exists, send stop there
                if 'crate_controller' in self.dets[det]:
                    pending_commands.append({
                        "user": doc['user'],
                        "host": [self.dets[det]['crate_controller']],
                        "command": "stop"
                    })


                # Send stop command
                pending_commands.append({
                    "user": doc['user'],
                    "host": self.dets[det]['hosts'],
                    "command": "stop"
                })

                # We need to remove the 'hosts' so we don't start reporting issues
                # when running TPC runs using the MV and NV hosts
                self.dets[det]['hosts'] = []

            # Case 3: doc says active, detector not in self.dets
            elif doc['active'] == 'true' and det not in self.dets.keys():

                
                # create det entry
                # arm command will be issued on next iteration
                det_doc = { "mode": doc['mode'], 'status': self.status_codes["IDLE"],
                            'stop_after': doc['stop_after'], 'active': doc['active'],
                            "user": doc['user'], "comment": doc['comment'], "hosts": [],
                            "diagnosis": "processing"}
                self.dets[det] = det_doc

                
            # Before proceeding, update all our statuses with HostStatus
            self._update_status(HostStatus)
                
            # Case 4 (notice no ELSE cause we want to execute every time even if we
            # just created the det_doc (why wait a second?)
            if doc['active'] == 'true' and det in self.dets.keys():
                self.dets[det]['active'] = doc['active']
                force_restart = False
                # Make sure our det_doc has the right info and force a restart if needed
                self.dets[det]['comment'] = doc['comment']
                self.dets[det]['user'] = doc['user']
                self.dets[det]['stop_after'] = doc['stop_after']
                if self.dets[det]['mode'] != doc['mode']:
                    self.dets[det]['mode'] = doc['mode']
                    force_restart = True
                
                # OK, so we should be running. What could to possibilities be?
                # If IDLE, start the arm command
                if self.dets[det]['status'] == self.status_codes["IDLE"]:

                    self.dets[det]['diagnosis'] = 'processing'
                    
                    # Check: is another detector arming? In order to keep run numbers
                    #        consecutive we only allow one detector to arm at once.
                    #        Only upon run doc insertion (happens at START) or failure
                    #        do we allow another to start
                    arming = False
                    for det, doc in self.dets.items():
                        if doc['status'] in [self.status_codes["ARMING"],
                                             self.status_codes["ARMED"]]:
                            arming = True
                    if arming:
                        continue
                    
                    # Check: does the host list for this detector match the hosts
                    #        needed for the run? If so, proceed. If not update the
                    #        host list now and defer running any commands until the
                    #        next iteration
                    needed_hosts, cc = self.db.GetHostsForMode(doc['mode'])
                    if (('hosts' not in self.dets[det]) or
                        (sorted(needed_hosts) != sorted(self.dets[det]['hosts']))):
                        self.dets[det]['hosts'] = needed_hosts
                        continue                    
                    self.dets[det]['crate_controller'] = cc

                    # Assign a temporary run number. Will become official if this work
                    self.dets[det]['number'] = self.db.GetNextRunNumber()
                    
                    pending_commands.append({
                        'user': doc['user'],
                        'host': self.dets[det]['hosts'],
                        'mode': doc['mode'],
                        'command': 'arm',
                        'number': self.dets[det]['number']
                    })

                    # Set the detector status to ARMING and set the armed_at flag
                    # and set the other stff
                    self.dets[det]['status'] = self.status_codes["ARMING"]
                    self.dets[det]['armed_at'] = datetime.datetime.now()
                    self.dets[det]['mode'] = doc['mode']
                    if 'stop_after' in doc:
                        self.dets[det]['stop_after'] = doc['stop_after']
                    else:
                        self.dets[det]['stop_after'] = None
                        
                # If ARMED, send the start command
                elif self.dets[det]['status'] == self.status_codes["ARMED"]:

                    self.dets[det]['diagnosis'] = 'processing'
                    
                    # clear armed_at
                    self.dets[det]['armed_at'] = None

                    # check if we already sent a start command
                    if 'stared_at' in self.dets[det] and self.dets[det]['started_at'] != None:
                        continue
                    
                    # send it
                    pending_commands.append({
                        'user': doc['user'],
                        'command': 'start',
                        'host': self.dets[det]['hosts'],
                        'crate_controller': self.dets[det]['crate_controller'],
                        'mode': doc['mode'],
                        'number': self.dets[det]['number']
                    })

                    # update det start time
                    self.dets[det]['started_at'] = datetime.datetime.now()
                    self.db.InsertRunDoc(self.dets[det], doc)
                    
                # If RUNNING, check how long we've been running and send the
                #             stop command in case we've hit our desired run length
                elif self.dets[det]['status'] == self.status_codes["RUNNING"]:

                    self.dets[det]['diagnosis'] = 'goal'
                    send_stop = False
                    if self.dets[det]['stop_after'] is not None:
                        t = (datetime.datetime.now() -
                             self.dets[det]['started_at']).total_seconds()
                        if t > int(self.dets[det]['stop_after'])*60:
                            send_stop = True
                    if force_restart:
                        send_stop = True

                    if send_stop:
                        # Setting started_at to None means we won't spam
                        self.dets[det]['started_at'] = None
                        self.dets[det]['diagnosis'] = 'processing'
                            
                        # Send stop
                        # If crate controller exists, send stop there
                        if 'crate_controller' in self.dets[det]:
                            pending_commands.append({
                                "user": doc['user'],
                                "host": [self.dets[det]['crate_controller']],
                                "command": "stop"
                            })
                            
                        # Send stop command
                        pending_commands.append({
                            "user": doc['user'],
                            "host": self.dets[det]['hosts'],
                            "command": "stop"
                        })

                                
                            
                # If ARMING, check if timed out
                elif self.dets[det]['status'] == self.status_codes["ARMING"]:
                    t = (datetime.datetime.now() - self.dets[det]['armed_at']).total_seconds()
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
                    self.dets[det]['diagnosis'] = 'error'
                    if ("stopped_at" not in self.dets[det].keys()):
                        # Send stop command
                        pending_commands.append({
                            "user": doc['user'],
                            "host": self.dets[det]['hosts'],
                            "command": "stop"
                        })
                        self.dets[det]['stopped_at'] = datetime.datetime.now()
            
                
        # For persistence we update self.dets to the database
        self.db.UpdateDispatcherStatus(self.dets)
        
        return pending_commands

    def GetStatus(self):
        return self.dets 

    def _update_status(self, HostStatus):

        c = self.status_codes
        nowtime = datetime.datetime.utcnow()

        for det, doc in self.dets.items():

            status = -1
            # If the detector is "ARMING" then we give it this flag, otherwise it
            # would always say 'undecided'
            arming = False
            if doc['status'] == c["ARMING"]:
                arming = True

            if 'hosts' not in doc or len(doc['hosts'])==0:
                # No hosts, idle by default
                self.dets[det]['status'] = c["IDLE"]
                continue
            for host in doc['hosts']:
                for host_stat in HostStatus:
                    if host == host_stat['host']:
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
            
        
