/*****************************************************************************
 *                                                                           *
 * Copyright (C) 2016-2021, Greg Stevens, <greg@e-ctrl.com>                  *
 *                                                                           *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell  *
 * copies of this Software, and permit persons to whom this Software is      *
 * furnished to do so.                                                       *
 *                                                                           *
 * This Software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY *
 * KIND, either expressed or implied.                                        *
 *                                                                           *
 *****************************************************************************

Description:

	This app will continuously run (samples every 2 minutes - customizable) to determine if and
	when the electric vehicle's (EV) charger switch should be turned on or off based on the
	lowest cost of electricity at the time, and turn on or off the EV charger switch accordingly.
	This is accomplished by acquiring the house electric meter's reading, adding the EV's charge
	current and then apply the following comparison:

	If the solar array is producing more than the house is using plus how much the electric vehicle
	charger uses, then turn the charger on, otherwise, turn it off and wait until the time period of
	when the grid is at its least expensive rate time period.
	
	The app will also send email and/or text message when and why the EV charger switch changes state.
	
	Also note that this app can be used for pool pumps, pool heaters, solar water heater pumps, etc.
	Basically, anything that draws a lot of current and you want to ensure it only turns on when the
	electricity is either free (solar is generating more than it is using) or at the least expensive
	tier/rate.

APIs:

* Use this to read the house electric meter via the Rainforest gateway:
https://rainforestautomation.com/wp-content/uploads/2017/02/EAGLE-200-Local-API-Manual-v1.0.pdf

* Use this to turn on/off the Insteon wall outlet that the electric vehicle's charger is plugged into:
http://www.smarthome.com.au/smarthome-blog/insteon-hub-http-commands/
Use this to list the hubs on your LAN: http://connect.insteon.com/getinfo.asp Can use either a HUB1 or a HUB2.
My Insteon HUB2's IP address: http://192.168.1.3:port
To turn on bottom plug:  http://user:password@192.168.1.3:port/3?0262418C4B0F3202=I=3  My EV charger is plugged into the bottom plug.
To turn off bottom plug: http://user:password@192.168.1.3:port/3?0262418C4B0F3302=I=3

Use Curl to talk to the above API's RESTful interfaces: https://curl.haxx.se/libcurl/c

Use GNU toolchain; command line to compile:
gcc -Wall -ggdb3 ev-charger.c -oev-charger.exe -Lc:/cygwin/bin -lcygcurl-4 -Ic:/Users/Admin/Desktop/ev-charger/curl/include

Use gdb to debug.
Use strip to clean for production.

*/

/* Include the needed libraries */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>

/* Define constants used */
#define DEBUG 0 /* 1 = Print debug info, 0 = Do not print debug info */

#define SLEEP_SECONDS 120 /* Time to wait in seconds before again checking to see if need to switch the EV charger switch on or off */

#define VALUE_CHARGE_START_HOUR 23 /* Hour of when the least expensive PG&E tier starts */
#define VALUE_CHARGE_END_HOUR 7    /* Hour of when the least expensive PG&E tier ends */

#define EV_CHARGING_CURRENT 1.4 /* Number of kilowatts the EV draws when charging; adjust this according to car model */

#define SWITCHING_THRESHOLD 0 /* If current house + EV_CHARGING_CURRENT draw is less than this then turn the EV charger switch on */

/* To send email/txt message notifications of state changes */
#define GMAIL_SERVER "smtps://smtp.gmail.com"
#define USER "email@gmail.com"
#define PWD "password"
#define TO "mobilenumber@vtext.com"
#define FROM "email@gmail.com"

/* To talk to the Rainforest gateway */
#define RAINFOREST "http://192.168.1.4/cgi-bin/post_manager" // Eagle 200 URL
#define CONTENT_TYPE "Content-Type: text/xml"
#define USERNAME "nnnnnn" // Cloud ID is username
#define PASSWORD "nnnnnnnnnnnnnnnn" // Install Code is password

/* The POST body to read the hardware address of the meter */
static const char *ha_post_body = "<Command><Name>device_list</Name></Command>";

/* The POST body to read the InstantaneousDemand of the meter */
static const char *meter_post_body_pre = "<Command><Name>device_query</Name><DeviceDetails><HardwareAddress>";
static const char *meter_post_body_suf = "</HardwareAddress></DeviceDetails><Components><Component><Name>Main</Name><Variables><Variable><Name>zigbee:InstantaneousDemand</Name></Variable></Variables></Component></Components></Command>";

