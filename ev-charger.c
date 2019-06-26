/*****************************************************************************
 *                                                                           *
 * Copyright (C) 2016, Greg Stevens, <greg@e-ctrl.com>                       *
 *                                                                           *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell  *
 * copies of this Software, and permit persons to whom this Software is      *
 * furnished to do so.                                                       *
 *                                                                           *
 * This Software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY *
 * KIND, either express or implied.                                          *
 *                                                                           *
 *****************************************************************************

Description:

	This app will continuously run to determine if and when the electric vehicle's (EV) charger switch should be
	turned on or off based on the lowest cost of electricity at the time, and turn on or off the EV charger switch accordingly.
	This is accomplished by acquiring the house electric meter's reading, adding the EV's charge current and then apply
	the following comparison:


	If the solar array is producing more than the house is using plus how much the electric vehicle charger uses,
	then turn the charger on, else, turn it off and wait until the time period of when the electricity
	provider is at its least expensive rate tier.
	
	The app will also send email and/or text message when and why the EV charger switch changes state.
	
	Also note that this app could be used for pool pumps, pool heaters, solar water heater pumps, etc. !
	Basically, anything that draws a lot of current and that you want to ensure it only turns on when the
	electricity is either free (solar is generating more than you are using) or at the least expensive tier/rate.

APIs:

* Use this to read the house electric meter:
https://rainforestautomation.com/wp-content/uploads/2015/07/EAGLE_REST_API-1.1.pdf

	The HTTP POST command looks like this:

Host: https://rainforestcloud.com/cgi-bin/post_manager
Content-Type: text/xml
Cloud-Id: *****
User: *********
Password: *********

	The POST Body looks like this:

<Command>
<Name>get_instantaneous_demand</Name>
<MacId>0x****************</MacId>
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
Use this to list the hubs on your LAN: http://connect.insteon.com/getinfo.asp Can use either a HUB1 or a HUB2.
My HUB2's IP address:    http://192.168.1.35:25105
To turn on top plug:     http://username:password@192.168.1.14:25106/3?0262418C4B0F3201=I=3
To turn off top plug:    http://username:password@192.168.1.14:25106/3?0262418C4B0F3301=I=3
To turn on bottom plug:  http://username:password@192.168.1.14:25106/3?0262418C4B0F3202=I=3  My EV charger is plugged into the bottom plug.
To turn off bottom plug: http://username:password@192.168.1.14:25106/3?0262418C4B0F3302=I=3

To turn the Rainforest gateway switch on/off:
To turn on:  http://username:password@192.168.1.14:25106/3?02623765240F12FF=I=3
To turn off: http://username:password@192.168.1.14:25106/3?02623765240F1400=I=3

Use this to talk to the APIs REST interfaces: https://curl.haxx.se/libcurl/c

Use this command line to compile:
gcc -Wall -ggdb3 ev-charger.c -oev-charger.exe -Lc:/cygwin/bin -lcygcurl-4 -Ic:/ev-charger/curl/include

*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>

#define DEBUG 0 /* 1 = Print debug info, 0 = Do not print debug info */

#define SLEEP_SECONDS 120 /* Time to wait in seconds before again checking to see if need to switch the EV charger switch on or off */

#define VALUE_CHARGE_START_HOUR 23 /* Hour of when the least expensive PG&E tier starts */
#define VALUE_CHARGE_END_HOUR 7    /* Hour of when the least expensive PG&E tier ends */

#define EV_CHARGING_CURRENT 1.4 /* Number of kilowatts the EV draws when charging; adjust this according to car model */

#define SWITCHING_THRESHOLD 0 /* If current house + EV_CHARGING_CURRENT draw is less than this then turn the EV charger switch on */

#define EMAIL "foobar@gmail.com"
#define PWD "********"
#define TEXT "1234567890@vtext.com"

#define HUB2 2

enum { /* Modes of EV charger */
   OFF,
   ON,
   ON_ERROR,
   OFF_ERROR,
   ON_VC,
   ON_VC_ERROR,
   OFF_CURRENT,
   OFF_VALUE,
   OFF_STARTUP,
   REBOOT_GATEWAY_TIMEOUT,
   REBOOT_GATEWAY_UNAVAILABLE
};

int get_meter_reading();

int switch_charger(int mode, int top, int hub);

void turn_gateway_on(int hub);
void turn_gateway_off(int hub);

void sendmail(int event);

char *parse_buffer = NULL;

double actual_demand = 0;

time_t mytime;
struct tm * timeinfo;

