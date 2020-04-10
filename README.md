# officeAir - Temp and Humidity Meter and Google Sheet Logger with Raspberry Pi and I2C sensors

#enabling I2C interface 
1. sudo raspi-config
2. sudo apt install i2c-tools

#veryfication of I2C sensor:
3. sudo i2cdump -y 1 64
#should look like somthing like this:
#...
#e0: 00 00 00 69 00 48 00 3a 00 00 00 00 00 00 7f 00    ...i.H.:......?.
#f0: 00 00 00 XX XX XX XX XX XX XX XX XX XX XX XX XX    ...XXXXXXXXXXXXX

#creating log script to execute I2C sensor call and google web app data upload:
#/bins/sh
DATA=`sht21.py`
curl -L "https://script.google.com/macros/s/__Google script deployed web app key here__/exec?$DATA"



#modifying crontab to execute the script every minuite
sudo nano /etc/crontab
*  *    * * *   root  /usr/local/bin/log #log air quality