/* The URLs to turn on and off the wall plug bottom outlet */
static const char *switch_on_url =   "http://user:password@192.168.1.3:port/3?0262418C4B0F3202=I=3"; // The bottom outlet that my EV charger is plugged into
static const char *switch_off_url =  "http://user:password@192.168.1.3:port/3?0262418C4B0F3302=I=3";

enum { /* Modes of EV charger */
   OFF,
   ON,
   ON_ERROR,
   OFF_ERROR,
   ON_VC,
   ON_VC_ERROR,
   ON_METER,
   OFF_CURRENT,
   OFF_VALUE,
   ON_STARTUP,
};

/* The POST body to read the Instantaneous Demand value */
char meter_post_body[300];
char hardware_address[19];

int get_hardware_address();
int get_meter_reading();
int switch_charger(int mode);
void sendmail(int event);

char *parse_buffer = NULL;

double actual_demand = 0;

time_t mytime;
struct tm * timeinfo;

char reading[30]; // Buffer used to parse meter reading string into for emails

static const char *payload_text_on[] = {
  "To: " TO "\r\n",
  "From: " FROM " (Greg Stevens)\r\n",
  "Subject: EV Charger Switch Turned On\r\n",
  "\r\n", /* Empty line to divide headers from body, see RFC5322 */
  "Turned EV charger switch on as the solar panels are generating more than the house usage plus the EV charger usage.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_off_current[] = {
  "To: " TO "\r\n",
  "From: " FROM " (Greg Stevens)\r\n",
  "Subject: EV Charger Switch Turned Off\r\n",
  "\r\n",
  "Turned EV charger switch off as the house usage plus the EV charger usage is more than 0 kW.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_off_value[] = {
  "To: " TO "\r\n",
  "From: " FROM " (Greg Stevens)\r\n",
  "Subject: EV Charger Switch Turned Off\r\n",
  "\r\n",
  "Turned EV charger switch off as it is not in PG&E's lowest cost tier.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_on_startup[] = {
  "To: " TO "\r\n",
  "From: " FROM " (Greg Stevens)\r\n",
  "Subject: EV Charger Starting\r\n",
  "\r\n",
  "Turned EV charger switch on at startup. Waiting 60 secsonds before first meter reading to allow it to boot.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_on_error[] = {
  "To: " TO "\r\n",
  "From: " FROM " (Greg Stevens)\r\n",
  "Subject: EV Charger Error Turning On\r\n",
  "\r\n",
  "Could not turn EV charger switch on.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_off_error[] = {
  "To: " TO "\r\n",
  "From: " FROM " (Greg Stevens)\r\n",
  "Subject: EV Charger Error Turning Off\r\n",
  "\r\n",
  "Could not turn EV charger switch off.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_on_vc[] = {
  "To: " TO "\r\n",
  "From: " FROM " (Greg Stevens)\r\n",
  "Subject: EV Charger Switch Turned On\r\n",
  "\r\n",
  "Turned EV charger switch on as it is now in PG&E's lowest cost tier.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_on_vc_error[] = {
  "To: " TO "\r\n",
  "From: " FROM " (Greg Stevens)\r\n",
  "Subject: EV Charger Error Turning On\r\n",
  "\r\n",
  "Could not turn EV charger switch on during PG&E's lowest cost tier.\r\n",
  "\r\n",
  NULL
};