const char *meter_post_body = "\r\n<Command>\r\n<Name>get_instantaneous_demand</Name>\r\n<MacId>0xd8d5b90000005a54</MacId>\r\n</Command>\r\n";

const char *switch_on_url =    "http://username:password@192.168.1.14:25106/3?0262418C4B0F3202=I=3";
const char *switch_on_url2 =   "http://username:password@192.168.1.35:25105/3?0262418C4B0F3202=I=3";
const char *switch_off_url =   "http://username:password@192.168.1.14:25106/3?0262418C4B0F3302=I=3";
const char *switch_off_url2 =  "http://username:password@192.168.1.35:25105/3?0262418C4B0F3302=I=3";
const char *gateway_off_url =  "http://username:password@192.168.1.14:25106/3?02623765240F1400=I=3";
const char *gateway_off_url2 = "http://username:password@192.168.1.35:25105/3?02623765240F1400=I=3";
const char *gateway_on_url =   "http://username:password@192.168.1.14:25106/3?02623765240F12FF=I=3";
const char *gateway_on_url2 =  "http://username:password@192.168.1.35:25105/3?02623765240F12FF=I=3";
/* The top outlet */
const char *switch_on_top_url =  "http://username:password@192.168.1.14:25106/3?0262418C4B0F3201=I=3";
const char *switch_on_top_url2 = "http://username:password@192.168.1.35:25105/3?0262418C4B0F3201=I=3";

char reading[30]; // Buffer used to parse meter reading string into for emails

static const char *payload_text_on[] = {
  "To: " EMAIL "\r\n",
  "From: " EMAIL "(Greg Stevens)\r\n",
  "Subject: EV Charger Switch Turned On\r\n",
  "\r\n", /* Empty line to divide headers from body, see RFC5322 */
  "Turned the EV charger switch on as the solar panels are generating more than the house usage plus the EV charger usage.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_off_current[] = {
  "To: " EMAIL "\r\n",
  "From: " EMAIL "(Greg Stevens)\r\n",
  "Subject: EV Charger Switch Turned Off\r\n",
  "\r\n",
  "Turned the EV charger switch off as the combined current usage plus the EV charger usage is more than 0 kW.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_off_value[] = {
  "To: " EMAIL "\r\n",
  "From: " EMAIL "(Greg Stevens)\r\n",
  "Subject: EV Charger Switch Turned Off\r\n",
  "\r\n",
  "Turned the EV charger switch off as it is not in PG&E's lowest cost tier's time period.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_off_startup[] = {
  "To: " EMAIL "\r\n",
  "From: " EMAIL "(Greg Stevens)\r\n",
  "Subject: EV Charger Starting\r\n",
  "\r\n",
  "Turned the gateway switch on and the EV charger switch off at startup. Waiting 60 secs. before the first meter reading to allow the gateway to boot up.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_on_error[] = {
  "To: " EMAIL "\r\n",
  "From: " EMAIL "(Greg Stevens)\r\n",
  "Subject: EV Charger Error Turning On\r\n",
  "\r\n",
  "Could not turn the EV charger switch on.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_off_error[] = {
  "To: " EMAIL "\r\n",
  "From: " EMAIL "(Greg Stevens)\r\n",
  "Subject: EV Charger Error Turning Off\r\n",
  "\r\n",
  "Could not turn the EV charger switch off.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_on_vc[] = {
  "To: " EMAIL "\r\n",
  "From: " EMAIL "(Greg Stevens)\r\n",
  "Subject: EV Charger Turned On\r\n",
  "\r\n",
  "Turned the EV charger switch on as it is now in PG&E's lowest cost tier's time period.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_on_vc_error[] = {
  "To: " EMAIL "\r\n",
  "From: " EMAIL "(Greg Stevens)\r\n",
  "Subject: EV Charger Error Turning On\r\n",
  "\r\n",
  "Could not turn the EV charger switch on during PG&E's lowest cost tier's time period.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_reboot_timeout[] = { 
  "To: " EMAIL "\r\n",
  "From: " EMAIL "(Greg Stevens)\r\n",
  "Subject: EV Charger Rebooted Gateway - Request Timeout\r\n",
  "\r\n",
  "Rebooted the gateway since a Request Timeout response was received.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_reboot_unavailable[] = {
  "To: " EMAIL "\r\n",
  "From: " EMAIL "(Greg Stevens)\r\n",
  "Subject: EV Charger Rebooted Gateway - Service Unavailable\r\n\r\n",
  "\r\n",
  "Rebooted the gateway since a Service Unavailable response was received.\r\n",
  "\r\n",
  NULL
};

