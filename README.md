# ev-charger

This app will continuously run to determine if and when the electric vehicle's (EV) charger switch should be turned on or off based on the lowest cost of electricity at the time. This is accomplished by acquiring the house electric meter's reading, adding the EV's charge current and then apply the following comparison:

	If the solar array is producing more than the house is using plus how much the electric vehicle charger uses,
	then turn the charger on, else, turn it off and wait until the time period of when the electricity
	provider is at its least expensive rate tier.
	
	The app will also send email and/or text message when and why the EV charger switch changes state.
	
	Also note that this app could be used for pool pumps, pool heaters, solar water heater pumps, etc.
	Basically, anything that draws a lot of current and that you want to ensure it only turns on when the
	electricity is either free (solar is generating more than you are using) or at the least expensive tier/rate.

APIs:

* Use this to read the house electric meter:
https://rainforestautomation.com/wp-content/uploads/2015/07/EAGLE_REST_API-1.1.pdf

	The HTTP POST command looks like this:

Host: https://rainforestcloud.com/cgi-bin/post_manager
Content-Type: text/xml
Cloud-Id: *****
User: *****
Password: *****

	The POST Body looks like this:

<Command>
<Name>get_instantaneous_demand</Name>
<MacId>0xd8d5b90000005a54</MacId>
</Command>

   The POST Response has the data and looks like this:

<InstantaneousDemand>
<DeviceMacId>0x00158d0000000004</DeviceMacId>
<MeterMacId>0x00178d0000000004</MeterMacId>
<TimeStamp>0x185adc1d</TimeStamp>
<Demand>0x001738</Demand>
<Multiplier>0x00000001</Multiplier>
<Divisor>0x000003e8</Divisor>
<DigitsRight>0x03</DigitsRight>
<DigitsLeft>0x00</DigitsLeft>
<SuppressLeadingZero>Y</SuppressLeadingZero>
</InstantaneousDemand>

   The actual Demand value is calculated by using the multiplier and divisor:
   5944 x 1 / 1000 = 5.944
   If the multiplier or divisor is zero then use a value of one instead.

* Use this to turn on/off the Insteon wall outlet that the electric vehicle's charger is plugged into:
http://www.smarthome.com.au/smarthome-blog/insteon-hub-http-commands/
Use this to list the hubs on your LAN: http://connect.insteon.com/getinfo.asp

To turn on top plug:     http://username:pwd@192.168.1.14:25106/3?0262418C4B0F3201=I=3
To turn off top plug:    http://username:pwd@192.168.1.14:25106/3?0262418C4B0F3301=I=3
To turn on bottom plug:  http://username:pwd@192.168.1.14:25106/3?0262418C4B0F3202=I=3
To turn off bottom plug: http://username:pwd@192.168.1.14:25106/3?0262418C4B0F3302=I=3

To turn the Rainforest gateway switch on/off:
To turn on:  http://username:pwd@192.168.1.14:25106/3?02623765240F12FF=I=3
To turn off: http://username:pwd@192.168.1.14:25106/3?02623765240F1400=I=3

Use this to talk to the APIs REST interfaces: https://curl.haxx.se/libcurl/c

Use this command line to compile:
gcc -Wall -ggdb3 ev-charger.c -oev-charger.exe -Lc:/cygwin/bin -lcygcurl-4 -Ic:ev-charger/curl/include
