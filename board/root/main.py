import subprocess, time, os

GUI_BIN = "/root/Executables/luckfox_gui"
SERVER  = "/mnt/sdcard/http_api_server_v2.py"

if os.path.isfile(GUI_BIN):
    subprocess.Popen(
        [GUI_BIN],
        stdout=open("/tmp/luckfox_gui.log", "w"),
        stderr=subprocess.STDOUT
    )
    time.sleep(1.5)

subprocess.Popen(
    ["python3", SERVER],
    stdout=open("/tmp/http_api_server_v2.log", "w"),
    stderr=subprocess.STDOUT
)
