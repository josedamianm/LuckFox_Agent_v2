import subprocess, time, os

GUI_BIN    = "/root/Executables/luckfox_gui"
CAM_DAEMON = "/root/Executables/camera_daemon"
SERVER     = "/mnt/sdcard/http_api_server_v2.py"

if os.path.isfile(GUI_BIN):
    subprocess.Popen(
        [GUI_BIN],
        stdout=open("/tmp/luckfox_gui.log", "w"),
        stderr=subprocess.STDOUT
    )
    time.sleep(1.5)

if os.path.isfile(CAM_DAEMON):
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = "/oem/usr/lib:/usr/lib"
    subprocess.Popen(
        [CAM_DAEMON, "10"],
        env=env,
        stdout=open("/tmp/camera_daemon.log", "w"),
        stderr=subprocess.STDOUT
    )
    time.sleep(8)

subprocess.Popen(
    ["python3", SERVER],
    stdout=open("/tmp/http_api_server_v2.log", "w"),
    stderr=subprocess.STDOUT
)
