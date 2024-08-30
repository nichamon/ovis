import json
import pandas as pd

def probe_as_json(comm):
    o = comm.xprt_probe()
    return json.loads(o[1])

def lookup_df(host_probe):
    df = pd.DataFrame(host_probe['LOOKUP'])
    return df