static size_t WriteMemoryCallbackMeter(void *contents, size_t size, size_t nmemb, void *userp) {

  size_t realsize = size * nmemb;

  if (DEBUG) {
	printf("\nPost Response from Meter:\n%s", (char *)contents);
  }

  /* Save the address of the POST Response so we can parse out the info later */
  parse_buffer = contents;

  return realsize;
}

void turn_gateway_on(int hub) {
   CURL *curl = NULL;
   CURLcode res;

   /* Turn the gateway's power switch on */
   
   /* Initialize the network interface (winsock) */
   curl_global_init(CURL_GLOBAL_ALL);

   /* Get a curl handle */

   curl = curl_easy_init();

   if (!curl) {
	  printf("\n%sturn_on_gateway: failed to get a curl handle to hard reboot the gateway to turn it on.\n", ctime(&mytime));
	  curl_global_cleanup();
	  return;
   }
   
   curl_easy_setopt(curl, CURLOPT_URL, gateway_on_url2);

   if (DEBUG) {
	  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
   }  else {
	  curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
   }

   /* Perform the request, res will get the return code */
   res = curl_easy_perform(curl);

   /* Check for errors */

   if (res != CURLE_OK) {
	  curl_easy_cleanup(curl);
	  curl_global_cleanup();
	  printf("\n%sturn_on_gateway: curl_easy_perform() failed to hard reboot the gateway to turn it on: %s.\n", ctime(&mytime), curl_easy_strerror(res));
	  return;
   }

   curl_easy_cleanup(curl);
   curl_global_cleanup();
}

void turn_gateway_off(int hub) {
   CURL *curl = NULL;
   CURLcode res;

   /* Turn the gateway's power switch off */
	  
   /* Initialize the network interface (winsock) */
   curl_global_init(CURL_GLOBAL_ALL);

   /* Get a curl handle */
   curl = curl_easy_init();
 
   if (!curl) {
	  printf("\n%sturn_off_gateway: failed to get a curl handle to hard reboot the gateway to turn it off.\n", ctime(&mytime));
	  curl_global_cleanup();
	  return;
   }

   curl_easy_setopt(curl, CURLOPT_URL, gateway_off_url2);

   if (DEBUG) {
	  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
   }  else {
	  curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
   }

   /* Perform the request, res will get the return code */
   res = curl_easy_perform(curl);

   /* Check for errors */

   if (res != CURLE_OK) {
	  curl_easy_cleanup(curl);
	  curl_global_cleanup();
	  printf("\n%sturn_off_gateway: curl_easy_perform() failed to hard reboot the gateway to turn it off: %s.\n", ctime(&mytime), curl_easy_strerror(res));
	  return;
   }
   
   curl_easy_cleanup(curl);
   curl_global_cleanup();
}

void reboot_gateway() {
	turn_gateway_off(HUB2);
	printf("\n%sTurned the gateway switch off.\n", ctime(&mytime));
	printf("\n%sWaiting 5 seconds before turning gateway switch back on...\n", ctime(&mytime));
	sleep(5);
	turn_gateway_on(HUB2);
	printf("\n%sTurned the gateway switch back on.\n", ctime(&mytime));
	sleep(60); // Allow time for the gateway to reboot
	printf("\n%sWaiting 60 seconds before reading the meter to allow the gateway to reboot...\n", ctime(&mytime));
}

