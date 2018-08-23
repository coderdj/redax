
class Detector:

    def __init__(self, detector_config):
        self.status = -1
        self.detector_config = detector_config
        self.arming = False
        self.arm_timestamp = None
        self.pending_command = None
        self.current_number = None
        self.aggregate = {}
        
    def readers(self):
        return list(self.detector_config['readers'])

    def crate_controller(self):
        if "crate_controller" in self.detector_config.keys():
            return self.detector_config['crate_controller']
        return None
    
    def start_arm(self, timestamp, doc):
        self.arming = True
        self.arm_timestamp = timestamp
        self.pending_command = dict(doc)

    def check_arm_fail(self, timestamp, tolerance):
        if self.arm_timestamp is not None and timestamp - self.arm_timestamp > tolerance:
            return True
        return False

    def clear_arm(self):
        self.arming = False
        self.arm_timestamp = None
        self.pending_command = None

