
class Detector:

    def __init__(self, detector_config):
        self.status = -1
        self.running_since = None
        self.detector_config = detector_config
        self.arming = False
        self.arm_timestamp = None
        self.pending_command = None
        self.current_number = None
        self.mode = None
        self.readers = self.detector_config['readers']
        self.n_readers = len(self.detector_config['readers'])
        self.aggregate = {"rate": 0, "buff": 0, "status": 0}
        self.crate_controller = self.detector_config['crate_controller']  
        
    def set_readers(self, config):
        readers = list(set([a['host'] for a in config['boards']]))
        self.readers = readers
        self.n_readers = len(readers)

    def readers(self):
        return list(self.readers)

    def set_crate_controller(self, config):
        if 'crate_controller' in config:
            self.crate_controller = config['crate_controller']
        else:
            self.crate_controller = None
    def crate_controller(self):
        return self.crate_controller
    
    def start_arm(self, timestamp, doc):
        self.arming = True
        self.arm_timestamp = timestamp
        self.mode = doc['mode']
        self.pending_command = dict(doc)

    def check_arm_fail(self, timestamp, tolerance):
        if self.arm_timestamp is not None and timestamp - self.arm_timestamp > tolerance:
            return True
        return False

    def clear_arm(self):
        self.arming = False
        self.arm_timestamp = None
        self.pending_command = None