int get_meter_reading() {

	/* Call the Rainforest APIs to get the current meter reading */

	CURL *curl = NULL;
	CURLcode res;

	char *start_demand;
	char *end_demand;
	char *start_multiplier;
	char *end_multiplier;
	char *start_divisor;
	char *end_divisor;

	char demand_string[11];
	char multiplier_string[11];
	char divisor_string[11];

	double demand = 0;
	double demand_tmp;
	double multiplier;
	double divisor;
	
	actual_demand = 0;   // Set the demand to 0kW in case we can't read the meter
	parse_buffer = NULL; // Clear the parse buffer in case next time we cannot read meter as we don't want the previous values in it

	/* Initialize the network interface (winsock) */
	curl_global_init(CURL_GLOBAL_ALL);

	/* Get a curl handle */
	curl = curl_easy_init();

	if (!curl) {
		printf("\n%sget_meter_reading: failed to get a curl handle.\n", ctime(&mytime));
		curl_global_cleanup();
		return 0;  // Didn't get a clean meter reading so return error
	}

   struct curl_slist *chunk = NULL;

   /* Add all the custom headers */
   chunk = curl_slist_append(chunk, "Content-Type: text/xml");
   chunk = curl_slist_append(chunk, "Cloud-Id: *****");
   chunk = curl_slist_append(chunk, "User: foobar@gmail.com");
   chunk = curl_slist_append(chunk, "Password: ******");

   /* Set our custom set of headers */
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

   /* Set the URL that is about to receive our POST. */
   curl_easy_setopt(curl, CURLOPT_URL, "https://rainforestcloud.com:9445/cgi-bin/post_manager");

   /* Specify we want to POST data */
   curl_easy_setopt(curl, CURLOPT_POST, 1L);

   /* Send the POST Body to this function  */
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallbackMeter);

   /* Pass 'chunk' struct to the callback function */
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

   /* Set the POST Body data */
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, meter_post_body);

   if (DEBUG) {
	  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
   } else {
	  curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
   }

   /* Perform the request, res will get the return code */
   res = curl_easy_perform(curl);

   /* Check for errors */
   if (res != CURLE_OK) {
	  curl_slist_free_all(chunk);
	  curl_easy_cleanup(curl);
	  curl_global_cleanup();
	  printf("\n%sget_meter_reading: curl_easy_perform() failed: %s.\n", ctime(&mytime), curl_easy_strerror(res));
	  return 0;  // Didn't get a clean meter reading so return error
   }

   /* Free the custom headers */
   curl_slist_free_all(chunk);
   curl_easy_cleanup(curl);
   curl_global_cleanup();

   if (parse_buffer) {
	 /* Sometimes the parse_buffer does not have the <Demand> token in it so have to bail out, but check possible other responses as well. */
	   if (!strstr(parse_buffer, "<Demand>")) {
		  printf("\n%sNo <Demand> token in POST response:\n%s\n", ctime(&mytime), parse_buffer); 
		  
		  if (strstr(parse_buffer, "Request Timeout")) { // If timeout then Rainforest gateway needs to be rebooted
			 printf("\n%sRebooting the gateway since a Request Timeout response was received.\n", ctime(&mytime));
			 reboot_gateway();
			 sendmail(REBOOT_GATEWAY_TIMEOUT);
		  }
		  if (strstr(parse_buffer, "Service Unavailable")) { // If unavailable then Rainforest gateway needs to be rebooted
			 printf("\n%sRebooting the gateway since a Service Unavailable response was received.\n", ctime(&mytime));
			 reboot_gateway();
			 sendmail(REBOOT_GATEWAY_UNAVAILABLE);
		  }
		  return 0;  // Didn't get a clean meter reading so return error
	   }
	   
	   /*
		  Parse the XML that is returned in the POST Response to search for the <Demand> token value.
		  Must also get the <Multiplier> and <Divisor> values and do the math to get the actual
		  kilowatts being consumed.

		  The actual Demand value is calculated by using the multiplier and divisor: 5944 x 1 / 1000 = 5.944
		  If the multiplier or divisor is zero then use a value of one instead.
	   */

	   start_demand = strstr(parse_buffer, "<Demand>") + strlen("<Demand>");
	   end_demand = strchr(start_demand, '<');
	   strncpy(demand_string, start_demand, end_demand - start_demand);
	   demand_string[end_demand - start_demand] = '\0';
	   demand = atof(demand_string);

	   if (demand_string[2]=='f') {        // If meter is negative, then the demand number is very high since all leading zeros are f's,
		  demand_tmp = atof("0xffffffff"); //  so if the first letter after the "0x" is an 'f' then correct it using these three lines of code
		  demand = demand_tmp - demand;
		  demand = demand * -1;
	   }

	   /* Parse out the multiplier */
	   start_multiplier = strstr(parse_buffer, "<Multiplier>") + strlen("<Multiplier>");
	   end_multiplier = strchr(start_multiplier, '<');
	   strncpy(multiplier_string, start_multiplier, end_multiplier - start_multiplier);
	   multiplier_string[end_multiplier - start_multiplier] = '\0';
	   multiplier = atof(multiplier_string);
	   if (multiplier == 0) {
		  multiplier = 1;
	   }

	   /* Parse out the divisor */
	   start_divisor = strstr(parse_buffer, "<Divisor>") + strlen("<Divisor>");
	   end_divisor = strchr(start_divisor, '<');
	   strncpy(divisor_string, start_divisor, end_divisor - start_divisor);
	   divisor_string[end_divisor - start_divisor] = '\0';
	   divisor = atof(divisor_string);
	   if (divisor == 0) {
		  divisor = 1;
	   }

	   /* Now do the calculation to get the actual usage */
	   actual_demand = (demand * multiplier) / divisor;

	   if (DEBUG) {
		 printf("\ndemand_string: %s\nmultiplier_string: %s\ndivisor_string: %s\n", demand_string, multiplier_string, divisor_string);
		 printf("\ndemand: %.0f\nmultiplier: %.0f\ndivisor: %.0f\nactual_demand: %.3f\n\n", demand, multiplier, divisor, actual_demand);
	   }
	   return 1;
   }
   printf("\n%sget_meter_reading: parse_buffer is NULL.\n", ctime(&mytime));
   return 0;
}

