#!/usr/bin/env python3

import argparse
import re
from collections import namedtuple

Metric = namedtuple('Metric', ['metric_type', 'value_type', 'value'])

PRODUCER = "${HOSTNAME}_${PORT}"
INTERVAL = "1000000"
OFFSET = "0"

_META_BEGIN = r'(?P<meta_begin>Schema\s+Instance\s+Flags.*\s+Info)'
_META_DASHES = r'(?:^(?P<meta_dashes>[ -]+)$)'
_META_SUMMARY = '(?:'+ \
                r'Total Sets: (?P<meta_total_sets>\d+), ' + \
                r'Meta Data \(kB\):? (?P<meta_sz>\d+(?:\.\d+)?), ' + \
                r'Data \(kB\):? (?P<data_sz>\d+(?:\.\d+)?), ' + \
                r'Memory \(kB\):? (?P<mem_sz>\d+(?:\.\d+)?)' + \
                ')'
_META_DATA = r'(?:' + \
             r'(?P<meta_schema>\S+)\s+' + \
             r'(?P<meta_inst>\S+)\s+' + \
             r'(?P<meta_flags>\D+)\s+' + \
             r'(?P<meta_msize>\d+)\s+' + \
             r'(?P<meta_dsize>\d+)\s+' + \
             r'(?P<meta_uid>\d+)\s+' + \
             r'(?P<meta_gid>\d+)\s+' + \
             r'(?P<meta_perm>-(?:[r-][w-][x-]){3})\s+' + \
             r'(?P<meta_update>\d+\.\d+)\s+' + \
             r'(?P<meta_duration>\d+\.\d+)' + \
             r'(?:\s+(?P<meta_info>.*))?' + \
             r')'
_META_END = r'(?:^(?P<meta_end>[=]+)$)'
_LS_L_HDR = r'(?:(?P<set_name>[^:]+): .* last update: (?P<ts>.*))'
_LS_L_DATA = r'(?:(?P<F>.) (?P<type>\S+)\s+(?P<metric_name>\S+)\s+' \
             r'(?P<metric_value>.*))'
_LS_RE = re.compile(
            _META_BEGIN + "|" +
            _META_DASHES + "|" +
            _META_DATA + "|" +
            _META_SUMMARY + "|" +
            _META_END + "|" +
            _LS_L_HDR + "|" +
            _LS_L_DATA
         )
def int0(s):
    return int(s, base=0)
_TYPE_FN = {
    "char": lambda x: str(x).strip("'"),
    "char[]": lambda x: str(x).strip('"'),

    "u8": int0,
    "s8": int0,
    "u16": int0,
    "s16": int0,
    "u32": int0,
    "s32": int0,
    "u64": int0,
    "s64": int0,
    "f32": float,
    "d64": float,

    "u8[]": lambda x: list(map(int0, x.split(','))),
    "s8[]": lambda x: list(map(int0, x.split(','))),
    "u16[]": lambda x: list(map(int0, x.split(','))),
    "s16[]": lambda x: list(map(int0, x.split(','))),
    "u32[]": lambda x: list(map(int0, x.split(','))),
    "s32[]": lambda x: list(map(int0, x.split(','))),
    "u64[]": lambda x: list(map(int0, x.split(','))),
    "s64[]": lambda x: list(map(int0, x.split(','))),
    "f32[]": lambda x: list(map(float, x.split(','))),
    "d64[]": lambda x: list(map(float, x.split(','))),
}

def get_core_set_name(s):
    return s.partition('/')[2]

