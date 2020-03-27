# Connect GDB serial to RPi: pin 5 black, 7 yellow, 9 red
source sdk/alpha.gdb

set serial baud 921600
target remote COM7
#target remote /dev/ttyUSB0

# Load the executable in the target
# By default, the loaded file is the one GDB debugs, given as argument
# to gdb or using the command file.
load

# Run untile reaching main
# tbreak main
break gdb_break
commands 1
  kill
  quit
end
continue