int switch_charger(int mode, int top, int hub) {

    /*  This funtion will turn the switch on or off. It returns the following:
        1 = Successfully turned the switch on
        0 = Successfully turned the switch off
       -1 = Failed to turn the switch on or off
    */

	CURL *curl = NULL;
  	CURLcode res;

	/* Initialize the network interface (winsock) */
	curl_global_init(CURL_GLOBAL_ALL);

	/* Get a curl handle */
	curl = curl_easy_init();

	if (!curl) {
	   printf("\n%sswitch_charger: failed to get a curl handle to turn the EV charger switch on or off.\n", ctime(&mytime));
	   curl_global_cleanup();
	   return -1;
    }

   /* Set the URL */
   if (mode == ON) {
	  if (top == ON) {

		 curl_easy_setopt(curl, CURLOPT_URL, switch_on_top_url2);

	  } else {

	    curl_easy_setopt(curl, CURLOPT_URL, switch_on_url2);

	  }
   } else {

	   curl_easy_setopt(curl, CURLOPT_URL, switch_off_url2);

   }

   if (DEBUG) {
	  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
   }  else {
	  curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
   }

   /* Perform the request, res will get the return code */
   res = curl_easy_perform(curl);

   /* Check for errors */
   if (res != CURLE_OK) {
	  curl_easy_cleanup(curl);
	  curl_global_cleanup();
	  printf("\n%sswitch_charger: curl_easy_perform() failed turning the EV charger switch on or off: %s.\n", ctime(&mytime), curl_easy_strerror(res));
	  return -1;
   }

   curl_easy_cleanup(curl);
   curl_global_cleanup();

   return mode;
}

struct upload_status {
  int lines_read;
};

static size_t payload_source_on(void *ptr, size_t size, size_t nmemb, void *userp)
{
  struct upload_status *upload_ctx = (struct upload_status *)userp;
  const char *data;

  if((size == 0) || (nmemb == 0) || ((size*nmemb) < 1)) {
    return 0;
  }

  data = payload_text_on[upload_ctx->lines_read];

  if(data) {
    size_t len = strlen(data);
    memcpy(ptr, data, len);
	if (strstr(data, "Turned the EV charger switch on as the solar")) {
		sprintf(reading, " Meter reading: %.3f kW.\r\n", actual_demand);
		memcpy(ptr+(strlen(data)-2), reading, strlen(reading));
		upload_ctx->lines_read++;
        return (strlen((char *)ptr));
    }
    upload_ctx->lines_read++;

    return len;
  }

  return 0;
}

 static size_t payload_source_on_error(void *ptr, size_t size, size_t nmemb, void *userp)
{
  struct upload_status *upload_ctx = (struct upload_status *)userp;
  const char *data;

  if((size == 0) || (nmemb == 0) || ((size*nmemb) < 1)) {
    return 0;
  }

  data = payload_text_on_error[upload_ctx->lines_read];

  if(data) {
    size_t len = strlen(data);
    memcpy(ptr, data, len);
	if (strstr(data, "Could not turn the EV charger switch on")) {
		sprintf(reading, " Meter reading: %.3f kW.\r\n", actual_demand);
		memcpy(ptr+(strlen(data)-2), reading, strlen(reading));
		upload_ctx->lines_read++;
        return (strlen((char *)ptr));
    }
    upload_ctx->lines_read++;

    return len;
  }

  return 0;
}

