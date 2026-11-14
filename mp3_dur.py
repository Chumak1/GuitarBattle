import struct, sys
def frames(path):
    data = open(path,'rb').read()
    i = 0
    if data[:3] == b'ID3':
        size = ((data[6]&0x7f)<<21)|((data[7]&0x7f)<<14)|((data[8]&0x7f)<<7)|(data[9]&0x7f)
        i = 10 + size
    brs = {1:[0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0]}
    srs = {3:[44100,48000,32000,0]}
    while i < len(data)-4:
        if data[i] == 0xFF and (data[i+1] & 0xE0) == 0xE0:
            ver = (data[i+1]>>3)&3; layer=(data[i+1]>>1)&3
            bri = (data[i+2]>>4)&0xF; sri=(data[i+2]>>2)&3
            if ver==3 and layer==1 and 0<bri<15 and sri<3:
                br = brs[1][bri]*1000; sr=srs[3][sri]
                payload = len(data)-i
                return br, sr, payload*8/br
        i += 1
    return None
for name in ['drums.mp3','bass.mp3','guitar.mp3','others.mp3']:
    r = frames(r'C:\Users\KK\AppData\Roaming\GuitarBattle\OnlineSongs\the-lat-kingdom\%s' % name)
    print(name, 'bitrate=%d samplerate=%d duration=%.2fs' % r if r else 'unparsed')
