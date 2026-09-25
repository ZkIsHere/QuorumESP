#!/bin/bash
# Stop the WSL test qdevice client, verify it's gone, then probe the ESP32.
pkill -f "corosync-qdevice -f" --full 2>/dev/null
# NOTE: pkill -f above cannot match this script (different cmdline).
sleep 8
echo "qdevice procs left: $(ps aux | grep '[c]orosync-qdevice' | wc -l)"
python3 /mnt/d/PersonalProject/QuorumESP/host/tools/preinit-probe.py
