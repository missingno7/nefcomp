import struct, sys
TYPES={1:1,2:1,3:2,4:4,5:8,6:1,7:1,8:2,9:4,10:8,11:4,12:8,13:4}
def rd(f,base,off,bo):
    pass
class T:
    def __init__(s,data,base=0,bo=None):
        s.d=data; s.base=base
        if bo is None:
            bo = '<' if data[base:base+2]==b'II' else '>'
        s.bo=bo
    def u16(s,o): return struct.unpack_from(s.bo+'H',s.d,s.base+o)[0]
    def u32(s,o): return struct.unpack_from(s.bo+'I',s.d,s.base+o)[0]
    def ifd(s,o):
        n=s.u16(o); ents=[]
        for i in range(n):
            e=o+2+12*i
            tag,typ,cnt=s.u16(e),s.u16(e+2),s.u32(e+4)
            sz=TYPES.get(typ,1)*cnt
            voff = e+8 if sz<=4 else s.u32(e+8)
            ents.append((tag,typ,cnt,voff))
        nxt=s.u32(o+2+12*n)
        return ents,nxt
    def val(s,typ,cnt,voff,maxn=16):
        o=s.base+voff
        if typ==2: return s.d[o:o+min(cnt,80)].split(b'\0')[0]
        fmt={1:'B',3:'H',4:'I',6:'b',8:'h',9:'i',7:'B',11:'f',12:'d',13:'I'}
        if typ in (5,10):
            c='I' if typ==5 else 'i'
            return [struct.unpack_from(s.bo+c+c,s.d,o+8*i) for i in range(min(cnt,maxn))]
        c=fmt.get(typ,'B'); sz=struct.calcsize(c)
        return list(struct.unpack_from(s.bo+c*min(cnt,maxn),s.d,o))
def dump(t,o,ind='',subtags=(0x14a,0x8769,0x927c),name='IFD'):
    ents,nxt=t.ifd(o)
    print(f"{ind}{name} @0x{o:x} ({len(ents)} entries)")
    for tag,typ,cnt,voff in ents:
        v=t.val(typ,cnt,voff)
        print(f"{ind}  0x{tag:04x} t{typ} n{cnt} @0x{voff:x}: {v}")
        if tag in (0x14a,0x8769) :
            offs=t.val(typ,cnt,voff)
            for so in offs: dump(t,so,ind+'    ',name=f'Sub{tag:x}')
        if tag==0x927c and t.d[t.base+voff:t.base+voff+6]==b'Nikon\0':
            mt=T(t.d,t.base+voff+10)
            dump(mt,mt.u32(4),ind+'    ',name='NikonMN')
    return nxt
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read(); t=T(d)
    o=t.u32(4)
    while o:
        o=dump(t,o)
