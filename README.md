# ev-charger

I have a plugin electric vehicle (EV) and solar panels on my roof that typically generate more than I use so I push it back to the grid for credit. I created an app that runs 24/7 (sampling every 2 mins) to determine if and when my electric vehicle's charger should be turned on or off based on the lowest cost of electricity and turn on or off the EV charger accordingly. This is accomplished by reading the house electric meter's current draw, add how much the electric vehicle's charge current to it and then do the following comparison:
	If the solar array is producing more than the house is using plus how much the electric vehicle charger uses, then turn the 		charger on. Otherwise, turn it off and wait to turn it on when the time period of electricity cost is at the least expensive 		rate tier.

APIs:

* Use this to read the house electric meter:
https://rainforestautomation.com/wp-content/uploads/2017/02/EAGLE-200-Local-API-Manual-v1.0.pdf
If you use a different smart meter reader gateway other than Rainforest, then you will have to modify the code accordingly so you can parse the Post Response payload.

* Use this to turn on/off the Insteon wall outlet that the electric vehicle's charger is plugged into:
http://www.smarthome.com.au/smarthome-blog/insteon-hub-http-commands/
Use this to list the hubs on your LAN: http://connect.insteon.com/getinfo.asp
To turn on bottom plug:  http://username:pwd@192.168.1.4:port/3?0262418C4B0F3202=I=3
To turn off bottom plug: http://username:pwd@192.168.1.4:port/3?0262418C4B0F3302=I=3
If you use a different smart outlet other than Insteon, then you will have to modify the code accordingly so you can send the correct on and off commands.

* Use this to talk to the APIs REST interfaces: https://curl.haxx.se/libcurl/c

* Use this command line to compile:
gcc -Wall -ggdb3 ev-charger.c -oev-charger.exe -Lc:/cygwin/bin -lcygcurl-4 -Ic:ev-charger/curl/include
