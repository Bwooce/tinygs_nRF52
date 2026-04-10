#!/usr/bin/env python3
import serial
import time
import os
import subprocess
import sys

def trigger_dfu(port):
    print(f"Attempting to trigger DFU mode on {port}...")
    try:
        # Open at 1200 baud to trigger bootloader
        ser = serial.Serial(port, 1200)
        ser.close()
        time.sleep(3)
        print("Trigger signal sent.")
    except Exception as e:
        print(f"Error triggering DFU: {e}")

def find_mount_point(label="HT-n5262"):
    print(f"Searching for drive with label containing '{label}'...")
    try:
        # Try to find the mount point using lsblk
        output = subprocess.check_output(['lsblk', '-no', 'MOUNTPOINTS,LABEL']).decode()
        for line in output.splitlines():
            # Check for the label anywhere in the line
            if label.upper() in line.upper():
                parts = line.strip().split()
                if parts:
                    # The first part is the mountpoint if it exists
                    # lsblk -no MOUNTPOINTS,LABEL output is like "/media/bruce/DRIVE LABEL"
                    # We might need to be careful with spaces in labels
                    mount_path = line[:line.find(label)].strip()
                    if mount_path and os.path.isdir(mount_path):
                        return mount_path
    except Exception as e:
        print(f"Error finding mount point: {e}")
    
    # Fallback: check standard linux mount path
    standard_path = f"/media/bruce/{label}"
    if os.path.isdir(standard_path):
        return standard_path
        
    return None

if __name__ == "__main__":
    tty_port = "/dev/ttyACM0"
    uf2_file = "/home/bruce/dev/tinygs_nRF52/build/zephyr/zephyr.uf2"
    
    if os.path.exists(tty_port):
        trigger_dfu(tty_port)
    else:
        print(f"Port {tty_port} not found, checking if already in DFU mode...")
    
    print("Waiting for device to mount...")
    mount_point = None
    for i in range(15):
        mount_point = find_mount_point()
        if mount_point:
            break
        print(f"Retrying... ({i+1}/15)")
        time.sleep(1)
    
    if mount_point:
        print(f"Found mount point: {mount_point}")
        print(f"Copying {uf2_file} to {mount_point}...")
        subprocess.run(['cp', uf2_file, mount_point])
        subprocess.run(['sync'])
        print("Flash successful!")
    else:
        print("Could not find mounted HT-n5262 drive.")
        print("Please double-tap the RST button on the board.")
        sys.exit(1)
