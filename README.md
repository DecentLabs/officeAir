# officeAir - A temperature and Humidity Meter and Google Sheet Logger with Raspberry Pi and I2C sensors

This project lets you log temperature and humidity data via Raspberry Pi and a sensor easily and automated way into you google sheet, collecting the raw data and automatically drawing a nice chart as data coming in. (The name and use case comes from life: initially we set this up to monitor DecentLabs office climate in a hot summer period, when air conditioning had some weakness.)

Code by: [Róbert Szalóki](https://github.com/rszaloki), Hardware configuration and manual by: [Zoltan Dóczi (RFsparkling)](http://rfsparkling.com), Licensed by [DecentLabs](https://decent.org/) 

First lets see the Google sheet and its script side to prepare that before Raspberry pi configurations.

### You can take this example Google sheet (make a copy to yourself and format for your needs):
[example google sheet](https://docs.google.com/spreadsheets/d/1t9r_rUyFjBkhwC56gJk5tmbUgx4k2Acqq_upUnAoVYM/edit?usp=sharing)

Here is a chart for temperature and humidity for several days:

![example chart](https://github.com/DecentLabs/officeAir/blob/master/example/5_balcony_temp_hum_chart.png)

And this is how the raw data looks like:

![example raw data](https://github.com/DecentLabs/officeAir/blob/master/example/5_balcony_temp_hum_chart_2.png)
       

Let's see how to configure RPi and google sheet scripts and web app deploy to enable this "data flow gateway".

### 1. go to the "script" menu to open the google sheet corresponding and pre-programmed small script:
![script menu](https://github.com/DecentLabs/officeAir/blob/master/example/5_balcony_temp_hum_chart_2b.png)

[or you can download the script code file too from the repo here](https://github.com/DecentLabs/officeAir/blob/master/code.gs)

### 2. select publish->deploy as web app from the menu there:
![web app deploy](https://github.com/DecentLabs/officeAir/blob/master/example/5_balcony_temp_hum_chart_2c.png)

### 3. copy that link (the most important thinkg) which needs to be paste in the future steps into the Rapsberry Pi code (see below in step 8). This key makes a "link" between the RPi and the Google web app server, thus the data will be uploded into the right place: your google sheet.
![web app key, copy that](https://github.com/DecentLabs/officeAir/blob/master/example/5balcony_temp_hum_chart_2d.png)

After the google sheet configuration now see what configuration we need to do under the hood of Raspberry Pi:

## 4. enable I2C interface at Raspberry Pi
 
       sudo raspi-config

![raspi-config](https://github.com/DecentLabs/officeAir/blob/master/example/1_raspi-config_intef_options.png)

![Interface Options](https://github.com/DecentLabs/officeAir/blob/master/example/2_raspi-config_intef_options_i2c.png)

![enable I2C](https://github.com/DecentLabs/officeAir/blob/master/example/2_raspi-config_intef_options_i2c.png)

### 4b. connect the sensor to the RaspberryPi's corresponding pins as shown below:
![wiring](https://github.com/DecentLabs/officeAir/blob/master/example/6_sensor_wiring1.png)
![wiring](https://github.com/DecentLabs/officeAir/blob/master/example/6_sensor_wiring2.png)
![wiring](https://github.com/DecentLabs/officeAir/blob/master/example/6_sensor_wiring3.png)

### 5. update + upgrade and install i2c-tools to be able to reach the sensor and check its status
	sudo apt update
	sudo apt upgrade
	sudo apt install i2c-tools

### 6. veryfication of I2C sensor:
	sudo i2cdump -y 1 64
 
#should look like somthing like this:
![I2C map](https://github.com/DecentLabs/officeAir/blob/master/example/4_i2cdump_map.png)


### 7. creating log script to execute I2C sensor call and google web app data upload:
	sudo mkdir /usr/local/bin/log
	sudo cd /usr/local/bin/log
	sudo nano log
    
### 8. copy paste this into log, take care to modify the _web app key_ what you copied at setp 3 above
	#/bins/sh
	DATA=`sht21.py`
	curl -L "https://script.google.com/macros/s/**copy your Google script deployed web app key here**/exec?$DATA"

### 9. set rights
	sudo chmod +x log
    
### 10. copy the python script into the proper folder:
	cd /usr/local/bin/log
	sudo wget https://raw.githubusercontent.com/DecentLabs/officeAir/master/sht21.py
	sudo chmod +x sht21.py
### 11. modifying crontab to execute the script every minuite
	sudo nano /etc/crontab

### 11b. and copy paste to the last line this:

	*  *    * * *   root  /usr/local/bin/log #log air quality
    
### Finally reboot the RPi, once it comes back it should report temperature and humidity values every minute to the google sheet
	sudo reboot
    


    
    
