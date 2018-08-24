import blosc
import numpy as np
import strax
import matplotlib.pyplot as plt

#path = '/home/coderre/trigger_buffer/307/000000/fdaq00'
path = '/mongodb/strax_output/307/000000/fdaq00_reader_0'
#path = '/home/coderre/eventbuilder/testdata/from_fake_daq/000000/reader_0'

f = open(path, "rb")
data = blosc.decompress(f.read())
darr = np.frombuffer(data, dtype=strax.record_dtype())

direction = 'f'

for i in range(0, len(darr)):
    if darr[i][6]!=0:
        if direction == 'f':
            continue
        else:
            i-=2
            continue
        
    # Print some stuff about the channel
    print("Channel: %i"%darr[i][0])
    print("Time resolution: %i ns"%darr[i][1])
    print("Timestamp: %i"%darr[i][2])
    print("Interval length: %i samples"%darr[i][3])
    print("Integral: %i"%darr[i][4])
    print("Pulse length: %i samples"%darr[i][5])
    print("Fragment in pulse: %i"%darr[i][6])
    print("Baseline: %i"%darr[i][7])
    print("Reduction level: %i"%darr[i][8])
    #print("Payload (%i): %s"%(len(darr[i][9]), str(darr[i][9])))
    print("Record %i/%i shown."%(i, len(darr)))

    
    data = []
    data.extend(darr[i][9][:darr[i][3]])
    thisi = darr[i][6]+1
    while len(data) < darr[i][5]:
        print("Channel: %i"%darr[i+thisi][0])
        print("Time resolution: %i ns"%darr[i+thisi][1])
        print("Timestamp: %i"%darr[i+thisi][2])
        print("Interval length: %i samples"%darr[i+thisi][3])
        print("Integral: %i"%darr[i+thisi][4])
        print("Pulse length: %i samples"%darr[i+thisi][5])
        print("Fragment in pulse: %i"%darr[i+thisi][6])
        print("Baseline: %i"%darr[i+thisi][7])
        print("Reduction level: %i"%darr[i+thisi][8])
        #print("Payload (%i): %s"%(len(darr[i][9]), str(darr[i][9])))                                    
        print("Record %i/%i shown."%(i+thisi, len(darr)))

        data.extend(darr[i+thisi][9][:darr[i+thisi][3]])
        thisi+=1
    #print(data)
    plt.figure()
    plt.plot(np.arange(0, len(data)), data)
    plt.xlabel("Sample (10ns)")
    plt.ylabel("ADC value")
    plt.show()

    

    inp = input("(p)revious or (n)ext record. Or (s)kip ahead 100")
    if inp == 'p':
        i-=2
        direction = 'b'
    elif inp == 'n':
        direction = 'f'
        continue
    elif inp == 's':
        direction = 'f'
        i+=99
    else:
        i-=1
