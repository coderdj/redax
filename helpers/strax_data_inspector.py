import blosc
import numpy as np
import strax

#path = '/home/coderre/trigger_buffer/263/000000/fdaq00'
path = '/home/coderre/eventbuilder/testdata/from_fake_daq/000000/reader_0'

f = open(path, "rb")
data = blosc.decompress(f.read())
darr = np.frombuffer(data, dtype=strax.record_dtype())

for i in range(0, len(darr)):
    #print(darr[i])
    print("Channel: %i"%darr[i][0])
    print("Time resolution: %i ns"%darr[i][1])
    print("Timestamp: %i"%darr[i][2])
    print("Interval length: %i samples"%darr[i][3])
    print("Integral: %i"%darr[i][4])
    print("Pulse length: %i samples"%darr[i][5])
    print("Fragment in pulse: %i"%darr[i][6])
    print("Baseline: %i"%darr[i][7])
    print("Reduction level: %i"%darr[i][8])
    print("Payload (%i): %s"%(len(darr[i][9]), str(darr[i][9])))
    print("Record %i/%i shown."%(i, len(darr)))
    inp = input("(p)revious or (n)ext record. Or (s)kip ahead 100")
    if inp == 'p':
        i-=2
    elif inp == 'n':
        continue
    elif inp == 's':
        i+=99
    else:
        i-=1
