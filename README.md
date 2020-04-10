# officeAir - Temp and Humidity Meter and Google Sheet Logger with Raspberry Pi and I2C sensors

## enabling I2C interface 
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
    #/bins/sh
    DATA=`sht21.py`
    curl -L "https://script.google.com/macros/s/__Google script deployed web app key here__/exec?$DATA"



## modifying crontab to execute the script every minuite
```sudo nano /etc/crontab
*  *    * * *   root  /usr/local/bin/log #log air quality```
