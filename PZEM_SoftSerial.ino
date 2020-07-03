/*
This code works as per 20jun2020
The new version is in the folder 
/home/ctv/Documents/PZEM_SoftSerial/PZEM_SoftSerial/
The new (this) version should do:
[X]Change from 5 minutes to 1 minute
[o]Interrogating a google server for the date and time 
[o]Reset the energy on first day of each month at 00:00:00

To do:
1.  [X]include libraries to communicate with ThingSpeak
2.  [X]transform the data read from PZEM in text to be sent to ThingSpeak
3.  [o]synchronize with MQTT or with NTP server
4.  [X]save the max and the min for <voltage> and the max for <power> and send them to TS as status
5.  [X]use ESP-01 with SoftwareSerial: Rx -> gpio2, Tx -> gpio0

6.  [o]use MQTT, subscribe to <date/time> and send data each 5 min exactly
7.  [_]websockets
8.  [_]resetEnergy at the end of the cycle (see HQ)

The update of the channel is made each 5min.
    bool setPowerAlarm(uint16_t watts);
    bool getPowerAlarm();

*/
/* Use software serial for the PZEM
 * Pin gpio2 Rx (Connects to the Tx pin on the PZEM)
 * Pin gpio0 Tx (Connects to the Rx pin on the PZEM)
*/
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <PZEM004Tv30.h>
#include "credentials.h"            //this file keeps some private settings

WiFiClient      wificlient;
WiFiClient			glient;		//Google client

//PZEM004Tv30   pzem(11, 12);   //original config 
//PZEM004Tv30   pzem(4, 5);     //Rx,Tx,(D2,D1) on nodeMCU 
//PZEM004Tv30   pzem(13, 15);   //Rx,Tx,(D7,D8) another config
PZEM004Tv30     pzem(2, 0);     //Rx,Tx,(gpio2,gpio0) on ESP-01 

String	tsStatus;		        //ThingSpeak, 255 char, zero terminated
String  status;

int     error   =   0;          //0 no error; 1,2,4,8,16,32 errors for each parameter
float   voltage, current, power, energy, frequency, pf;
float   Vmax    =   120.0;
float   Vmin    =   120.0;
float   Pmax    =   0.0;
int     readAvg =   29;        //nr of measurements averaged and sent to TS
int     hour, minute, second, day, month, year;

//==============================================================================
//  Functions
//==============================================================================
void wifiConnect(int n) 
//  "n" is the max number of tries to connect to SSID. If "n" reaches zero then
//  wait for 15min and tries again for k (=100) cycles (100 * 15min ~ 25 hours)
{
	WiFi.disconnect();
    WiFi.mode(WIFI_STA);
	WiFi.begin(ssid,password);
    int nn = n;
    int k = 100;        //  [_] to put this at the beginning
    while (k > 0)
    {
        while ((WiFi.status() != WL_CONNECTED) && (n > 0))
        {
            delay(1000);
            Serial.print(n);Serial.print(" >");
            //Serial.print(F(":"));
            n--;
        }//while
        //either connected or n = 0
        //if connected -> break
        if (WiFi.status() == WL_CONNECTED)
        {
            Serial.print(F("\nWiFi connected to IP address: "));		//@
            Serial.println(WiFi.localIP());				            //@
            
            break;//exit from outward while
            //return;
        }
        if (n == 0)         //not connected to wifi during n*1000ms 
        {
            n = nn;         //start over with initial n
            k--;
            if (k != 0)
            {
                delay(300000);      // 5 min
            }
            else    //-> k == 0
            {
                Serial.println(F("Restart"));
                delay(50);//to allow the serial to finish
                ESP.restart();
            }
        }
    }//while
    return;//this point is reached only from the above 'break'
}

String getTime()
{
//in the main prog is defined the client to access google as "glient"
  Serial.println(F("connecting to google"));//@
  //Serial.println(google);			//@
  
  if (!glient.connect(ghost, httpPort)) 
  {
      Serial.println(F("connection failed"));
      return "google_fail";
  }

  // This will send the request to the server
  glient.println("HEAD / HTTP/1.1");
  glient.println("Host: www.google.com"); // "Host: www.google.com"
  glient.println("Accept: */*");
  glient.println("User-Agent: Mozilla/4.0 (compatible; esp8266 Arduino;)");
  glient.println("Connection: close");
  glient.println();
  delay(500);

  // Read all the characters of the reply from server and print them to Serial
  String reply = String("");
  while(glient.available())
  {
    char c = glient.read();
    reply = reply + String(c);
  }
  //Serial.print(reply);		//@
  
  String d = reply.substring(reply.indexOf("Date: ")+11,reply.indexOf("Date: ")+23);
  String t = reply.substring(reply.indexOf("Date: ")+23,reply.indexOf("Date: ")+35);

  hour      = t.substring(0, 2).toInt();
  minute    = t.substring(3, 5).toInt();
  second    = t.substring(6, 8).toInt();
  day       = d.substring(0, 2).toInt();
  month     = d.substring(3, 5).toInt();
  year      = d.substring(6, 8).toInt();

  hour = hour + 16;//for summer is 16, for winter is 15
    if (hour >= 24) 
    {
        hour = hour % 24;       //This is GMT - 4 -> Mtl hour
    }

  if(!glient.connected())
  {
    //Serial.println("disconnecting");
    glient.stop();
  }
  Serial.println("connection closed");		//@
  return t;
}


