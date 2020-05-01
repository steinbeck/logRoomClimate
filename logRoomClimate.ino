#include <ESP8266WiFi.h>
#include <PubSubClient.h> // MQTT
#include <ThingSpeak.h>
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     //Local WebServer used to serve the configuration portal
#include <IotWebConf.h>
#include <NTPClient.h>
// Begin includes from simpleDSTAdjust library example
#include <Ticker.h>
#include <time.h>
#include <simpleDSTadjust.h>
// End of includes from simpleDSTAdjust library example
#include <WiFiUdp.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <BME280I2C.h>
#include <Wire.h>

#define SERIAL_BAUD 115200

// -------------- Configuration options for simpleDSTAdjust-----------------

// Update time from NTP server every 5 hours
#define NTP_UPDATE_INTERVAL_SEC 5*3600

// Maximum of 3 servers
#define NTP_SERVERS "time.nist.gov", "europe.pool.ntp.org", "pool.ntp.org"

// Daylight Saving Time (DST) rule configuration
// Rules work for most contries that observe DST - see https://en.wikipedia.org/wiki/Daylight_saving_time_by_country for details and exceptions
// See http://www.timeanddate.com/time/zones/ for standard abbreviations and additional information
// Caution: DST rules may change in the future

//German Time Zone (Berlin)
#define timezone +1 // Central European Time
struct dstRule StartRule = {"CEST", Last, Sun, Mar, 2, 3600};    // Daylight time = UTC/GMT +2 hours
struct dstRule EndRule = {"CET", Last, Sun, Oct, 2, 0};       // Standard time = UTC/GMT +1 hour

// --------- End of configuration section simpleDSTAdjust ---------------

// Setup simpleDSTadjust Library rules
simpleDSTadjust dstAdjusted(StartRule, EndRule);

Ticker ticker1;
int32_t tick;

// flag changed in the ticker function to start NTP Update
bool readyForNtpUpdate = true;

BME280I2C bme;    // Default : forced mode, standby time = 1000 ms
// Oversampling = pressure ×1, temperature ×1, humidity ×1, filter off,

#define sleepTimeS 60
//seconds of deep sleep between updates
#define STRING_LEN 128
#define ENABLE_GxEPD2_GFX 0

#define CONFIG_VERSION "td9"


// mapping suggestion from Waveshare SPI e-Paper to Wemos D1 mini
// BUSY -> D2, RST -> D4, DC -> D3, CS -> D8, CLK -> D5, DIN -> D7, GND -> GND, 3.3V -> 3.3V

// mapping suggestion from Waveshare SPI e-Paper to generic ESP8266
// BUSY -> GPIO4, RST -> GPIO2, DC -> GPIO0, CS -> GPIO15, CLK -> GPIO14, DIN -> GPIO13, GND -> GND, 3.3V -> 3.3V

#if defined (ESP8266)
// ***** for mapping of Waveshare e-Paper ESP8266 Driver Board *****
// select one , can use full buffer size (full HEIGHT)
//GxEPD2_BW<GxEPD2_213, GxEPD2_213::HEIGHT> display(GxEPD2_213(/*CS=15*/ SS, /*DC=4*/ 4, /*RST=5*/ 5, /*BUSY=16*/ 16));
GxEPD2_BW<GxEPD2_154, GxEPD2_154::HEIGHT> display(GxEPD2_154(/*CS=15*/ SS, /*DC=2*/ 2, /*RST=12*/ 12, /*BUSY=16*/ 16));
#endif

WiFiClient espClient;
WiFiUDP ntpUDP;


const char thingName[] = "Thermometer-Ding";

// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "smrtTHNG8266";

//PubSubClient client(espClient);
long lastMsg = 0;
char msg[50];
int value = 0;
char timeString[10] = "012345678";
int minutes;
int hours;
int years;
int months;
int dayofmonth;

boolean needReset = false;
char thingSpeakAPIKeyValue[STRING_LEN];
char thingSpeakChannelIDValue[STRING_LEN];

float humidity(NAN);
float temperature(NAN);
float pressure(NAN);
float batteryVoltage(NAN);

