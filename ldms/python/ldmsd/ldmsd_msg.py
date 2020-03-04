import json
import errno
import struct

class LDMSD_msg_obj(object):

    LDMSD_MSG_OBJ_TYPE_CFG = 1
    LDMSD_MSG_OBJ_TYPE_ACT = 2
    LDMSD_MSG_OBJ_TYPE_CMD = 3
    LDMSD_MSG_OBJ_TYPE_ERR = 4
    LDMSD_MSG_OBJ_TYPE_ADV_REC = 5
    LDMSD_MSG_OBJ_TYPE_INFO = 6
    
    LDMSD_MSG_OBJ_TYPE_STR_MAP = {
        LDMSD_MSG_OBJ_TYPE_CFG: "cfg_obj",
        LDMSD_MSG_OBJ_TYPE_ACT: "act_obj",
        LDMSD_MSG_OBJ_TYPE_CMD: "cmd_obj",
        LDMSD_MSG_OBJ_TYPE_ERR: "err_obj",
        LDMSD_MSG_OBJ_TYPE_ADV_REC: "adv_rec",
        LDMSD_MSG_OBJ_TYPE_INFO: "info_obj"
        }
    
    def __init__(self, type):
        self.type = type
        self.type_str = self.LDMSD_MSG_OBJ_TYPE_STR_MAP[type]
        self.obj = {'type': self.type_str}

class LDMSD_msg_cfg_obj(LDMSD_msg_obj):

    def __init__(self, cfg_obj):
        super().__init__(type)
        self.obj["cfg_obj"] = cfg_obj
        self.obj["spec"] = {}

    def specSet(self, spec):
        self.obj["spec"] = spec

    def __init__(self, ctrl):
        self.msg_no = LDMSD_Message_Key.MESSAGE_NO
        self.conn_id = ctrl

class LDMSD_Message(object):
    LDMSD_MSG_TYPE_REQ = 1
    LDMSD_MSG_TYPE_RSP = 2

    LDMSD_REC_F_SOM = 1
    LDMSD_REC_F_EOM = 2

    MESSAGE_NO = 1
    LDMSD_REC_HDR_FMT = '!LLLL'
    LDMSD_REC_HDR_SZ = struct.calcsize(LDMSD_REC_HDR_FMT)
    
    def __init__(self, ctrl):
        self.ctrl = ctrl
        self.type = None
        self.msg_no = -1
        self.json_str = ""
        self.json_ent = None
        self.num_rec = 0

    def _newRecord(self, flags, json_str_offset, remaining):
        """Create a record

        offset is offset from the request header
        sz is the size of the data to be sent
        """
        rec_len = self.LDMSD_REC_HDR_SZ + remaining
        hdr =  struct.pack(self.LDMSD_REC_HDR_FMT, self.type, flags, 
                           self.msg_no, rec_len)
        data = struct.pack(str(remaining) + 's', \
                           self.json_str[json_str_offset:json_str_offset+remaining].encode())
        return hdr + data

    def send(self, type, json_ent, json_str):
        self.msg_no = self.MESSAGE_NO
        self.MESSAGE_NO += 1
        
        self.type = type
        if json_str:
            self.json_str += json_str
        if self.json_ent is not None:
            self.json_str = json.dumps(json_ent)
        self.json_str_len = len(self.json_str)

        max_msg = self.ctrl.getMaxRecvLen()
        offset = 0
        leftover = self.json_str_len
        try:
            while True:
                remaining = max_msg - self.LDMSD_REC_HDR_SZ
                if offset == 0:
                    flags = self.LDMSD_REC_F_SOM
                else:
                    flags = 0
                if remaining > leftover:
                    remaining = leftover
                    flags |= self.LDMSD_REC_F_EOM
                record = self._newRecord(flags, offset, remaining)
                offset += remaining
                leftover -= remaining
                self.ctrl.send_command(bytes(record))
                self.num_rec += 1
                if leftover == 0:
                    break
        except:
            raise

    def receive(self):
        json_str = ""
        self.num_rec = 0
        while True:
            record = self.ctrl.receive_response()
            if record is None:
                raise LDMSDRequestException(message="No data received", 
                                            errcode=errno.ECONRESET)
            (self.type, flags, self.msg_no, rec_len) = struct.unpack(self.LDMSD_REC_HDR_FMT, \
                                                              record[:self.LDMSD_REC_HDR_SZ])
            json_str += struct.unpack(str(rec_len - self.LDMSD_REC_HDR_SZ) + 's', 
                                      record[self.LDMSD_REC_HDR_SZ:])[0].decode()
            self.num_rec += 1
            if (flags & self.LDMSD_REC_F_EOM):
                break
        self.json_ent = json.loads(json_str)
        return self
        
    