def parse_ldms_ls(txt):
    """Parse output of `ldms_ls -l [-v]` into { SET_NAME : SET_DICT } dict

    Each SET_DICT is {
        "name" : SET_NAME,
        "ts" : UPDATE_TIMESTAMP_STR,
        "meta" : {
            "schema"    :  SCHEMA_NAME,
            "instance"  :  INSTANCE_NAME,
            "flags"     :  FLAGS,
            "meta_sz"   :  META_SZ,
            "data_sz"   :  DATA_SZ,
            "uid"       :  UID,
            "gid"       :  GID,
            "perm"      :  PERM,
            "update"    :  UPDATE_TIME,
            "duration"  :  UPDATE_DURATION,
            "info"      :  APP_INFO,
        },
        "metrics" : {
            METRIC_NAME : (METRIC_TYPE, VALUE_TYPE, VALUE),
            ...
        },
        "data" : {
            METRIC_NAME : METRIC_VALUE,
            ...
        },
        "data_type" : {
            METRIC_NAME : METRIC_TYPE,
        },
    }
    """
    ret = dict()
    lines = txt.splitlines()
    itr = iter(lines)
    meta_section = False
    for l in itr:
        l = l.strip()
        if not l: # empty line, end of set
            lset = None
            data = None
            meta = None
            continue
        m = _LS_RE.match(l)
        if not m:
            raise RuntimeError("Bad line format: {}".format(l))
        m = m.groupdict()
        if m["meta_begin"]: # start meta section
            if meta_section:
                raise RuntimeError("Unexpected meta info: {}".format(l))
            meta_section = True
            continue
        elif m["meta_schema"]: # meta data
            if not meta_section:
                raise RuntimeError("Unexpected meta info: {}".format(l))
            meta = dict( schema = m["meta_schema"],
                         instance = get_core_set_name(m["meta_inst"]),
                         flags = m["meta_flags"],
                         meta_sz = m["meta_msize"],
                         data_sz = m["meta_dsize"],
                         uid = m["meta_uid"],
                         gid = m["meta_gid"],
                         perm = m["meta_perm"],
                         update = m["meta_update"],
                         duration = m["meta_duration"],
                         info = m["meta_info"],
                    )
            _set = ret.setdefault(get_core_set_name(m["meta_inst"]), dict())
            _set["meta"] = meta
            _set["name"] = get_core_set_name(m["meta_inst"])
        elif m["meta_total_sets"]: # the summary line
            if not meta_section:
                raise RuntimeError("Unexpected meta info: {}".format(l))
            # else do nothing
            continue
        elif m["meta_dashes"]: # dashes
            if not meta_section:
                raise RuntimeError("Unexpected meta info: {}".format(l))
            continue
        elif m["meta_end"]: # end meta section
            if not meta_section:
                raise RuntimeError("Unexpected meta info: {}".format(l))
            meta_section = False
            continue
        elif m["set_name"]: # new set
            if meta_section:
                raise RuntimeError("Unexpected data info: {}".format(l))
            metrics = dict() #placeholder for metrics
            data = dict() # placeholder for metric data
            data_type = dict() # placeholder for metric data type
            sname = m["set_name"].partition('/')[2]
            lset = ret.setdefault(get_core_set_name(m["set_name"]), dict())
            lset["ts"] = m["ts"]
            lset["metrics"] = metrics
            lset["data"] = data
            lset["data_type"] = data_type
        elif m["metric_name"]: # data
            if meta_section:
                raise RuntimeError("Unexpected data info: {}".format(l))
            if m["type"] == "char[]":
                _val = m["metric_value"]
            else:
                _val = m["metric_value"].split(' ', 1)[0] # remove units
            mname = m["metric_name"]
            mtype = m["type"]
            metrics[mname] = Metric(metric_type = m["F"],
                                    value_type = mtype,
                                    value = _val)
            data[mname] = _TYPE_FN[mtype](_val)
            data_type[mname] = mtype
        else:
            raise RuntimeError("Unable to process line: {}".format(l))
    return ret

schema_list = []

def add_schema(set_info):
    schema_name = set_info['meta']['schema']
    if schema_name in schema_list:
        return
    schema_list.append(schema_name)
    s = "config name=test_sampler action=add_schema schema={} metrics=".format(schema_name)
    count = 0
    for set_name in set_info['metrics'].keys():
        m = set_info['metrics'][set_name]
        if count > 0:
            s += ","
        s += "{name}:{mtype}:{vtype}:{value}".format(name = set_name,
                                                      mtype = m.metric_type,
                                                      vtype = m.value_type,
                                                      value = m.value)
        count += 1
    print(s)

def add_set(set_info):
    print("config name=test_sampler action=add_set "
          "instance={producer}/{set_name}".format(set_name = set_info['meta']['instance'],
                                                   producer = PRODUCER))

def config_producer():
    print("config name=test_sampler producer={producer}".format(producer = PRODUCER))

if __name__ == '__main__':
    try:
        parser = argparse.ArgumentParser(description="Parse ldms_ls -lv to the test_sampler config")
        parser.add_argument('--path', '-p', help = "Path to the ldms_ls result")
        parser.add_argument('--producer', '-P', help = "Producer name format. Default is ${HOSTNAME}_${PORT}")
        parser.add_argument('--interval', '-i', help = "Sample interval in seconds. Default is 1000000.")
        parser.add_argument('--offset', '-o', help = "Sample offset in seconds. Default is 0")
        args = parser.parse_args()

        if args.producer is not None:
            PRODUCER = args.producer
        if args.interval is not None:
            INTERVAL = args.interval
        if args.offset is not None:
            OFFSET = args.offset

        with open(args.path, 'r') as fin:
            txt = fin.read()
            result = parse_ldms_ls(txt)

        print("load name=test_sampler")
        for set_name in result.keys():
            add_schema(result[set_name])
        for set_name in result.keys():
            add_set(result[set_name])
        config_producer()
        print("start name=test_sampler interval={interval} offset={offset}".format(interval = INTERVAL, offset = OFFSET))
    except KeyboardInterrupt:
        sys.exit(0)
    except Exception as e:
        raise