static const char *payload_text_on_meter[] = {
  "To: " TO "\r\n",
  "From: " FROM " (Greg Stevens)\r\n",
  "Subject: EV Charger Switch Turn On - couldn't read meter\r\n",
  "\r\n",
  "Turned EV charger switch on due to failure reading meter.\r\n",
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

int get_hardware_address() {
	
	/* Get the zigbee radio's mac address from the gateway */
	
	CURL *curl = NULL;
	CURLcode res;
	parse_buffer = NULL; // Clear the parse buffer in case next time we cannot read meter as we don't want the previous values in it
		
	/* Initialize the network interface (winsock) */
	curl_global_init(CURL_GLOBAL_ALL);

	/* Get a curl handle */
	curl = curl_easy_init();

	if (!curl) {
		printf("\n%sget_hardware_address: failed to get a curl handle.\n", ctime(&mytime));
		curl_global_cleanup();
		return 0;  
	}

   /* Set the URL that is about to receive our POST. */
   curl_easy_setopt(curl, CURLOPT_URL, RAINFOREST);
   
   /* Set the username and password */
   curl_easy_setopt(curl, CURLOPT_USERNAME, USERNAME);
   curl_easy_setopt(curl, CURLOPT_PASSWORD, PASSWORD);
   
   /* Add all the custom headers */
   struct curl_slist *chunk = NULL;
   chunk = curl_slist_append(chunk, CONTENT_TYPE);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

   /* Specify we want to POST data */
   curl_easy_setopt(curl, CURLOPT_POST, 1L);

   /* Send the POST Body to this function  */
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallbackMeter);

   /* Pass 'chunk' struct to the callback function */
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

   /* Set the POST Body data */
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, ha_post_body);

   if (DEBUG) {
	  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
   } else {
	  curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
   }

   /* Perform the request, res will get the return code */
   res = curl_easy_perform(curl);
   
   /* Free the custom headers */
   curl_slist_free_all(chunk);
   curl_easy_cleanup(curl);
   curl_global_cleanup();
   
   /* Check for errors */
   if (res != CURLE_OK) {
	  printf("\n%sget_hardware_address: curl_easy_perform() failed: %s.\n", ctime(&mytime), curl_easy_strerror(res));
	  return 0;
   }
   if (!parse_buffer) {
	  printf("\n%sget_hardware_address: parse_buffer is NULL.\n", ctime(&mytime));
      return 0;
   }
   if (!strstr(parse_buffer, "<HardwareAddress>")) { //If no Hardware Address field
	  printf("\n%sNo <HardwareAddress> token in POST response:\n%s", ctime(&mytime), parse_buffer);
	  return 0;
   }
   
   /* Parse out the hardware address */
   strncpy(hardware_address, strstr(parse_buffer, "<HardwareAddress>") + strlen("<HardwareAddress>"), 18);
   hardware_address[18] = '\0';
   
   /* Create the POST body */
   strcpy(meter_post_body, meter_post_body_pre);
   strcat(meter_post_body, hardware_address);
   strcat(meter_post_body, meter_post_body_suf);

   return 1;
}

int get_meter_reading() {

	/* Call the Rainforest APIs to get the current meter reading */

	CURL *curl = NULL;
	CURLcode res;

	char *start_demand;
	char *end_demand;
	char demand_string[11];
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

   /* Set the URL that is about to receive our POST. */
   curl_easy_setopt(curl, CURLOPT_URL, RAINFOREST);
   
   /* Set the username and password */
   curl_easy_setopt(curl, CURLOPT_USERNAME, USERNAME);
   curl_easy_setopt(curl, CURLOPT_PASSWORD, PASSWORD);
   
   /* Add all the custom headers */
   struct curl_slist *chunk = NULL;
   chunk = curl_slist_append(chunk, CONTENT_TYPE);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

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
   
   /* Free the custom headers */
   curl_slist_free_all(chunk);
   curl_easy_cleanup(curl);
   curl_global_cleanup();
   
   /* Check for errors */
   if (res != CURLE_OK) {
	  printf("\n%sget_meter_reading: curl_easy_perform() failed: %s.\n", ctime(&mytime), curl_easy_strerror(res));
	  return 0;  // Didn't get a clean meter reading so return error
   }

   if (!parse_buffer) {
	  printf("\n%sget_meter_reading: parse_buffer is NULL.\n", ctime(&mytime));
      return 0;
   }
   /* Sometimes the parse_buffer does not have the <zigbee:InstantaneousDemand> token in it so have to bail out */
   if (!strstr(parse_buffer, "<Value>")) {
	  printf("\n%sNo <Value> token for the <zigbee:InstantaneousDemand> token in POST response:\n%s", ctime(&mytime), parse_buffer + 600);
	  return 0;  // Didn't get a clean meter reading so return error
   }
   /*
	  Parse the XML that is returned in the POST Response to search for the <Value> token after the <zigbee:InstantaneousDemand> token
   */
   start_demand = strstr(parse_buffer, "<Value>") + strlen("<Value>");
   end_demand = strchr(start_demand, '<');
   strncpy(demand_string, start_demand, end_demand - start_demand);
   demand_string[end_demand - start_demand] = '\0';
   actual_demand = atof(demand_string); // Convert the text to numeric
   return 1;
}

