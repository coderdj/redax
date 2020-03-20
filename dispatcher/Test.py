

class Klass(object):

    def SolveProblem(self, latest_status, goal_state):

        self.goal_state = goal_state
        self.latest_status = latest_status
        link_nv = goal_state['tpc']['link_nv']
        link_mv = goal_state['tpc']['link_mv']

        for det in self.goal_state.keys():
            status = latest_status[det]['status']
            if self.goal_state[det]['active'] == 'true':
                # detector should be running
                if status == self.st['IDLE']:
                    # detector is idle, send an ARM command
                    if det == 'tpc':
                        # handle tpc separately because linking
                        if (link_mv == 'true' and 
                                latest_status['muon_veto'] != self.st['IDLE']):
                            # MV linked but not idle
                            self.StopDetector('muon_veto')
                        elif (link_nv == 'true' and 
                                latest_status['neutron_veto'] != self.st['IDLE']):
                            # NV linked but not idle
                            self.StopDetector('neutron_veto')
                        else:
                            # unlinked, or linked and idle
                            self.ArmDetector(det)
                    elif ((det == 'muon_veto' and link_mv == 'false') or
                          (det =='neutron_veto' and link_nv == 'false')):
                        self.ArmDetector(det)

                elif status == self.st['ARMING']:
                    # detector is arming, check timeout
                    self.CheckArmTimeout(det)

                elif status == self.st['ARMED']:
                    # detector is armed, send START command
                    self.StartDetector(det)

                elif status == self.st['RUNNING']:
                    # detector is running, check run turnover
                    if det == 'tpc':
                        if ((link_mv == 'true' and
                                latest_status['mv'] != self.st['RUNNING']) or
                            (link_nv == 'true' and
                                latest_status['nv'] != self.st['RUNNING'])):
                            # linked but not running
                            self.StopDetector(det)
                        else:
                            self.CheckRunTurover(det)
                    elif ((det == 'muon_veto' and link_mv == 'false') or
                          (det =='neutron_veto' and link_nv == 'false')):
                        self.CheckRunTurnover(det)
                elif status == self.st['ERROR']:
                    # detector is in error
                    self.StopDetector(det)
                elif status == self.st['TIMEOUT']:
                    # detector is timing out, log error
                    self.StopDetector(det)
                else: # UNKNOWN
                    # in unknown territory here
                    self.StopDetector(det)


            else: # goal['active'] == 'false'
                if status == self.st['IDLE']:
                    # nothing to do
                    pass
                elif status == self.st['ARMING']:
                    # wait until it's armed
                    self.CheckArmTimeout(det)
                elif status == self.st['ARMED']:
                    # issue stop
                    self.StopDetector(det)
                elif status == self.st['RUNNING']:
                    # issue stop
                    self.StopDetector(det)
                elif status == self.st['ERROR']:
                    # not trying to run, but send a stop anyway
                    self.StopDetector(det)
                elif status == self.st['TIMEOUT']:
                    # one node not responding
                    pass
                else: # UNKNOWN
                    # Do nothing right now
                    pass

