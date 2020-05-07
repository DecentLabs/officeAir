# officeAir - Temp and Humidity Meter and Google Sheet Logger with Raspberry Pi and I2C sensors

## enabling I2C interface at Raspberry Pi
1. `sudo raspi-config`

![raspi-config](https://github.com/DecentLabs/officeAir/blob/master/1_raspi-config_intef_options.png)

![Interface Options](https://github.com/DecentLabs/officeAir/blob/master/2_raspi-config_intef_options_i2c.png)

![enable I2C](https://github.com/DecentLabs/officeAir/blob/master/2_raspi-config_intef_options_i2c.png)

2. `sudo apt install i2c-tools`

## veryfication of I2C sensor:
3. `sudo i2cdump -y 1 64`
#should look like somthing like this:
![I2C map](https://github.com/DecentLabs/officeAir/blob/master/4_i2cdump_map.png)


## creating log script to execute I2C sensor call and google web app data upload:
    sudo nano log
    
# copy paste this into log, take care to modify the _web app key_ to your deployed one
    #/bins/sh
    DATA=`sht21.py`
    curl -L "https://script.google.com/macros/s/__Google script deployed web app key here__/exec?$DATA"

### set rights
    sudo chmod +x log
    
 ## copy the python script into the proper folder:
    cd /usr/local/bin/log
    sudo wget https://github.com/DecentLabs/officeAir/blob/master/sht21.py
    sudo chmod +x sht21.py

## modifying crontab to execute the script every minuite
    sudo nano /etc/crontab

### and copy paste to the last line this:

    *  *    * * *   root  /usr/local/bin/log #log air quality
    
## You can take this example Google sheet (copy it to yourself and format for your needs)


    
    https://docs.google.com/spreadsheets/d/1NwUGx-ZrcANtIkeNKTUgpd7Sba6uL-PD1U4rKH_r9Pc/edit?usp=sharing

    
    
