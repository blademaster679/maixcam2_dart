"""nl80211 cached power state; current AIC callback is a no-op, not firmware proof."""
import socket,struct,os

def attrs(data):
 out={};at=0
 while at+4<=len(data):
  size,kind=struct.unpack_from('HH',data,at)
  if size<4 or at+size>len(data):raise ValueError('invalid netlink attribute')
  out[kind&0x3fff]=data[at+4:at+size];at+=(size+3)&~3
 return out

def attr(kind,value):
 data=struct.pack('HH',len(value)+4,kind)+value
 return data+b'\0'*((-len(data))%4)

class Power:
 def __init__(self,interface='wlan0'):
  self.s=socket.socket(socket.AF_NETLINK,socket.SOCK_RAW,16);self.s.bind((0,0));self.s.settimeout(3);self.seq=0
  reply=self.request(16,3,attr(2,b'nl80211\0'))
  self.family=struct.unpack('H',attrs(reply[0][4:])[1])[0]
  self.interface=attr(3,struct.pack('I',socket.if_nametoindex(interface)))
 def request(self,family,cmd,data):
  self.seq+=1;payload=struct.pack('BBH',cmd,1,0)+data
  self.s.send(struct.pack('IHHII',16+len(payload),family,5,self.seq,0)+payload);out=[]
  while True:
   packet=self.s.recv(65536);at=0
   while at+16<=len(packet):
    n,kind,flags,seq,pid=struct.unpack_from('IHHII',packet,at)
    if n<16:raise ValueError('invalid netlink message')
    data=packet[at+16:at+n];at+=(n+3)&~3
    if seq!=self.seq:continue
    if kind==2:
     err=struct.unpack_from('i',data)[0]
     if err:raise OSError(-err,os.strerror(-err))
     return out
    out.append(data)
 def get(self):return struct.unpack('I',attrs(self.request(self.family,62,self.interface)[0][4:])[93])[0]
 def set(self,enabled):
  self.request(self.family,61,self.interface+attr(93,struct.pack('I',int(enabled))))
  return self.get()

if __name__=='__main__':
 import argparse
 p=argparse.ArgumentParser();p.add_argument('--set',choices=['0','1']);a=p.parse_args();n=Power()
 print(n.get() if a.set is None else n.set(int(a.set)))