void configSaved();
boolean formValidator();
char batteryChar[15];
char temperatureChar[15];
char timeChar[15];
char dateChar[15];
char humidityChar[15];
char pressureChar[15];
char* monthNames[]={ "Jan", "Feb", "Mar", "Apr", "May","Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

DNSServer dnsServer;
WebServer server(80);

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
// -- When CONFIG_PIN is pulled to ground on startup, the Thing will use the initial
//      password to buld an AP. (E.g. in case of lost password)
//#define CONFIG_PIN 2

// -- Status indicator pin.
//      First it will light up (kept LOW), on Wifi connection it will blink,
//      when connected to the Wifi it will turn off (kept HIGH).
//#define STATUS_PIN LED_BUILTIN


IotWebConfParameter thingSpeakAPIKeyParam = IotWebConfParameter("ThingSpeak API Key", "thingSpeakAPIKey", thingSpeakAPIKeyValue, STRING_LEN);
IotWebConfParameter thingSpeakChannelIDParam = IotWebConfParameter("ThingSpeak Channel ID", "thingSpeakChannelID", thingSpeakChannelIDValue, STRING_LEN);

//int pinState = HIGH;

void setup() {
  wifi_set_sleep_type(LIGHT_SLEEP_T);
  Serial.begin(SERIAL_BAUD);  // Serial connection from ESP-01 via 3.3v console cable
  Serial.println();
  Serial.println("Starting ESP8266 climate data logger");

  Serial.println("Configuring BME280 sensor");

  Wire.begin();

  while (!bme.begin())
  {
    Serial.println("Could not find BME280 sensor!");
    delay(1000);
  }

  // bme.chipID(); // Deprecated. See chipModel().
  Serial.print("Found model ");
  Serial.println(bme.chipModel());
  switch (bme.chipModel())
  {
    case BME280::ChipModel_BME280:
      Serial.println("Found BME280 sensor! Success.");
      break;
    case BME280::ChipModel_BMP280:
      Serial.println("Found BMP280 sensor! No Humidity available.");
      break;
    default:
      Serial.println("Found UNKNOWN sensor! Error!");
  }


  // -- Initializing the configuration.
  // iotWebConf.setStatusPin(STATUS_PIN);
  // iotWebConf.setConfigPin(CONFIG_PIN);

  iotWebConf.addParameter(&thingSpeakAPIKeyParam);
  iotWebConf.addParameter(&thingSpeakChannelIDParam);


  // -- Initializing the configuration.
  boolean validConfig = iotWebConf.init();
  iotWebConf.setApTimeoutMs(0);
  if (!validConfig)
  {
    Serial.println("No valid config");
    thingSpeakAPIKeyValue[0] = '\0';
    thingSpeakChannelIDValue[0] = '\0';
  }


  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", [] { iotWebConf.handleConfig(); });
  server.onNotFound([]() {
    iotWebConf.handleNotFound();
  });

  while (!Serial) {} // Wait

  tick = NTP_UPDATE_INTERVAL_SEC; // Init the NTP update countdown ticker
  ticker1.attach(1, secTicker); // Run a 1 second interval Ticker

  Serial.println("Ready.");
  display.init(115200);
}

void loop() {

  // Will not run
  iotWebConf.doLoop();
  if (iotWebConf.getState() == IOTWEBCONF_STATE_ONLINE)
  {
    //readBME280Data();
    readBME280Data();

    delay(1000);
    if (readyForNtpUpdate)
    {
      readyForNtpUpdate = false;
      updateNTP();
      Serial.print("Updated time from NTP Server: ");
      printTime(0);
      Serial.print("Next NTP Update: ");
      printTime(tick);
      delay(1000);
    }

    batteryVoltage = readBatteryVoltage();
    
    readTime();

    formatDateAndTime();

  
    String temperatureString = String(temperature, 1);
    temperatureString.toCharArray(temperatureChar, 15);

    String humidityString = String(humidity, 0) + "% humidity" ;
    humidityString.toCharArray(humidityChar, 15);

    String pressureString = String(pressure/100, 0) + " millibar" ;
    pressureString.toCharArray(pressureChar, 15);

    
    String batteryString = "   V=" + String(batteryVoltage, 1);
    batteryString.toCharArray(batteryChar, 15);

    displayValues();
    reportDataToThinkSpeak();

    //Sleep and repeat
    iotWebConf.setApTimeoutMs(30000);

    Serial.print("Going to sleep for 60 s");
    delay(60000);
  }

  if (needReset)
  {
    Serial.println("Rebooting after 1 second.");
    iotWebConf.delay(1000);
    ESP.restart();
  }
  delay(100);
}

/**
   Handle web requests to "/" path.
*/
void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>IotWebConf 01 Minimal</title></head><body>Hello world!";
  s += "Go to <a href='config'>configure page</a> to change settings.";
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}



