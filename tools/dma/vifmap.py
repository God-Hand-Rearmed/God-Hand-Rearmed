#!/usr/bin/env python3
"""What the frame's VIF1 chain carries, per VU1 microcode image and MSCAL / MSCNT (claude/native-next.md rung 3).

Walks the ordering table's chain (head at 0x754C80 + 128 x buffer; the finished buffer = the one the main loop is not
filling, word 0x7476B0 & 1) with TTE in capture A, parses the VIF codes (STCYCL, UNPACK sizes, DIRECT, MPG, MSCAL /
MSCNT) and sums the bytes each program's runs read, split by tag kind (REF / CNT-NEXT) and by whether the same bytes
are in capture B too ("static" = model data as loaded; "changes" = written per frame).

    python3 tools/dma/vifmap.py <ram_A.bin> <ram_B.bin>      (two F8 captures of the same scene, K's play folder)
"""
import struct,sys,collections,bisect
A=open(sys.argv[1],"rb").read(); B=open(sys.argv[2],"rb").read()
def walk(ram,which):
    pieces=[];tag=struct.unpack_from('<I',ram,0x754C80+which*128)[0];asp=0;asr=[0,0]
    while True:
        a=tag&0x1FFFFFF;q=struct.unpack_from('<Q',ram,a)[0]
        qwc=q&0xFFFF;i=(q>>28)&7;addr=(q>>32)&0x7FFFFFFF;end=False
        if i==0:data=addr;tag+=16;end=True
        elif i==1:data=tag+16;tag=data+qwc*16
        elif i==2:data=tag+16;tag=addr
        elif i in(3,4):data=addr;tag+=16
        elif i==5:data=tag+16;asr[asp]=data+qwc*16;asp+=1;tag=addr
        elif i==6:
            data=tag+16
            if asp:asp-=1;tag=asr[asp]
            else:end=True
        else:data=tag+16;end=True
        pieces.append((a+8,8,a,i))
        if qwc: pieces.append((data&0x1FFFFFF,qwc*16,a,i))
        if end:break
    return pieces
ram=A
b=struct.unpack_from('<I',ram,0x7476B0)[0]&1
pieces=walk(ram,1-b)
stream=bytearray();starts=[]
for p in pieces: starts.append(len(stream)); stream+=ram[p[0]:p[0]+p[1]]
def src(off):
    k=bisect.bisect_right(starts,off)-1; p=pieces[k]; return p[0]+off-starts[k],p
off=0;cl=wl=1;N=len(stream);pend=[];image=None;img_at={}
stats=collections.defaultdict(lambda:collections.Counter())
runs=collections.Counter()
def static(addr,n): return A[addr:addr+n]==B[addr:addr+n]
while off+4<=N:
    code=struct.unpack_from('<I',stream,off)[0];cmd=(code>>24)&0x7F;imm=code&0xFFFF;num=(code>>16)&0xFF
    off+=4
    if cmd==1: cl=imm&0xFF;wl=(imm>>8)&0xFF
    elif cmd==0x20: off+=4
    elif cmd in(0x30,0x31): off+=16
    elif cmd==0x4A:
        n=(num or 256)*8; s,p=src(off); 
        if imm*8<0x700: image=hex(s)
        off+=n
    elif cmd in(0x50,0x51):
        n=(imm or 65536)*16; s,p=src(off); pend.append(('direct',s,n,p[3])); off+=n
    elif cmd>=0x60:
        vn=(cmd>>2)&3;vl=cmd&3;nn=num or 256
        bits=(32>>vl)*(vn+1) if not(vn==3 and vl==3) else 16
        cnt=nn if wl<=cl else cl*(nn//wl)+min(nn%wl,cl)
        n=((cnt*bits+31)//32)*4; s,p=src(off); pend.append(('unpack',s,n,p[3])); off+=n
    elif cmd in(0x14,0x15,0x17):
        key=(image, 'MSCNT' if cmd==0x17 else 'MSCAL %04X'%(imm*8))
        runs[key]+=1
        for u in pend:
            st=static(u[1],u[2])
            stats[key][(u[0],'REF' if u[3] in(3,4,0) else 'CNT/NEXT' if u[3] in(1,2) else 'other', 'static' if st else 'changes')]+=u[2]
        pend=[]
for k in sorted(stats,key=lambda x:-sum(stats[x].values())):
    print(k,'runs',runs[k],'bytes',sum(stats[k].values()))
    for kk,v in stats[k].most_common(8): print('   ',kk,v)
