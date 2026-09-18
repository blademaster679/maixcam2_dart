import os,fcntl,ctypes,struct
class Transfer(ctypes.Structure):
    _fields_=[('tx_buf',ctypes.c_uint64),('rx_buf',ctypes.c_uint64),('len',ctypes.c_uint32),('speed_hz',ctypes.c_uint32),('delay_usecs',ctypes.c_uint16),('bits_per_word',ctypes.c_uint8),('cs_change',ctypes.c_uint8),('tx_nbits',ctypes.c_uint8),('rx_nbits',ctypes.c_uint8),('word_delay_usecs',ctypes.c_uint8),('pad',ctypes.c_uint8)]
class SPI:
    def __init__(self,path='/dev/spidev1.0',speed=1000000):
        self.fd=os.open(path,os.O_RDWR);self.speed=speed
        self.saved=[]
        for request,n in [(0x80016b01,1),(0x80016b03,1),(0x80046b04,4)]:
            b=bytearray(n);fcntl.ioctl(self.fd,request,b,True);self.saved.append(bytes(b))
        fcntl.ioctl(self.fd,0x40016b01,b'\x00');fcntl.ioctl(self.fd,0x40016b03,b'\x08')
    def xfer(self,data):
        tx=ctypes.create_string_buffer(bytes(data),len(data));rx=ctypes.create_string_buffer(len(data))
        t=Transfer(ctypes.addressof(tx),ctypes.addressof(rx),len(data),self.speed,0,8,0,0,0,0,0)
        fcntl.ioctl(self.fd,0x40206b00,bytes(t));return rx.raw
    def read(self,addr,n=1):return self.xfer(bytes([addr|128])+bytes(n))[1:]
    def write(self,addr,value):self.xfer(bytes([addr&127,value]))
    def close(self):
        for req,data in zip([0x40016b01,0x40016b03,0x40046b04],self.saved):fcntl.ioctl(self.fd,req,data)
        os.close(self.fd)