static size_t payload_source_off_current(void *ptr, size_t size, size_t nmemb, void *userp)
{
  struct upload_status *upload_ctx = (struct upload_status *)userp;
  const char *data;

  if((size == 0) || (nmemb == 0) || ((size*nmemb) < 1)) {
    return 0;
  }

  data = payload_text_off_current[upload_ctx->lines_read];

  if(data) {
    size_t len = strlen(data);
    memcpy(ptr, data, len);
	if (strstr(data, "Turned the EV charger switch off as the combined")) {
		sprintf(reading, " Meter reading: %.3f kW.\r\n", actual_demand);
		memcpy(ptr+(strlen(data)-2), reading, strlen(reading));
		upload_ctx->lines_read++;
        return (strlen((char *)ptr));
    }
    upload_ctx->lines_read++;

    return len;
  }

  return 0;
}

static size_t payload_source_off_value(void *ptr, size_t size, size_t nmemb, void *userp)
{
  struct upload_status *upload_ctx = (struct upload_status *)userp;
  const char *data;

  if((size == 0) || (nmemb == 0) || ((size*nmemb) < 1)) {
    return 0;
  }

  data = payload_text_off_value[upload_ctx->lines_read];

  if(data) {
    size_t len = strlen(data);
    memcpy(ptr, data, len);
	if (strstr(data, "Turned the EV charger switch off as it is not")) {
		sprintf(reading, " Meter reading: %.3f kW.\r\n", actual_demand);
		memcpy(ptr+(strlen(data)-2), reading, strlen(reading));
		upload_ctx->lines_read++;
        return (strlen((char *)ptr));
    }
    upload_ctx->lines_read++;

    return len;
  }

  return 0;
}

static size_t payload_source_off_startup(void *ptr, size_t size, size_t nmemb, void *userp)
{
  struct upload_status *upload_ctx = (struct upload_status *)userp;
  const char *data;

  if((size == 0) || (nmemb == 0) || ((size*nmemb) < 1)) {
    return 0;
  }

  data = payload_text_off_startup[upload_ctx->lines_read];

  if(data) {
    size_t len = strlen(data);
    memcpy(ptr, data, len);

    upload_ctx->lines_read++;

    return len;
  }

  return 0;
}

static size_t payload_source_off_error(void *ptr, size_t size, size_t nmemb, void *userp)
{
  struct upload_status *upload_ctx = (struct upload_status *)userp;
  const char *data;

  if((size == 0) || (nmemb == 0) || ((size*nmemb) < 1)) {
    return 0;
  }

  data = payload_text_off_error[upload_ctx->lines_read];

  if(data) {
    size_t len = strlen(data);
    memcpy(ptr, data, len);
	if (strstr(data, "Could not turn the EV charger switch off")) {
		sprintf(reading, " Meter reading: %.3f kW.\r\n", actual_demand);
		memcpy(ptr+(strlen(data)-2), reading, strlen(reading));
		upload_ctx->lines_read++;
        return (strlen((char *)ptr));
    }
    upload_ctx->lines_read++;

    return len;
  }

  return 0;
}

static size_t payload_source_on_vc(void *ptr, size_t size, size_t nmemb, void *userp)
{
  struct upload_status *upload_ctx = (struct upload_status *)userp;
  const char *data;

  if((size == 0) || (nmemb == 0) || ((size*nmemb) < 1)) {
    return 0;
  }

  data = payload_text_on_vc[upload_ctx->lines_read];

  if(data) {
    size_t len = strlen(data);
    memcpy(ptr, data, len);
	if (strstr(data, "Turned the EV charger switch on as it is now")) {
		sprintf(reading, " Meter reading: %.3f kW.\r\n", actual_demand);
		memcpy(ptr+(strlen(data)-2), reading, strlen(reading));
		upload_ctx->lines_read++;
        return (strlen((char *)ptr));
    }
    upload_ctx->lines_read++;

    return len;
  }

  return 0;
}

static size_t payload_source_on_vc_error(void *ptr, size_t size, size_t nmemb, void *userp)
{
  struct upload_status *upload_ctx = (struct upload_status *)userp;
  const char *data;

  if((size == 0) || (nmemb == 0) || ((size*nmemb) < 1)) {
    return 0;
  }

  data = payload_text_on_vc_error[upload_ctx->lines_read];

  if(data) {
    size_t len = strlen(data);
    memcpy(ptr, data, len);
	if (strstr(data, "Could not turn the EV charger switch on during")) {
		sprintf(reading, " Meter reading: %.3f kW.\r\n", actual_demand);
		memcpy(ptr+(strlen(data)-2), reading, strlen(reading));
		upload_ctx->lines_read++;
        return (strlen((char *)ptr));
    }
    upload_ctx->lines_read++;

    return len;
  }

  return 0;
}