int switch_charger(int mode) {

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
	   if (mode == ON) {
		   printf("\n%sswitch_charger: failed to get a curl handle to turn EV charger switch on.\n", ctime(&mytime));
	   } else {
		   printf("\n%sswitch_charger: failed to get a curl handle to turn EV charger switch off.\n", ctime(&mytime));
	   }
	   curl_global_cleanup();
	   return -1;
    }

   /* Set the URL */
   if (mode == ON) {
		curl_easy_setopt(curl, CURLOPT_URL, switch_on_url);
   } else {
		  curl_easy_setopt(curl, CURLOPT_URL, switch_off_url);
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
	  if (mode == ON) {
	     printf("\n%sswitch_charger: curl_easy_perform() failed turning EV charger switch on: %s.\n", ctime(&mytime), curl_easy_strerror(res));
	  } else {
		  printf("\n%sswitch_charger: curl_easy_perform() failed turning EV charger switch off: %s.\n", ctime(&mytime), curl_easy_strerror(res));
      }
	  mode = -1;
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
	if (strstr(data, "Turned EV charger switch on as the solar")) {
		sprintf(reading, "\r\nMeter reading: %.3f kW.\r\n", actual_demand);
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
	if (strstr(data, "Could not turn EV charger switch on")) {
		sprintf(reading, "\r\nMeter reading: %.3f kW.\r\n", actual_demand);
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
	if (strstr(data, "Turned EV charger switch off as the house")) {
		sprintf(reading, "\r\nMeter reading: %.3f kW.\r\n", actual_demand);
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
	if (strstr(data, "Turned EV charger switch off as it is not")) {
		sprintf(reading, "\r\nMeter reading: %.3f kW.\r\n", actual_demand);
		memcpy(ptr+(strlen(data)-2), reading, strlen(reading));
		upload_ctx->lines_read++;
        return (strlen((char *)ptr));
    }
    upload_ctx->lines_read++;

    return len;
  }

  return 0;
}

static size_t payload_source_on_meter(void *ptr, size_t size, size_t nmemb, void *userp)
{
  struct upload_status *upload_ctx = (struct upload_status *)userp;
  const char *data;

  if((size == 0) || (nmemb == 0) || ((size*nmemb) < 1)) {
    return 0;
  }

  data = payload_text_on_meter[upload_ctx->lines_read];

  if(data) {
    size_t len = strlen(data);
    memcpy(ptr, data, len);

    upload_ctx->lines_read++;

    return len;
  }

  return 0;
}
static size_t payload_source_on_startup(void *ptr, size_t size, size_t nmemb, void *userp)
{
  struct upload_status *upload_ctx = (struct upload_status *)userp;
  const char *data;

  if((size == 0) || (nmemb == 0) || ((size*nmemb) < 1)) {
    return 0;
  }

  data = payload_text_on_startup[upload_ctx->lines_read];

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
	if (strstr(data, "Could not turn EV charger switch off")) {
		sprintf(reading, "\r\nMeter reading: %.3f kW.\r\n", actual_demand);
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
	if (strstr(data, "Turned EV charger switch on as it is now")) {
		sprintf(reading, "\r\nMeter reading: %.3f kW.\r\n", actual_demand);
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
	if (strstr(data, "Could not turn EV charger switch on during")) {
		sprintf(reading, "\r\nMeter reading: %.3f kW.\r\n", actual_demand);
		memcpy(ptr+(strlen(data)-2), reading, strlen(reading));
		upload_ctx->lines_read++;
        return (strlen((char *)ptr));
    }
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
  curl_easy_setopt(curl, CURLOPT_URL, GMAIL_SERVER);

  /* Set username and password */
  curl_easy_setopt(curl, CURLOPT_USERNAME, USER);
  curl_easy_setopt(curl, CURLOPT_PASSWORD, PWD);

  /* Set FROM address */
  curl_easy_setopt(curl, CURLOPT_MAIL_FROM, FROM);

  /* Set to use SSL */
  curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);

    /* Add recipient(s) */
	recipients = curl_slist_append(recipients, TO);	// text message
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
		case ON_STARTUP:
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source_on_startup);
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
		case ON_METER:
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source_on_meter);
			break;
		case ON_VC_ERROR:
			curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source_on_vc_error);
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
       printf("\n%scurl_easy_perform() failed sending email: %s.\n", ctime(&mytime), curl_easy_strerror(res));

    /* Free the list of recipient(s) */
    curl_slist_free_all(recipients);

    /* Cleanup */
	curl_easy_cleanup(curl);
	curl_global_cleanup();
}

