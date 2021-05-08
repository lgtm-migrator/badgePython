freeze("$(MPY_DIR)/tools", ("upip.py", "upip_utarfile.py"))
freeze("$(MPY_DIR)/drivers/dht", "dht.py")
freeze("$(MPY_DIR)/drivers/onewire")
include("$(MPY_DIR)/extmod/uasyncio/manifest.py")
include("$(MPY_DIR)/extmod/webrepl/manifest.py")
freeze("../python_modules/campzone2020")
freeze("../python_modules/common", ("upysh.py", "valuestore.py", "_buttons.py", "shell.py", "term.py", "virtualtimers.py", "wifi.py", "system.py", "ntp.py"))
freeze("../python_modules/common", "tasks/powermanagement.py")
freeze("../python_modules/common", "tasks/otacheck.py")
freeze("../python_modules/common", "umqtt")
freeze("../python_modules/woezel")