void readBME280Data()
{

  BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);
  BME280::PresUnit presUnit(BME280::PresUnit_Pa);

  bme.read(pressure, temperature, humidity, tempUnit, presUnit);

  Serial.print("Temp: ");
  Serial.println(temperature);
  Serial.println("°" + String(tempUnit == BME280::TempUnit_Celsius ? 'C' : 'F'));
  Serial.print("\t\tHumidity: ");
  Serial.print(humidity);
  Serial.println("% RH");
  Serial.print("\t\tPressure: ");
  Serial.print(pressure);
  Serial.println(" Pa");

  delay(1000);
}


/*
 * Writes all data to ThinkSpeak
 */
void reportDataToThinkSpeak()
{
  ThingSpeak.begin(espClient);

  int result = 0;

  Serial.println("Attempting upload to ThingsSpeak:");
  //String channelString = String(thingSpeakChannelIDValue);
  long channelID = atol(thingSpeakChannelIDValue);
  Serial.println(channelID);
  //snprintf (msg, 75, "%ld", (int)humidity);
  int x = ThingSpeak.writeField(channelID, 1, temperature, thingSpeakAPIKeyValue);

  // Check the return code
  if (x == 200) {
    Serial.println("Temperature update successful.");
  }
  else {
    Serial.println("Problem updating temperature. HTTP error code " + String(x));
  }

  delay(16000); // we need 15 seconds delay between writes to ThinkSpeak


  //snprintf (msg, 75, "%ld", (int)temperature);
  x = ThingSpeak.writeField(channelID, 2, (int)humidity, thingSpeakAPIKeyValue);

  // Check the return code
  if (x == 200) {
    Serial.println("Humidity update successful.");
  }
  else {
    Serial.println("Problem updating humidity. HTTP error code " + String(x));
  }

  delay(16000); // we need 15 seconds delay between writes to ThinkSpeak


  x = ThingSpeak.writeField(channelID, 3, (int)(pressure/100), thingSpeakAPIKeyValue);

  // Check the return code
  if (x == 200) {
    Serial.println("Pressure update successful.");
  }
  else {
    Serial.println("Problem updating pressure. HTTP error code " + String(x));
  }
  
}

/*
 * Shows date, time, battery power as well as temperature, humidity and pressure from BME280 to the epaper display
 */
void displayValues()
{
  display.init(115200);
  display.setRotation(1);
  display.setFont(&FreeMonoBold12pt7b);
  display.setTextColor(GxEPD_BLACK);
  int16_t tbx, tby; uint16_t tbw, tbh;
  display.getTextBounds(timeChar, 0, 0, &tbx, &tby, &tbw, &tbh);
  uint16_t x = 10;
  uint16_t y = (display.height() + tbh) / 6; // y is base line!
  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(x, y);
    display.print(timeChar);
    display.print(batteryChar);

    y = (display.height() + tbh) / 6 * 2;
    display.setCursor(x, y);
    display.print(dateChar);

    y = (display.height() + tbh) / 6 * 3;
    display.setCursor(x, y);
    display.print(temperatureChar);
    int old_y = y;
    display.getTextBounds(temperatureChar, 0, 0, &tbx, &tby, &tbw, &tbh);
    y = y - (tbh / 2);
    display.setCursor(x + tbw + 4, y);
    //display.setFont(&FreeMonoBold9pt7b);
    display.print("o");
    //display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(x + tbw + 15, old_y);
    display.print("C");


    y = (display.height() + tbh) / 6 * 4;
    display.setCursor(x, y);
    display.print(humidityChar);

    y = (display.height() + tbh) / 6 * 5;
    display.setCursor(x, y);
    display.print(pressureChar);

  }
  while (display.nextPage());
}


