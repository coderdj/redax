import blosc
import numpy as np
import strax
import matplotlib.pyplot as plt

path = '/home/coderre/trigger_buffer/301/000000/fdaq00'
#path = '/home/coderre/eventbuilder/testdata/from_fake_daq/000000/reader_0'


f = open(path, "rb")
data = blosc.decompress(f.read())
darr = np.frombuffer(data, dtype=strax.record_dtype())

def plot_waveform(data, baseline, integral):
    thresh = 500
    plt.figure()
    plt.plot(range(len(data)), data)
    plt.plot([0, len(data)], [baseline, baseline], lineStyle='--', c='r', label='baseline')
    plt.plot([0, len(data)], [baseline-thresh, baseline-thresh], lineStyle='--', c='g', label='threshold')
    plt.legend()
    plt.xlabel("bins (10ns)")
    plt.ylabel("ADC value")
    plt.show()

direction = 'f'
integrals = []
for i in range(0, len(darr)):
    if darr[i][6]!=0:
        if direction == 'f':
            continue
        else:
            i-=2
            continue
        
    # Print some stuff about the channel
    '''
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
    '''
    
    data = []
    data.extend(darr[i][9][:darr[i][3]])
    thisi = darr[i][6]+1
    while len(data) < darr[i][5]:
        #print("Channel: %i"%darr[i+thisi][0])
        #print("Time resolution: %i ns"%darr[i+thisi][1])
        #print("Timestamp: %i"%darr[i+thisi][2])
        #print("Interval length: %i samples"%darr[i+thisi][3])
        #print("Integral: %i"%darr[i+thisi][4])
        #print("Pulse length: %i samples"%darr[i+thisi][5])
        #print("Fragment in pulse: %i"%darr[i+thisi][6])
        #print("Baseline: %i"%darr[i+thisi][7])
        #print("Reduction level: %i"%darr[i+thisi][8])
        #print("Payload (%i): %s"%(len(darr[i][9]), str(darr[i][9])))                                    
        if i%1000==0:
            print("Record %i/%i shown."%(i+thisi, len(darr)))

        data.extend(darr[i+thisi][9][:darr[i+thisi][3]])
        thisi+=1

    baseline = float(sum(data[:20]))/20.
    integral = (baseline*float(len(data)))-float(sum(data))

    integrals.append(integral)

    if integral > 10000:# and integral < 20100:
        plot_waveform(data, baseline, integral)
        
#print(data)
plt.figure()
plt.hist(integrals, bins=np.arange(-500, 500000, 500))
#plt.xlabel("Sample (10ns)")
#plt.ylabel("ADC value")
plt.show()