static size_t payload_source_reboot_timeout(void *ptr, size_t size, size_t nmemb, void *userp)
{
  struct upload_status *upload_ctx = (struct upload_status *)userp;
  const char *data;

  if((size == 0) || (nmemb == 0) || ((size*nmemb) < 1)) {
    return 0;
  }

  data = payload_text_reboot_timeout[upload_ctx->lines_read];

  if(data) {
    size_t len = strlen(data);
    memcpy(ptr, data, len);
	/* No need to print meter reading as there is none hence having to reboot
	if (strstr(data, "Rebooted the")) {
		sprintf(reading, " Meter reading: %.3f kW.\r\n", actual_demand);
		memcpy(ptr+(strlen(data)-2), reading, strlen(reading));
		upload_ctx->lines_read++;
        return (strlen((char *)ptr));
    }
	*/
    upload_ctx->lines_read++;

    return len;
  }

  return 0;
}

static size_t payload_source_reboot_unavailable(void *ptr, size_t size, size_t nmemb, void *userp)
{
  struct upload_status *upload_ctx = (struct upload_status *)userp;
  const char *data;

  if((size == 0) || (nmemb == 0) || ((size*nmemb) < 1)) {
    return 0;
  }

  data = payload_text_reboot_unavailable[upload_ctx->lines_read];

  if(data) {
    size_t len = strlen(data);
    memcpy(ptr, data, len);
	/* No need to print meter reading as there is none hence having to reboot
	if (strstr(data, "Rebooted the")) {
		sprintf(reading, " Meter reading: %.3f kW.\r\n", actual_demand);
		memcpy(ptr+(strlen(data)-2), reading, strlen(reading));
		upload_ctx->lines_read++;
        return (strlen((char *)ptr));
    }
	*/
    upload_ctx->lines_read++;

    return len;
  }

  return 0;
}

void sendmail(int event) {

  CURL *curl = NULL;
  CURLcode res = CURLE_OK;
  struct curl_slist *recipients = NULL;
  struct upload_status upload_ctx;

  upload_ctx.lines_read = 0;

  curl_global_init(CURL_GLOBAL_ALL);
  curl = curl_easy_init();

  	if (!curl) {
	   printf("\n%ssendmail: failed to get a curl handle to send email.\n", ctime(&mytime));
	   curl_global_cleanup();
	   return;
    }

    /* Set the URL for the mailserver */
	curl_easy_setopt(curl, CURLOPT_URL, "smtps://smtp.gmail.com");

	/* Set username and password */
    curl_easy_setopt(curl, CURLOPT_USERNAME, EMAIL);
	curl_easy_setopt(curl, CURLOPT_PASSWORD, PWD);

    /* Set FROM address */
	curl_easy_setopt(curl, CURLOPT_MAIL_FROM, EMAIL);

	/* Set to use SSL */
	curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);

    /* Add recipient(s) */
//    recipients = curl_slist_append(recipients, EMAIL);	// email
	recipients = curl_slist_append(recipients, TEXT);	// text message
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

    /* We're using a callback function to specify the payload (the headers and body of the message */
    switch (event) {
		case ON:
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source_on);
			break;
		case OFF_CURRENT:
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source_off_current);
			break;
		case OFF_VALUE:
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source_off_value);
			break;
		case OFF_STARTUP:
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source_off_startup);
			break;
		case ON_ERROR:
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source_on_error);
			break;
		case OFF_ERROR:
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source_off_error);
			break;
		case ON_VC:
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source_on_vc);
			break;
		case ON_VC_ERROR:
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source_on_vc_error);
			break;
		case REBOOT_GATEWAY_TIMEOUT:
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source_reboot_timeout);
			break;
		case REBOOT_GATEWAY_UNAVAILABLE:
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source_reboot_unavailable);
			break;
	}

    curl_easy_setopt(curl, CURLOPT_READDATA, &upload_ctx);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

    if (DEBUG) {
	  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    } else {
	  curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
    }

    /* Send the message */
    res = curl_easy_perform(curl);

    /* Check for errors */
    if (res != CURLE_OK)
       printf("\n%scurl_easy_perform() failed: %s.\n", ctime(&mytime), curl_easy_strerror(res));

    /* Free the list of recipient(s) */
    curl_slist_free_all(recipients);

    /* Cleanup */
	curl_easy_cleanup(curl);
	curl_global_cleanup();
}

