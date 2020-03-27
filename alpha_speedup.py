# Utility for RPi Alpha to increase remote GDB baud rate
# From iosoft.blog, copyright (c) Jeremy P bentham 2020
# Requires pyserial package

import sys, serial, time

# Defaults
serport  = "COM7"
verbose  = False

# Default settings
OLD_BAUD    = 115200
new_baud    = 921600
TIMEOUT     = 0.2
SYS_CLOCK   = 250e6

# GDB remote commands
high_speed  = "mw32 0x20215068 %u"
qsupported  = "qSupported"

# Send command, return response
def cmd_resp(ser, cmd):
    txd = frame(cmd)
    if verbose:
        print("Tx: %s" % txd)
    ser.write(txd.encode('latin'))
    rxd = str(ser.read(1468))
    if verbose:
        print("Rx: %s" % rxd)
    resp = rxd.partition('$')
    return resp[2].partition('#')[0]

# Acknowledge a response
def ack_resp(ser):
    ser.write('+'.encode('latin'))
    if verbose:
        print("Tx: +")

# Return string, given hex values
def hex_str(hex):
    return bytearray.fromhex(hex).decode()

# Return remote hex command string
def cmd_hex(cmd):
    return "qRcmd,%s" % "".join([("%02x" % ord(c)) for c in cmd])

# Return framed data
def frame(data):
    return "$%s#%02x" % ("".join([escape(c) for c in data]), csum(data))

# Escape a character in the message
def escape(c):
    return c if c not in "#$}" else '}'+chr(ord(c)^0x20)

# GDB checksum calculation
def csum(data):
    return 0xff & sum([ord(c) for c in data])

# Open serial port
def ser_open(port, baud):
    try:
        ser = serial.Serial(port, baud, timeout=TIMEOUT)
    except:
        print("Can't open serial port %s" % port)
        sys.exit(1)
    return ser
        
# Close serial port
def ser_close(ser):
    if ser:
        ser.close()

if __name__ == "__main__":
    opt = None
    for arg in sys.argv[1:]:
        if len(arg)==2 and arg[0]=="-":
            opt = arg.lower()
            if opt == "-v":
                verbose = True
                opt = None
        elif opt == '-b':
            new_baud = int(arg)
            opt = None
        elif opt == '-c':
            serport = arg
            opt = None
    print("Opening serial port %s at %u baud" % (serport, OLD_BAUD))
    ser = ser_open(serport, OLD_BAUD);
    cmd_resp(ser, "")
    ack_resp(ser)
    if cmd_resp(ser, qsupported):
        ack_resp(ser)
        print("Setting %u baud" % new_baud)
        uart_div = int(round((SYS_CLOCK / (8 * new_baud)) - 1))
        cmd_resp(ser, cmd_hex(high_speed % uart_div))
    time.sleep(0.01)
    print("Reopening at %u baud" % new_baud)
    ser_close(ser)
    ser = ser_open(serport, new_baud);
    ack_resp(ser)
    if cmd_resp(ser, qsupported):
        ack_resp(ser)
        print("Target system responding OK")
        time.sleep(0.01)
    else:
        print("No response from target system")
#EOF