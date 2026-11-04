import struct, zlib, random, sys
random.seed(7)
ROM=0x200000
rom=bytearray(random.randrange(256) for _ in range(ROM))
rom[0:4]=bytes([0x80,0x37,0x12,0x40])
rom[0x3B:0x40]=b'NZLE\x01'
def yaz0_lit(d):
    out=bytearray(b'Yaz0'+struct.pack(">I",len(d))+b'\0'*8)
    for i in range(0,len(d),8):
        ch=d[i:i+8]
        out.append(0xFF>>(8-len(ch))<<(8-len(ch)) if len(ch)<8 else 0xFF)
        out+=ch
    return bytes(out)
# arquivos: A (nao comprimido), B (yaz0), C vazio mesmo vrom que B, D inexistente
A=bytes(random.randrange(256) for _ in range(0x100))
B=bytes(random.randrange(256) for _ in range(0x203))
Bc=yaz0_lit(B)
rom[0x10000:0x10100]=A
rom[0x20000:0x20000+len(Bc)]=Bc
ents=[(0x1000,0x1040,0x1000,0), (0x2000,0x2100,0x10000,0), (0x3000,0x3000,0x20000,0x20000), (0x3000,0x3000+len(B),0x20000,0x20000+len(Bc)),(0x9000,0x9100,0xFFFFFFFF,0xFFFFFFFF),(0,0,0,0)]
# dmadata em 0x1000 (a propria entrada 0)
d=b''.join(struct.pack(">IIII",*e) for e in ents)
rom[0x1000:0x1000+len(d)]=d
open("rom.z64","wb").write(rom)
open("dma.txt","w").write("0x1000 0x%x\n"%len(d))
# mapa: A: vrom 0x2000 codes por meia palavra; B vrom 0x3000
def codes(n):
    cd=[];i=0
    while i<(n+1)//2:
        r=random.choice([0,1,2,0])
        if r==2 and i+1<(n+1)//2 and (i+1)*2+2<=n: cd+=[2,3];i+=2
        else: cd.append(0 if r==2 else r);i+=1
    return cd
def apply(data,cd):
    data=bytearray(data);i=0
    while i<len(cd):
        c=cd[i];p=i*2
        if c==1 and p+2<=len(data): data[p],data[p+1]=data[p+1],data[p]
        elif c==2: data[p:p+4]=data[p:p+4][::-1];i+=1
        i+=1
    return bytes(data)
body=b'';exp={}
for vrom,data in ((0x2000,A),(0x3000,B)):
    cd=codes(len(data));packed=bytearray((len(cd)+3)//4)
    for i,x in enumerate(cd): packed[i//4]|=x<<(2*(i%4))
    body+=struct.pack("<III",vrom,len(data),len(cd))+bytes(packed)
    exp[vrom]=apply(data,cd)
raw=struct.pack("<I",2)+body
c=zlib.compressobj(9,zlib.DEFLATED,-15);df=c.compress(raw)+c.flush()
open("map.bin","wb").write(b"OOTSWP1\0"+struct.pack("<II",len(raw),zlib.crc32(raw)&0xFFFFFFFF)+df)
open("exp_A.bin","wb").write(exp[0x2000]);open("exp_B.bin","wb").write(exp[0x3000])