void configSaved()
{
  Serial.println("Configuration was updated.");
  needReset = true;
}

boolean formValidator()
{
  Serial.println("Validating form.");
  boolean valid = true;

  int l = server.arg(thingSpeakAPIKeyParam.getId()).length();
  if (l < 3)
  {
    thingSpeakAPIKeyParam.errorMessage = "Please provide at least 3 characters!";
    valid = false;
  }

  return valid;
}


// NTP timer update ticker
void secTicker()
{
  tick--;
  if (tick <= 0)
  {
    readyForNtpUpdate = true;
    tick = NTP_UPDATE_INTERVAL_SEC; // Re-arm
  }

  //printTime(0);  // Uncomment if you want to see time printed every second
}


/*
 * Reads the current time and date from an NTP server
 */
void updateNTP() {

  configTime(timezone * 3600, 0, NTP_SERVERS);

  delay(500);
  while (!time(nullptr)) {
    Serial.print("#");
    delay(1000);
  }
}


/*
 * Reads date and time from the NTP data structure and puts it into the right format
 */
void readTime()
{
  char *dstAbbrev;
  time_t t = dstAdjusted.time(&dstAbbrev);
  struct tm *timeinfo = localtime (&t);
  hours = timeinfo->tm_hour;
  //Serial.println("\nHours: " + hours);
  minutes = timeinfo->tm_min;
  //Serial.println("\nMinutes: " + minutes);
  years = timeinfo->tm_year + 1900;
  //Serial.println("\nYear: " + years);
  months = timeinfo->tm_mon + 1;
  //Serial.println("\nMonth: " + months);
  dayofmonth = timeinfo->tm_mday;
  //Serial.println("\nDay: " + dayofmonth);
  delay(1000);
}

/*
 * Helper method to print the current time and date to serial output for debuggin purposes
 */
void printTime(time_t offset)
{
  char buf[30];
  char *dstAbbrev;
  time_t t = dstAdjusted.time(&dstAbbrev) + offset;
  struct tm *timeinfo = localtime (&t);

  int hour = (timeinfo->tm_hour + 11) % 12 + 1; // take care of noon and midnight
  sprintf(buf, "%02d/%02d/%04d %02d:%02d:%02d%s %s\n", timeinfo->tm_mon + 1, timeinfo->tm_mday, timeinfo->tm_year + 1900, hour, timeinfo->tm_min, timeinfo->tm_sec, timeinfo->tm_hour >= 12 ? "pm" : "am", dstAbbrev);
  Serial.print(buf);
}


/*
 * Formats date and time to look nice and always have leading zeros
 */
void formatDateAndTime()
{
  String hoursString;
  String minutesString;

  if (hours < 10) hoursString = "0" + String(hours);
  else  hoursString = String(hours);

  if (minutes < 10) minutesString = "0" + String(minutes);
  else  minutesString = String(minutes);

  String timeString = hoursString + ":" + minutesString;
  timeString.toCharArray(timeChar, 15);

  String dateString = String(dayofmonth) + " " + monthNames[months-1] + " " + String(years);
  dateString.toCharArray(dateChar, 15);
}

/*
 *  Reads A0 analogue pin, which is connected to the battery input via a voltage divider.
 *  This voltage divider is the built-in plus 100k soldered to the battery plus pin.
 *  This follows https://arduinodiy.wordpress.com/2016/12/25/monitoring-lipo-battery-voltage-with-wemos-d1-minibattery-shield-and-thingspeak/
 */

float readBatteryVoltage()
{
  float volt=0.0;
  pinMode(A0, INPUT);
  unsigned int raw = analogRead(A0);
  Serial.print("\nRaw analog read: " + String(raw));
  volt=raw/1023.0;
  volt=volt*4.2;
  Serial.print("\nResulting voltage: " + String(volt));
  return volt;

}