int main() {

    int current_mode = ON_STARTUP; // Set the current state of the charger switch to startup mode
	
	/* First, try to turn the EV switch on. If fails, then wait 1 minute and keep trying */
	mytime = time(NULL); // Get current date and time and parse out time components
	timeinfo = localtime(&mytime);
	printf("\n%sTurning on EV charger switch at startup...\n", ctime(&mytime));
	while (1) {
		mytime = time(NULL); // Get current date and time and parse out time components
		timeinfo = localtime(&mytime);
		if (switch_charger(ON) != ON) {
			printf("\n%sCould not turn EV charger switch on at startup.\nTrying again in 1 minute...\n", ctime(&mytime));
			sleep(60);
		} else {
			break;
		}
	}
	printf("\n%sTurned EV charger switch on at startup.\n", ctime(&mytime));
	
	/* Now, read the gateway for the meter's Hardware Address */
    mytime = time(NULL); // Get current date and time and parse out time components
	timeinfo = localtime(&mytime);
	printf("\n%sReading the gateway for the meter's Hardware Address at startup...\n", ctime(&mytime));
	while (1) {
		mytime = time(NULL); // Get current date and time and parse out time components
		timeinfo = localtime(&mytime);
		if (!get_hardware_address()) {
			printf("\n%sCould not get the meter's Hardware Address from the gateway at startup.\nTrying again in 1 minute...\n", ctime(&mytime));
			sleep(60);
		} else {
			break;
		}
	}
    printf("\n%sRead the meter's Hardware Address at startup: %s\n", ctime(&mytime), hardware_address);
   
	/* Start our endless while loop of checking the time and meter reading and turning the switch on or off accordingly */
    while (1) {
		/* Get current date and time and parse out components */
		mytime = time(NULL);
		timeinfo = localtime(&mytime);
		    
		/* Get the meter reading */
		if (!get_meter_reading()) {
			sleep(SLEEP_SECONDS); // Couldn't read meter so wait until next time to check again
			continue;
		} 			
		/* If the current time of day is in the Value Charge time frame, then turn the EV charger switch on */
		if ((timeinfo->tm_hour < VALUE_CHARGE_END_HOUR) || (timeinfo->tm_hour >= VALUE_CHARGE_START_HOUR)) {
			if (switch_charger(ON) != ON) { // Turn EV charger switch on
				printf("\n%sCould not turn EV charger switch on during PG&E's lowest cost tier.\n", ctime(&mytime));
				sendmail(ON_VC_ERROR);
				sleep(SLEEP_SECONDS); // Wait until next time to check again
				continue;
		    } else {
			   if (current_mode == OFF) {
				   printf("\n%sTurned EV charger switch on as it is now in PG&E's lowest cost tier.\n", ctime(&mytime));
				   sendmail(ON_VC);
			   }
			   current_mode = ON_VC; // Set to indicate on during the Value Charge period
			}
			printf("\n%sMeter reading: %.3f kW.\nEV charger switch is on (Value Charge time period).\n", ctime(&mytime), actual_demand);
		} else {
			/* If the EV charger switch is currently ON, then don't include the EV_CHARGING_CURRENT in the below if statement equation, but do if OFF */
			if ((current_mode == ON && (actual_demand <= SWITCHING_THRESHOLD)) ||
			   (current_mode == OFF && ((actual_demand + EV_CHARGING_CURRENT) <= SWITCHING_THRESHOLD))) {
				if (switch_charger(ON) == ON) { // Turn EV charger switch on
					if (current_mode == OFF) {
					  printf("\n%sTurned EV charger switch on as the solar panels are generating more than the house usage plus the EV charger usage.\n", ctime(&mytime));
					  sendmail(ON);
					}
					current_mode = ON; // Set state to on
				} else {
					printf("\n%sCould not turn EV charger switch on.\n", ctime(&mytime));
					sendmail(ON_ERROR);
				}				  
			} else {
				if (switch_charger(OFF) == OFF) { // Turn EV charger switch off
					if (current_mode == ON_VC) {
					   printf("\n%sTurned EV charger switch off as it is not in PG&E's lowest cost tier.\n", ctime(&mytime));
					   sendmail(OFF_VALUE);
					} else {
						if (current_mode == ON) {
						   printf("\n%sTurned EV charger switch off as the house usage plus the EV charger usage is more than %d kW.\n", ctime(&mytime), SWITCHING_THRESHOLD);
						   sendmail(OFF_CURRENT);
						}
					}				
					current_mode = OFF; // Set state to off								 
				} else {
					printf("\n%sCould not turn EV charger switch off.\n", ctime(&mytime));
					sendmail(OFF_ERROR);
				}
			}
			if (current_mode == ON || current_mode == ON_VC) {
				printf("\n%sMeter reading: %.3f kW.\nEV charger switch is on.\n", ctime(&mytime), actual_demand);
			} else {
				printf("\n%sMeter reading: %.3f kW.\nEV charger switch is off.\n", ctime(&mytime), actual_demand);
			}
		}				
		sleep(SLEEP_SECONDS); // Wait until next time to check again
	}
}