# officeAir - Temp and Humidity Meter and Google Sheet Logger with Raspberry Pi and I2C sensors

First lets see the Google sheet and its script side to prepare that before Raspberry pi configurations.

## You can take this example Google sheet (make a copy to yourself and format for your needs)

[example google sheet](https://docs.google.com/spreadsheets/d/1NwUGx-ZrcANtIkeNKTUgpd7Sba6uL-PD1U4rKH_r9Pc/edit?usp=sharing)

Here is a chart for temperature and humidity for several days:

![example chart](https://github.com/DecentLabs/officeAir/blob/master/example/5_balcony_temp_hum_chart.png)

And this is how the raw data looks like:

![example raw data](https://github.com/DecentLabs/officeAir/blob/master/example/5_balcony_temp_hum_chart_2.png)
       

Let's see how to configure RPi and google sheet scripts and web app deploy to enable this "data flow gateway".

1. go to the "script" menu to open the google sheet corresponding and pre-programmed small script:
![script menu](https://github.com/DecentLabs/officeAir/blob/master/example/5_balcony_temp_hum_chart_2b.png)

2. select publish->deploy as web app from the menu there:
![web app deploy](https://github.com/DecentLabs/officeAir/blob/master/example/5_balcony_temp_hum_chart_2c.png)

3. copy paste that link (the most important thinkg) which needs to be paste in the future steps into the Rapsberry Pi code. This key makes a "link" between the RPi and the Google web app server, thus the data will be uploded into the right place: your google sheet.
![web app key, copy that](https://github.com/DecentLabs/officeAir/blob/master/example/5balcony_temp_hum_chart_2d.png)

## enabling I2C interface at Raspberry Pi
1. `sudo raspi-config`

![raspi-config](https://github.com/DecentLabs/officeAir/blob/master/example/1_raspi-config_intef_options.png)

![Interface Options](https://github.com/DecentLabs/officeAir/blob/master/example/2_raspi-config_intef_options_i2c.png)

![enable I2C](https://github.com/DecentLabs/officeAir/blob/master/example/2_raspi-config_intef_options_i2c.png)

2. `sudo apt install i2c-tools`

## veryfication of I2C sensor:
3. `sudo i2cdump -y 1 64`
#should look like somthing like this:
![I2C map](https://github.com/DecentLabs/officeAir/blob/master/example/4_i2cdump_map.png)


## creating log script to execute I2C sensor call and google web app data upload:
    sudo nano log
    
# copy paste this into log, take care to modify the _web app key_ to your deployed one
    #/bins/sh
    DATA=`sht21.py`
    curl -L "https://script.google.com/macros/s/>>copy your Google script deployed web app key here<</exec?$DATA"

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
    


    
    