int main() {

    int current_mode = OFF; // Start in the off state

	/* Get current date and time */
	mytime = time(NULL);

	/* Parse out time components */
	timeinfo = localtime(&mytime);
	
	/* First, turn the gateway switch on to ensure the gateway is running (it should always be on as no other automation controllers manage it). */
	
	turn_gateway_on(HUB2);
	printf("\n%sTurned the gateway switch on at startup.\n", ctime(&mytime));
	
	/* Next, we need to know if the EV charger switch is on or off so we will turn it off, wait to
       allow the meter to recognize the change, then start our endless while loop. */

	if (switch_charger(OFF, OFF, HUB2) != OFF) {
		printf("\n%sCould not turn the EV charger switch off at startup.\n", ctime(&mytime));
		exit(-1);
	} 
	
	printf("\n%sTurned the EV charger switch off at startup.\n", ctime(&mytime));
	sendmail(OFF_STARTUP);
	
	printf("\n%sWaiting 60 seconds before reading the meter to allow the gateway to boot up...\n", ctime(&mytime));
	sleep(60); // Wait for the gateway to boot
	
    while (1) {

		/* Get current date and time */
		mytime = time(NULL);

		/* Parse out time components */
		timeinfo = localtime(&mytime);

		/* If the current time of day is in the Value Charge time frame, then turn the EV charger switch on */
		if ((timeinfo->tm_hour < VALUE_CHARGE_END_HOUR) || (timeinfo->tm_hour >= VALUE_CHARGE_START_HOUR)) {
			if (switch_charger(ON, OFF, HUB2) != ON) { // Turn EV charger switch on
				printf("\n%sCould not turn the EV charger switch on during PG&E's lowest cost tier's time period.\n", ctime(&mytime));
				sendmail(ON_VC_ERROR);
				continue;
		    } else {
			   if (current_mode != ON_VC) {
				   printf("\n%sTurned the EV charger switch on as it is now in PG&E's lowest cost tier's time period.\n", ctime(&mytime));
				   sendmail(ON_VC);
				   current_mode = ON_VC;
			    }
			   
			   if (get_meter_reading()) {
				   printf("\n%sMeter reading: %.3f kW. EV charger switch is on (Value Charge time period).\n", ctime(&mytime), actual_demand);
			   }
            }
		} else {
		
		    if (get_meter_reading()) {  /* Otherwise, get the meter reading and determine if need to turn on or off the EV charger switch */
			    /* If the EV charger switch is currently ON, then don't include the EV_CHARGING_CURRENT in the below if statement equation, but do if OFF */
			    if ((current_mode == ON && (actual_demand <= SWITCHING_THRESHOLD)) ||
				   (current_mode == OFF && ((actual_demand + EV_CHARGING_CURRENT) <= SWITCHING_THRESHOLD))) {


				  if (switch_charger (ON, OFF, HUB2) == ON) {
					 
					if (current_mode == OFF) {
					   printf("\n%sTurned the EV charger switch on as the solar panels are generating more than the house usage plus the EV charger usage.\n", ctime(&mytime));
					   sendmail(ON);
					}
					current_mode = ON; // Set state to on
				 
					printf("\n%sMeter reading: %.3f kW.\nEV charger switch is on.\n", ctime(&mytime), actual_demand);
			  
				  }  else {
				    printf("\n%sCould not turn the EV charger switch on.\n", ctime(&mytime));
				    sendmail(ON_ERROR);
				    continue;
				  }	
				  
			    } else {
					
				  if (switch_charger(OFF, OFF, HUB2) == OFF) {
					  
					if (current_mode == ON_VC) {
					  printf("\n%sTurned the EV charger switch off as it is not in PG&E's lowest cost tier's time period.\n", ctime(&mytime));
					  sendmail(OFF_VALUE);
					} else {
					   if (current_mode == ON) {
						  printf("\n%sTurned the EV charger switch off as the combined current usage plus the EV charger usage is more than %d kW.\n", ctime(&mytime), SWITCHING_THRESHOLD);
						  sendmail(OFF_CURRENT);
					   }
					}
					
					current_mode = OFF; // Set state to off
								 	 
				    printf("\n%sMeter reading: %.3f kW.\nEV charger switch is off.\n", ctime(&mytime), actual_demand);
					
				 } else {
					 printf("\n%sCould not turn the EV charger switch off.\n", ctime(&mytime));
					 sendmail(OFF_ERROR);
					 continue;
				 }		 
			   }
			}
		}
				
		sleep(SLEEP_SECONDS);   // Wait until next time to check again
	}
}