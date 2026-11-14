"""Read-only esptool CLI with one unacknowledged READ_FLASH packet.

Process-local patch only. Keeps the installed read_flash implementation,
including its frame-length and final MD5 checks, byte-for-byte unchanged.
Protocol: https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/serial-protocol.html#read-flash
"""
from contextlib import contextmanager
from functools import wraps
import struct
import sys
import esptool
from esptool.loader import ESPLoader

VALUE_OPTIONS={'--chip','--port','--baud','--before','--after','--connect-attempts','-c','-p','-b'}
FLAG_OPTIONS={'--no-stub','--trace'}
FORBIDDEN={'write_flash','erase_flash','erase_region','write_mem','write_flash_status','run','load_ram','burn_efuse','burn_key'}

def validate_read_only(argv):
    if any(x.startswith('@') for x in argv):raise ValueError('Argument files are not allowed')
    at=0
    while at<len(argv) and argv[at].startswith('-'):
        option=argv[at]
        if option in VALUE_OPTIONS:
            if at+1>=len(argv):raise ValueError('Missing value for '+option)
            at+=2
        elif option.split('=',1)[0] in VALUE_OPTIONS and '=' in option:at+=1
        elif option in FLAG_OPTIONS:at+=1
        else:raise ValueError('Unsupported global option: '+option)
    if at>=len(argv) or argv[at] not in {'read_flash','flash_id'}:
        raise ValueError('Only read_flash and flash_id are allowed; no Flash writes or erase')
    command=argv[at];tail=argv[at+1:]
    if any(x in FORBIDDEN for x in tail):raise ValueError('Write/erase commands are forbidden')
    if command=='flash_id':
        if tail:raise ValueError('flash_id accepts no additional command or operands')
    else:
        operands=[x for x in tail if x!='--no-progress']
        if len(operands)!=3 or any(x.startswith('-') for x in operands):
            raise ValueError('read_flash requires offset length output-file')
        try:
            offset=int(operands[0],0);length=int(operands[1],0)
        except ValueError:raise ValueError('Explicit numeric offset and length are required') from None
        if offset<0 or length<=0 or offset+length>0x100000000:raise ValueError('Invalid read range')
    return command

def window1_check(original):
    @wraps(original)
    def wrapped(self,op_description,op=None,data=b'',*args,**kwargs):
        if op==self.ESP_READ_FLASH and len(data)==16:
            # First three little-endian words remain exactly as sent by esptool.
            data=data[:12]+struct.pack('<I',1)
        return original(self,op_description,op,data,*args,**kwargs)
    return wrapped

@contextmanager
def one_packet_window():
    original=ESPLoader.check_command
    ESPLoader.check_command=window1_check(original)
    try:yield
    finally:ESPLoader.check_command=original

def main(argv=None):
    argv=list(sys.argv[1:] if argv is None else argv)
    validate_read_only(argv)
    with one_packet_window():return esptool.main(argv)

if __name__=='__main__':
    try:main()
    except ValueError as error:
        print('READ_ONLY_GUARD: '+str(error),file=sys.stderr);sys.exit(2)