void setup() 
{
    Serial.begin(115200);
  	tsStatus = String("");
  	Serial.println(F("\nFilename: PZEM_SoftSerial.ino/30jun2020 "));
	//WiFi.persistent(false);			//see ESP8266WiFiGeneric.cpp - WiFi library
    //Wifi.persistent(false) is used for deep-sleep, to keep wifi param in ram
    wifiConnect(60);                //try for 60 seconds
    glient.setTimeout(5000);        //to the beginning
    //getTime();
}

void loop() 
{
/*
the time is got first in setup(). The loop is parsed each two seconds so 
I need a counter variable (or a time variable) to check the local time (millis)
to know when to ask again google for date&time.
I need also a procedure to sync on zero seconds.
The data is sent each minute. 
*/
    if ((WiFi.status() != WL_CONNECTED))
    {
        wifiConnect(60);    
    }
    
    float Voltage = 0.0, Current = 0.0, Power = 0.0, Energy = 0.0, Frequency = 0.0, PF = 0.0;
    for (int i = 0; i < readAvg; i++)
    {
        error = 0;
        voltage = pzem.voltage();
        if( !isnan(voltage) )
        {
            Serial.print("Voltage: "); Serial.print(voltage); Serial.println("V");
            Voltage += voltage;
            //check for Max and Min values
            if (voltage > Vmax) 
            {
                Vmax = voltage;
            }
            if (voltage < Vmin) 
            {
                Vmin = voltage;
            }
            //send Vmax and Vmin as tsStatus
            Serial.print("\tVmax = ");Serial.print(Vmax);Serial.println("V");
            Serial.print("\tVmin = ");Serial.print(Vmin);Serial.println("V");
        } 
        else 
        {
            Serial.println(F("Error reading voltage"));
            error = error + 1;
        }

        current = pzem.current();
        if( !isnan(current) )
        {
            Serial.print("Current: "); Serial.print(current); Serial.println("A");
            Current += current;
        } 
        else 
        {
            Serial.println(F("Error reading current"));
            error = error + 2;
        }

        power = pzem.power();
        if( !isnan(power) )
        {
            Serial.print("Power: "); Serial.print(power); Serial.println("W");
            Power += power;
            //check for Max value
            if (power > Pmax) 
            {
                Pmax = power; 
            }//--> to send Pmax as tsStatus
            Serial.print("\tPmax = ");Serial.print(Pmax);Serial.println("W");
        } 
        else 
        {
            Serial.println(F("Error reading power"));
            error = error + 4;
        }

        energy = pzem.energy();
        if( !isnan(energy) )
        {
            Serial.print("Energy: "); Serial.print(energy,3); Serial.println("kWh");
            Energy += energy;
        } 
        else 
        {
            Serial.println(F("Error reading energy"));
            error = error + 8;
        }

        frequency = pzem.frequency();
        if( !isnan(frequency) )
        {
            Serial.print("Frequency: "); Serial.print(frequency, 1); Serial.println("Hz");
            Frequency += frequency;
        } 
        else 
        {
            Serial.println(F("Error reading frequency"));
            error = error + 16;
        }

        pf = pzem.pf();
        if( !isnan(pf) )
        {
            Serial.print("PF: "); Serial.println(pf);
            PF += pf;
        } 
        else 
        {
            Serial.println(F("Error reading power factor"));
            error = error + 32;
        }
        Serial.println();
        if (error > 0)
        {
            Serial.print("--> Error: "); Serial.println(error);Serial.println();
        }
        delay(1950);    
    }//for
    
    voltage = Voltage / readAvg;
    current = Current / readAvg;
    power = Power / readAvg;
    energy = Energy / readAvg;
    frequency = Frequency / readAvg;
    pf = PF / readAvg;
    status = "Error = " + String(error) + " / Pmax = " + String(Pmax) + " / Vmax = " + String(Vmax) + " / Vmin = " + String(Vmin);
    tsStatus = String(status).c_str();
    //Serial.println(status);
    Serial.println(tsStatus);
    //delay(2000);
    transferData();
    Vmax = 120.0;
    Vmin = 120.0;
    Pmax = 0.0;
}
//------------------------------------------------------------------------------

void transferData()
//transfer data to ThingSpeak
{
	Serial.println(F("transferData()"));                //@
    String url = "/update?api_key=" + ThingSpeak_key +
        "&field1=" + energy +
        "&field2=" + power +
        "&field3=" + voltage +
        "&field4=" + current +
        "&field5=" + frequency +
        "&field6=" + pf +
        "&status=" + tsStatus;  //String(tsStatus).c_str();
        //see the notes at the top of this file.
    if (!wificlient.connect(ThingSpeak, 80))
    {
        Serial.println(F("connection to ThingSpeak failed"));
    }
    else
    {
        wificlient.print(String("GET ") + url + " HTTP/1.1\r\n" + "Host: " +
        ThingSpeak + "\r\n" + "Connection: keep-alive\r\n\r\n");

        while (!wificlient.available())
        {
              //waiting...
        }
        delay(200);
    }

    if (gettsStatus()) Serial.println(F("Data transfer OK to ThingSpeak"));  //@
}

boolean gettsStatus()
{
    Serial.println(F("getStatus()"));                   //@
	bool stat;
    String _line;

    _line = wificlient.readStringUntil('\n');
    int separatorPosition = _line.indexOf("HTTP/1.1");

    if (separatorPosition >= 0)
    {
        if (_line.substring(9, 12) == "200")
            stat = true;
        else
            stat = false;
    return  stat;
    }
}
