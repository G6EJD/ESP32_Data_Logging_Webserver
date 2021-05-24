/* ESP8266 plus WEMOS SHT30-D Sensor with a Temperature and Humidity Web Server
 Automous display of sensor results on a line-chart, gauge view and the ability to export the data via copy/paste for direct input to MS-Excel
 The 'MIT License (MIT) Copyright (c) 2016 by David Bird'. Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, 
 distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
 following conditions: 
   The above copyright ('as annotated') notice and this permission notice shall be included in all copies or substantial portions of the Software and where the
   software use is visible to an end-user.
   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHOR OR COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF, OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
See more at http://www.dsbird.org.uk
*/

#ifdef ESP8266
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h> 
  #define TZone 0 // or set you your requirements e.g. -5 for EST
  //#include <Server.h>
  #include <NTPClient.h>
  #include <WiFiUdp.h>
  WiFiUDP ntpUDP; //** NTP client class
  NTPClient timeClient(ntpUDP);
  #include <fs.h>
#else
  #include <WiFi.h>
  #include <ESP32WebServer.h>  //https://github.com/Pedroalbuquerque/ESP32WebServer download and place in your Libraries folder
  #include <WiFiClient.h>
  #include "FS.h"
  #include "SPIFFS.h"
#endif
#include <SPI.h>
#include <time.h>        

#include <WEMOS_SHT3X.h>     // https://github.com/wemos/WEMOS_SHT3x_Arduino_Library

#include "credentials.h"

SHT3X sht30(0x45);            // SHT30 object to enable readings (I2C address = 0x45)
String version = "v1.0";      // Version of this program
String site_width = "1023";
WiFiClient client;

#ifdef ESP8266
  ESP8266WebServer server(80); // Start server on port 80 (default for a web-browser, change to your requirements, e.g. 8080 if your Router uses port 80
                               // To access server from the outside of a WiFi network e.g. ESP8266WebServer server(8266); and then add a rule on your Router that forwards a
                               // connection request to http://your_network_ip_address:8266 to port 8266 and view your ESP server from anywhere.
                               // Example http://g6ejd.uk.to:8266 will be directed to http://192.168.0.40:8266 or whatever IP address your router gives to this server
#else
  ESP32WebServer server(80); // Start server on port 80 (default for a web-browser, change to your requirements, e.g. 8080 if your Router uses port 80
#endif

int       log_time_unit  = 15;  // default is 1-minute between readings, 10=15secs 40=1min 200=5mins 400=10mins 2400=1hr 
int       time_reference = 60;  // Time reference for calculating /log-time (nearly in secs) to convert to minutes
int const table_size     = 72;  // 80 is about the maximum for the available memory and Google Charts, based on 3 samples/hour * 24 * 1 day = 72 displayed, but not stored!
int       index_ptr, timer_cnt, log_interval, log_count, max_temp, min_temp;
String    webpage,time_now,log_time,lastcall,time_str, DataFile = "datalog.txt";
bool      AScale, auto_smooth, AUpdate, log_delete_approved;
float     temp, humi;

typedef signed short sint16;
typedef struct {
  int     lcnt; // Sequential log count
  String ltime; // Time record of when reading was taken
  sint16 temp;  // Temperature values, short unsigned 16-bit integer to reduce memory requirement, saved as x10 more to preserve 0.1 resolution
  sint16 humi;  // Humidity values, short unsigned 16-bit integer to reduce memory requirement, saved as x10 more to preserve 0.1 resolution
} record_type;

record_type sensor_data[table_size+1]; // Define the data array

void setup() {
  Serial.begin(115200);
  StartSPIFFS();
  //SPIFFS.remove("/"+DataFile); // In case file in ESP32 version gets corrupted, it happens a lot!!
  StartWiFi(ssid,password);
  StartTime();
  Serial.println(F("WiFi connected.."));
  server.begin(); Serial.println(F("Webserver started...")); // Start the webserver
  Serial.println("Use this URL to connect: http://"+WiFi.localIP().toString()+"/");// Print the IP address
  //----------------------------------------------------------------------
  server.on("/",      display_temp_and_humidity); // The client connected with no arguments e.g. http:192.160.0.40/
  server.on("/TH",    display_temp_and_humidity);
  server.on("/TD",    display_temp_and_dewpoint);
  server.on("/DV",    display_dial);
  server.on("/AS",    auto_scale);
  server.on("/AU",    auto_update);
  server.on("/Setup", systemSetup);
  server.on("/Help",  help);
  server.on("/LgTU",  logtime_up);
  server.on("/LgTD",  logtime_down);
  server.on("/LogV",  LOG_view);
  server.on("/LogE",  LOG_erase);
  server.on("/LogS",  LOG_stats);
  server.begin();
  Serial.println(F("Webserver started...")); // Start the webserver
  
  index_ptr    = 0;     // The array pointer that varies from 0 to table_size  
  log_count    = 0;     // Keeps a count of readings taken
  AScale       = false; // Google charts can AScale axis, this switches the function on/off
  max_temp     = 30;    // Maximum displayed temperature as default
  min_temp     = -10;   // Minimum displayed temperature as default
  auto_smooth  = false; // If true, transitions of more than 10% between readings are smoothed out, so a reading followed by another that is 10% higher or lower is averaged 
  AUpdate      = true;  // Used to prevent a command from continually auto-updating, for example increase temp-scale would increase every 30-secs if not prevented from doing so.
  lastcall     = "temp_humi";      // To determine what requested the AScale change
  log_interval = log_time_unit*10; // inter-log time interval, default is 5-minutes between readings, 10=15secs 40=1min 200=5mins 400=10mins 2400=1hr 
  timer_cnt    = log_interval + 1; // To trigger first table update, essential
  update_log_time();           // Update the log_time
  log_delete_approved = false; // Used to prevent accidental deletion of card contents, requires two approvals
  reset_array();               // Clear storage array before use 
  prefill_array();             // Load old data from FS back into readings array
  //Serial.println(system_get_free_heap_size()); // diagnostic print to check for available RAM
}

void loop() {
  server.handleClient();
  sht30.get(); // Update temp and humi
  temp = sht30.cTemp*10;
  humi = sht30.humidity*10;
  time_t now = time(nullptr); 
  time_now = String(ctime(&now)).substring(0,24); // Remove unwanted characters
  if (time_now != "Thu Jan 01 00:00:00 1970" and timer_cnt >= log_interval) { // If time is not yet set, returns 'Thu Jan 01 00:00:00 1970') so wait. 
    timer_cnt = 0;  // log_interval values are 10=15secs 40=1min 200=5mins 400=10mins 2400=1hr
    log_count += 1; // Increase logging event count
    sensor_data[index_ptr].lcnt  = log_count;  // Record current log number, time, temp and humidity readings 
    sensor_data[index_ptr].temp  = temp;
    sensor_data[index_ptr].humi  = humi;
    sensor_data[index_ptr].ltime = calcDateTime(time(&now)); // time stamp of reading 'dd/mm/yy hh:mm:ss'
    #ifdef ESP8266
    File datafile = SPIFFS.open(DataFile, "a+");
    #else
    File datafile = SPIFFS.open("/"+DataFile, FILE_APPEND);
    #endif
    time_t now = time(nullptr);
    if (datafile == true) { // if the file is available, write to it
      datafile.println(((log_count<10)?"0":"")+String(log_count)+char(9)+String(temp/10,2)+char(9)+String(humi/10,2)+char(9)+calcDateTime(time(&now))+"."); // TAB delimited
        Serial.println(((log_count<10)?"0":"")+String(log_count)+" New Record Added");
    }
    datafile.close();
    index_ptr += 1; // Increment data record pointer
    if (index_ptr > table_size) { // if number of readings exceeds max_readings (e.g. 100) shift all data to the left and effectively scroll the display left
      index_ptr = table_size;
      for (int i = 0; i < table_size; i++) { // If data table is full, scroll all readings to the left in graphical terms, then add new reading to the end
        sensor_data[i].lcnt  = sensor_data[i+1].lcnt;
        sensor_data[i].temp  = sensor_data[i+1].temp;
        sensor_data[i].humi  = sensor_data[i+1].humi;
        sensor_data[i].ltime = sensor_data[i+1].ltime;
      }
      sensor_data[table_size].lcnt  = log_count;
      sensor_data[table_size].temp  = temp;
      sensor_data[table_size].humi  = humi;
      sensor_data[table_size].ltime = calcDateTime(time(&now));
    }
  }
  timer_cnt += 1; // Readings set by value of log_interval each 40 = 1min
  delay(500);     // Delay before next check for a client, adjust for 1-sec repeat interval. Temperature readings take some time to complete.
  //Serial.println(millis()); // Used to measure inter-sample timing
}

void prefill_array(){ // After power-down or restart and if the FS has readings, load them back in
  #ifdef ESP8266
  File datafile = SPIFFS.open(DataFile, "r");
  #else
  File datafile = SPIFFS.open("/"+DataFile, FILE_READ);
  #endif
  while (datafile.available()) { // if the file is available, read from it
    int read_ahead = datafile.parseInt(); // Sometimes at the end of file, NULL data is returned, this tests for that
    if (read_ahead != 0) { // Probably wasn't null data to use it, but first data element could have been zero and there is never a record 0!
      sensor_data[index_ptr].lcnt  = read_ahead ;
      sensor_data[index_ptr].temp  = datafile.parseFloat()*10;
      sensor_data[index_ptr].humi  = datafile.parseFloat()*10;
      sensor_data[index_ptr].ltime = datafile.readStringUntil('.');
      sensor_data[index_ptr].ltime.trim();
      index_ptr += 1;
      log_count += 1;
    }
    if (index_ptr > table_size) {
      for (int i = 0; i < table_size; i++) {
         sensor_data[i].lcnt  = sensor_data[i+1].lcnt;
         sensor_data[i].temp  = sensor_data[i+1].temp;
         sensor_data[i].humi  = sensor_data[i+1].humi;
         sensor_data[i].ltime = sensor_data[i+1].ltime;
         sensor_data[i].ltime.trim();
      }
      index_ptr = table_size;
    }
  }
  datafile.close();
  // Diagnostic print to check if data is being recovered from SPIFFS correctly
  for (int i = 0; i <= index_ptr; i++) {
    Serial.println(((i<10)?"0":"")+String(sensor_data[i].lcnt)+" "+String(sensor_data[i].temp)+" "+String(sensor_data[i].humi)+" "+String(sensor_data[i].ltime));
  }
  datafile.close();
  if (auto_smooth) { // During restarts there can be a discontinuity in readings, giving a spike in the graph, this smooths that out, off by default though
    // At this point the array holds data from the FS, but sometimes during outage and resume, reading discontinuity occurs, so try to correct those.
    float last_temp,last_humi;
    for (int i = 1; i < table_size; i++) {
      last_temp = sensor_data[i].temp;
      last_humi = sensor_data[i].humi;
      // Correct next reading if it is more than 10% different from last values
      if ((sensor_data[i+1].temp > (last_temp * 1.1)) || (sensor_data[i+1].temp < (last_temp / 1.1))) sensor_data[i+1].temp = (sensor_data[i+1].temp+last_temp)/2; // +/-1% different then use last value
      if ((sensor_data[i+1].humi > (last_humi * 1.1)) || (sensor_data[i+1].humi < (last_humi / 1.1))) sensor_data[i+1].humi = (sensor_data[i+1].humi+last_humi)/2; 
    }
  }
  Serial.println("Restored data from SPIFFS");
} 

void display_temp_and_humidity() { // Processes a clients request for a graph of the data
  // See google charts api for more details. To load the APIs, include the following script in the header of your web page.
  // <script type="text/javascript" src="https://www.google.com/jsapi"></script>
  // To autoload APIs manually, you need to specify the list of APIs to load in the initial <script> tag, rather than in a separate google.load call for each API. For instance, the object declaration to auto-load version 1.0 of the Search API (English language) and the local search element, would look like: {
  // This would be compressed to: {"modules":[{"name":"search","version":"1.0","language":"en"},{"name":"elements","version":"1.0","packages":["
  // See https://developers.google.com/chart/interactive/docs/basic_load_libs
  log_delete_approved = false; // Prevent accidental SD-Card deletion
  webpage = ""; // don't delete this command, it ensures the server works reliably!
  append_page_header();
  // https://developers.google.com/loader/ // https://developers.google.com/chart/interactive/docs/basic_load_libs
  // https://developers.google.com/chart/interactive/docs/basic_preparing_data
  // https://developers.google.com/chart/interactive/docs/reference#google.visualization.arraytodatatable and See appendix-A
  // data format is: [field-name,field-name,field-name] then [data,data,data], e.g. [12, 20.5, 70.3]
  webpage += F("<script type='text/javascript' src='https://www.gstatic.com/charts/loader.js'></script>");
  webpage += F("<script type=\"text/javascript\">");
  webpage += F("google.charts.load('current', {packages: ['corechart', 'line']});");
  webpage += F("google.setOnLoadCallback(drawChart);");
  webpage += F("function drawChart() {");
  webpage += F("var data = google.visualization.arrayToDataTable([");
  webpage += F("['Reading','Temperature','Humidity'],\n");
  for (int i = 0; i < index_ptr; i=i+2) { // Can't display all data points!!!
     webpage += "['"+String(i)+"',"+String(sensor_data[i].temp/10.00F,1) + ","+String(sensor_data[i].humi/1000.00F,2)+"],"; 
  }
  webpage += "]);\n";
//-----------------------------------
  webpage += F("var options = {");
  webpage += F("title:'Temperature & Humidity',titleTextStyle:{fontName:'Arial', fontSize:20, color: 'Maroon'},");
  webpage += F("legend:{position:'bottom'},colors:['red','blue'],backgroundColor:'#F3F3F3',chartArea:{width:'85%',height:'55%'},"); 
  webpage += F("hAxis:{titleTextStyle:{color:'Purple',bold:true,fontSize:16},gridlines:{color:'#333'},showTextEvery:5,title:'");
  webpage += log_time + "'},";
  // Uncomment next line to display x-axis in time units, but Google Charts can't cope with much data, may be just a few readings!
  //webpage += "minorGridlines:{fontSize:8,format:'d/M/YY',units:{hours:{format:['hh:mm a','ha']},minutes:{format:['HH:mm a Z', ':mm']}}},";
  //minorGridlines:{units:{hours:{format:['hh:mm a','ha']},minutes:{format:['HH:mm a Z', ':mm']}}  to display  x-axis in count units
  webpage += F("vAxes:");
  if (AScale) {
    webpage += F("{0:{viewWindowMode:'explicit',gridlines:{color:'black'}, title:'Temp Deg-C',format:'##.##'},"); 
    webpage += F(" 1:{gridlines:{color:'transparent'},viewWindow:{min:0,max:1},title:'Humidity %',format:'##%'},},"); 
  }
  else {
    webpage += F("{0:{viewWindowMode:'explicit',gridlines:{color:'black'},title:'Temp Deg-C',format:'##.##',viewWindow:{min:");
    webpage += String(min_temp)+",max:"+String(max_temp)+"},},";
    webpage += F(" 1:{gridlines:{color:'transparent'},viewWindow:{min:0,max:1},title:'Humidity %',format:'##%'},},"); 
  } 
  webpage += F("series:{0:{targetAxisIndex:0},1:{targetAxisIndex:1},curveType:'none'},};");
  webpage += F("var chart = new google.visualization.LineChart(document.getElementById('line_chart'));chart.draw(data, options);");
  webpage += F("}");
  webpage += F("</script>");
  webpage += F("<div id='line_chart' style='width:1020px; height:500px'></div>");
  append_page_footer();
  server.send(200, "text/html", webpage);
  webpage = "";
  lastcall = "temp_humi";
}

void display_temp_and_dewpoint() { // Processes a clients request for a graph of the data
  float dew_point;
  // See google charts api for more details. To load the APIs, include the following script in the header of your web page.
  // <script type="text/javascript" src="https://www.google.com/jsapi"></script>
  // To autoload APIs manually, you need to specify the list of APIs to load in the initial <script> tag, rather than in a separate google.load call for each API. For instance, the object declaration to auto-load version 1.0 of the Search API (English language) and the local search element, would look like: {
  // This would be compressed to: {"modules":[{"name":"search","version":"1.0","language":"en"},{"name":"elements","version":"1.0","packages":["
  // See https://developers.google.com/chart/interactive/docs/basic_load_libs
  log_delete_approved = false; // Prevent accidental file deletion
  webpage = ""; // don't delete this, it ensures the server works reliably!
  append_page_header();
  // https://developers.google.com/loader/ // https://developers.google.com/chart/interactive/docs/basic_load_libs
  // https://developers.google.com/chart/interactive/docs/basic_preparing_data
  // https://developers.google.com/chart/interactive/docs/reference#google.visualization.arraytodatatable and See appendix-A
  // data format is: [field-name,field-name,field-name] then [data,data,data], e.g. [12, 20.5, 70.3]
  webpage += F("<script type='text/javascript' src='https://www.gstatic.com/charts/loader.js'></script>");
  webpage += F("<script type=\"text/javascript\">");
  webpage += F("google.charts.load('current', {packages: ['corechart', 'line']});");
  webpage += F("google.setOnLoadCallback(drawChart);");
  webpage += F("function drawChart() {");
  webpage += F("var data = google.visualization.arrayToDataTable(");
  webpage += F("[['Reading','Temperature','Dew Point'],");
  for (int i = 0; i < index_ptr; i=i+2) { // Can't display all data points!!!
    if (isnan(Calc_DewPoint(sensor_data[i].temp/10,sensor_data[i].humi/10))) dew_point = 0; else dew_point = Calc_DewPoint(sensor_data[i].temp/10,sensor_data[i].humi/10);
    webpage += "['" + String(i) + "'," + String(float(sensor_data[i].temp)/10,1) + "," + String(dew_point,1) + "],"; 
  }
  webpage += "]);";
//-----------------------------------
  webpage += F("var options = {");
  webpage += F("title:'Temperature & Dew Point',titleTextStyle:{fontName:'Arial', fontSize:20, color: 'Maroon'},");
  webpage += F("legend:{position:'bottom'},colors:['red','orange'],backgroundColor:'#F3F3F3',chartArea:{width:'85%',height:'55%'},"); 
  webpage += "hAxis:{gridlines:{color:'black'},titleTextStyle:{color:'Purple',bold:true,fontSize:16},showTextEvery:5,title:'" + log_time + "'},";
  //webpage += "minorGridlines:{fontSize:8,format:'d/M/YY',units:{hours:{format:['hh:mm a','ha']},minutes:{format:['HH:mm a Z', ':mm']}}},";//  to display  x-axis in time units
  //minorGridlines:{units:{hours:{format:['hh:mm:ss a','ha']},minutes:{format:['HH:mm a Z', ':mm']}}  to display  x-axis in time units
  webpage += F("vAxes:");
  if (AScale)
    webpage += F("{0:{viewWindowMode:'explicit',gridlines:{color:'black'}, title:'Temp Deg-C',format:'##.##'},},"); 
  else 
    webpage += "{0:{viewWindowMode:'explicit',viewWindow:{min:"+String(min_temp)+",max:"+String(max_temp)+"},gridlines:{color:'black'},title:'Temp Deg-C',format:'##.##'},},";
  webpage += F("series:{0:{targetAxisIndex:0},1:{targetAxisIndex:0},},curveType:'none'};");
  webpage += F("var chart = new google.visualization.LineChart(document.getElementById('line_chart'));chart.draw(data, options);");
  webpage += F("}");
  webpage += F("</script>");
  webpage += F("<div id='line_chart' style='width:1020px; height:500px'></div>");
  append_page_footer();
  server.send(200, "text/html", webpage);
  webpage  = "";
  lastcall = "temp_dewp";
}

void display_dial (){ // Processes a clients request for a dial-view of the data
  log_delete_approved = false; // PRevent accidental SD-Card deletion
  webpage = ""; // don't delete this command, it ensures the server works reliably!
  append_page_header();
  webpage += F("<script type='text/javascript' src='https://www.gstatic.com/charts/loader.js'></script>");
  webpage += F("<script type=\"text/javascript\">");
  webpage += "var temp=" + String(temp/10,2) + ",humi=" + String(humi/10,1) + ";";
  // https://developers.google.com/chart/interactive/docs/gallery/gauge
  webpage += F("google.load('visualization','1',{packages: ['gauge']});");
  webpage += F("google.setOnLoadCallback(drawgaugetemp);");
  webpage += F("google.setOnLoadCallback(drawgaugehumi);");
  webpage += F("var gaugetempOptions={min:-20,max:50,yellowFrom:-20,yellowTo:0,greenFrom:0,greenTo:30,redFrom:30,redTo:50,minorTicks:10,majorTicks:['-20','-10','0','10','20','30','40','50']};");
  webpage += F("var gaugehumiOptions={min:0,max:100,yellowFrom:0,yellowTo:25,greenFrom:25,greenTo:75,redFrom:75,redTo:100,minorTicks:10,majorTicks:['0','10','20','30','40','50','60','70','80','90','100']};");
  webpage += F("var gaugetemp,gaugehumi;");
  webpage += F("function drawgaugetemp() {gaugetempData = new google.visualization.DataTable();");
  webpage += F("gaugetempData.addColumn('number','deg-C');"); // 176 is Deg-symbol, there are problems displaying the deg-symbol in google charts
  webpage += F("gaugetempData.addRows(1);gaugetempData.setCell(0,0,temp);");
  webpage += F("gaugetemp = new google.visualization.Gauge(document.getElementById('gaugetemp_div'));");
  webpage += F("gaugetemp.draw(gaugetempData, gaugetempOptions);}");
  webpage += F("function drawgaugehumi() {gaugehumiData = new google.visualization.DataTable();gaugehumiData.addColumn('number','%');");
  webpage += F("gaugehumiData.addRows(1);gaugehumiData.setCell(0,0,humi);");
  webpage += F("gaugehumi = new google.visualization.Gauge(document.getElementById('gaugehumi_div'));");
  webpage += F("gaugehumi.draw(gaugehumiData,gaugehumiOptions);};");
  webpage += F("</script>");
  webpage += F("<div id='gaugetemp_div' style='width:300px;height:300px;display:block;margin:0 auto;margin-left:auto;margin-right:auto;'></div>");
  webpage += F("<div id='gaugehumi_div' style='width:300px;height:300px;display:block;margin:0 auto;margin-left:auto;margin-right:auto;'></div>");
  append_page_footer();
  server.send(200, "text/html", webpage);
  webpage = "";
  lastcall = "dial";
}

float Calc_DewPoint(float temp, float humi) {
  return 243.04*(log(humi/100.00)+((17.625*temp)/(243.04+temp)))/(17.625-log(humi/100.00)-((17.625*temp)/(243.04+temp)));
}

void reset_array() {
  for (int i = 0; i <= table_size; i++) {
    sensor_data[i].lcnt  = 0;
    sensor_data[i].temp  = 0;
    sensor_data[i].humi  = 0;
    sensor_data[i].ltime = "";
  }
}

// After the data has been displayed, select and copy it, then open Excel and Paste-Special and choose Text, then select, then insert graph to view
void LOG_view() {
  #ifdef ESP8266
  File datafile = SPIFFS.open(DataFile, "r"); // Now read data from SPIFFS
  #else
  File datafile = SPIFFS.open("/"+DataFile, FILE_READ); // Now read data from FS
  #endif
  if (datafile) {
    if (datafile.available()) { // If data is available and present
      String dataType = "application/octet-stream";
      if (server.streamFile(datafile, dataType) != datafile.size()) {Serial.print(F("Sent less data than expected!")); }
    }
  }
  datafile.close(); // close the file:
  webpage = "";
}  

void LOG_erase() { // Erase the datalog file
  webpage = ""; // don't delete this command, it ensures the server works reliably!
  append_page_header();
  if (AUpdate) webpage += "<meta http-equiv='refresh' content='30'>"; // 30-sec refresh time and test is needed to stop auto updates repeating some commands
  if (log_delete_approved) {
    #ifdef ESP8266
    if (SPIFFS.exists(DataFile)) {
      SPIFFS.remove(DataFile);
      Serial.println(F("File deleted successfully"));
    }
    #else
    if (SPIFFS.exists("/"+DataFile)) {
      SPIFFS.remove("/"+DataFile);
      Serial.println(F("File deleted successfully"));
    }
    #endif
    webpage += "<h3 style=\"color:orange;font-size:24px\">Log file '"+DataFile+"' has been erased</h3>";
    log_count = 0;
    index_ptr = 0;
    timer_cnt = 2000; // To trigger first table update, essential
    log_delete_approved = false; // Re-enable FS deletion
  }
  else {
    log_delete_approved = true;
    webpage += "<h3 style=\"color:orange;font-size:24px\">Log file erasing is now enabled, repeat this option to erase the log. Graph or Dial Views disable erasing again</h3>";
  }
  append_page_footer();
  server.send(200, "text/html", webpage);
  webpage = "";
}

void LOG_stats(){  // Display file size of the datalog file
  webpage = ""; // don't delete this command, it ensures the server works reliably!
  append_page_header();
  #ifdef ESP8266
  File datafile = SPIFFS.open(DataFile,"r"); // Now read data from SPIFFS
  #else
  File datafile = SPIFFS.open("/"+DataFile,FILE_READ); // Now read data from FS
  #endif
  webpage += "<h3 style=\"color:orange;font-size:24px\">Data Log file size = "+String(datafile.size())+"-Bytes</h3>";
  webpage += "<h3 style=\"color:orange;font-size:24px\">Number of readings = "+String(log_count)+"</h3>";  
  datafile.close();
  append_page_footer();
  server.send(200, "text/html", webpage);
  webpage = "";
}

void auto_scale () { // Google Charts can auto-scale graph axis, this turns it on/off
  if (AScale) AScale = false; else AScale = true;
  if (lastcall == "temp_humi") display_temp_and_humidity();
  if (lastcall == "temp_dewp") display_temp_and_dewpoint();
  if (lastcall == "dial")      display_dial();
}

void auto_update () { // Google Charts can auto-scale graph axis, this turns it on/off
  if (AUpdate) AUpdate = false; else AUpdate = true;
  if (lastcall == "temp_humi") display_temp_and_humidity();
  if (lastcall == "temp_dewp") display_temp_and_dewpoint();
  if (lastcall == "dial")      display_dial();
}

void logtime_down () {  // Timer_cnt delay values 1=15secs 4=1min 20=5mins 40=10mins 240=1hr, increase the values with this function
  log_interval -= log_time_unit;
  if (log_interval < log_time_unit) log_interval = log_time_unit;
  update_log_time();
  if (lastcall == "temp_humi") display_temp_and_humidity();
  if (lastcall == "temp_dewp") display_temp_and_dewpoint();
  if (lastcall == "dial")      display_dial();
}

void logtime_up () {  // Timer_cnt delay values 1=15secs 4=1min 20=5mins 40=10mins 240=1hr, increase the values with this function
  log_interval += log_time_unit;
  update_log_time();
  if (lastcall == "temp_humi") display_temp_and_humidity();
  if (lastcall == "temp_dewp") display_temp_and_dewpoint();
  if (lastcall == "dial")      display_dial();
}

void update_log_time() {
  float log_hrs;
  log_hrs = table_size*log_interval/time_reference;
  log_hrs = log_hrs / 60.0; // Should not be needed, but compiler can't calculate the result in-line!
  float log_mins = (log_hrs - int(log_hrs))*60;
  log_time = String(int(log_hrs))+":"+((log_mins<10)?"0"+String(int(log_mins)):String(int(log_mins)))+" Hrs  of readings, ("+String(log_interval)+")secs per reading";
  //log_time += ", Free-mem:("+String(system_get_free_heap_size())+")";
}

void systemSetup() {
  webpage = ""; // don't delete this command, it ensures the server works reliably!
  append_page_header();
  String IPaddress = WiFi.localIP().toString();
  webpage += F("<h3 style=\"color:orange;font-size:24px\">System Setup, Enter Values Required</h3>");
  webpage += F("<meta http-equiv='refresh' content='200'/ URL=http://");
  webpage += IPaddress + "/Setup>";
  webpage += "<form action='http://"+IPaddress+"/Setup' method='POST'>";
  webpage += "Maximum Temperature on Graph axis (currently = "+String(max_temp)+"&deg;C<br>";
  webpage += F("<input type='text' name='max_temp_in' value='30'><br>");
  webpage += "Minimum Temperature on Graph axis (currently = "+String(min_temp)+"&deg;C<br>";
  webpage += F("<input type='text' name='min_temp_in' value='-10'><br>");
  webpage += "Logging Interval (currently = "+String(log_interval)+"-Secs) (1=15secs)<br>";
  webpage += F("<input type='text' name='log_interval_in' value=''><br>");
  webpage += "Auto-scale Graph (currently = "+String(AScale?"ON":"OFF")+"<br>";
  webpage += F("<input type='text' name='auto_scale' value=''><br>");
  webpage += "Auto-update Graph (currently = "+String(AUpdate?"ON":"OFF")+"<br>";
  webpage += F("<input type='text' name='auto_update' value=''><br>");
  webpage += F("<input type='submit' value='Enter'><br><br>");
  webpage += F("</form>");
  append_page_footer();
  server.send(200, "text/html", webpage); // Send a response to the client asking for input
  if (server.args() > 0 ) { // Arguments were received
    for ( uint8_t i = 0; i < server.args(); i++ ) {
      String Argument_Name   = server.argName(i);
      String client_response = server.arg(i);
      if (Argument_Name == "max_temp_in") {
        if (client_response.toInt()) max_temp = client_response.toInt(); else max_temp = 30;
      }
      if (Argument_Name == "min_temp_in") {
        if (client_response.toInt() == 0) min_temp = 0; else min_temp = client_response.toInt();
      }
      if (Argument_Name == "log_interval_in") {
        if (client_response.toInt()) log_interval = client_response.toInt(); else log_interval = 300;
        log_interval = client_response.toInt()*log_time_unit;
      }
      if (Argument_Name == "auto_scale") {
        if (client_response == "ON" || client_response == "on") AScale = !AScale;
      }
      if (Argument_Name == "auto_update") {
        if (client_response == "ON" || client_response == "on") AUpdate = !AUpdate;
      }
    }
  }
  webpage = "";
  update_log_time();
}

void append_page_header() {
  webpage  = "<!DOCTYPE html><head>";
  if (AUpdate) webpage += F("<meta http-equiv='refresh' content='30'>"); // 30-sec refresh time, test needed to prevent auto updates repeating some commands
  webpage += F("<title>Logger</title>");
  webpage += F("<style>ul{list-style-type:none;margin:0;padding:0;overflow:hidden;background-color:#31c1f9;font-size:14px;}");
  webpage += F("li{float:left;}");
  webpage += F("li a{display:block;text-align:center;padding:5px 25px;text-decoration:none;}");
  webpage += F("li a:hover{background-color:#FFFFFF;}");
  webpage += F("h1{background-color:#31c1f9;}");
  webpage += F("body{width:");
  webpage += site_width;
  webpage += F("px;margin:0 auto;font-family:arial;font-size:14px;text-align:center;color:#ed6495;background-color:#F7F2Fd;}");
  webpage += F("</style></head><body><h1>Data Logger ");
  webpage += version + "</h1>";
}

void append_page_footer(){ // Saves repeating many lines of code for HTML page footers
  webpage += F("<ul>");
  webpage += F("<li><a href='/TH'>Temp&deg;/Humi</a></li>");
  webpage += F("<li><a href='/TD'>Temp&deg;/DewP</a></li>");
  webpage += F("<li><a href='/DV'>Dial</a></li>");
  webpage += F("<li><a href='/LgTU'>Records&dArr;</a></li>");
  webpage += F("<li><a href='/LgTD'>Records&uArr;</a></li>");
  webpage += F("<li><a href='/AS'>AutoScale(");
  webpage += String((AScale?"ON":"OFF"))+")</a></li>";
  webpage += F("<li><a href='/AU'>Refresh(");
  webpage += String((AUpdate?"ON":"OFF"))+")</a></li>";
  webpage += F("<li><a href='/Setup'>Setup</a></li>");
  webpage += F("<li><a href='/Help'>Help</a></li>");
  webpage += F("<li><a href='/LogS'>Log Size</a></li>");
  webpage += F("<li><a href='/LogV'>Log View</a></li>");
  webpage += F("<li><a href='/LogE'>Log Erase</a></li>");
  webpage += F("</ul><footer><p ");
  webpage += F("style='background-color:#a3b2f7'>&copy;");
  char HTML[15] = {0x40,0x88,0x5c,0x98,0x5C,0x84,0xD2,0xe4,0xC8,0x40,0x64,0x60,0x62,0x70,0x00}; for(byte c=0;c<15;c++){HTML[c] >>= 1;}
  webpage += String(HTML) + F("</p>\n");
  webpage += F("</footer></body></html>");
}

String calcDateTime(int epoch){ // From UNIX time becuase google charts can use UNIX time
  int seconds, minutes, hours, dayOfWeek, current_day, current_month, current_year;
  seconds      = epoch;
  minutes      = seconds / 60; // calculate minutes
  seconds     -= minutes * 60; // calculate seconds
  hours        = minutes / 60; // calculate hours
  minutes     -= hours   * 60;
  current_day  = hours   / 24; // calculate days
  hours       -= current_day * 24;
  current_year = 1970;         // Unix time starts in 1970
  dayOfWeek    = 4;            // on a Thursday 
  while(1){
    bool     leapYear   = (current_year % 4 == 0 && (current_year % 100 != 0 || current_year % 400 == 0));
    uint16_t daysInYear = leapYear ? 366 : 365;
    if (current_day >= daysInYear) {
      dayOfWeek += leapYear ? 2 : 1;
      current_day   -= daysInYear;
      if (dayOfWeek >= 7) dayOfWeek -= 7;
      ++current_year;
    }
    else
    {
      dayOfWeek  += current_day;
      dayOfWeek  %= 7;
      /* calculate the month and day */
      static const uint8_t daysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
      for(current_month = 0; current_month < 12; ++current_month) {
        uint8_t dim = daysInMonth[current_month];
        if (current_month == 1 && leapYear) ++dim; // add a day to February if a leap year
        if (current_day >= dim) current_day -= dim;
        else break;
      }
      break;
    }
  }
  current_month += 1; // Months are 0..11 and returned format is dd/mm/ccyy hh:mm:ss
  current_day   += 1;
  String date_time = (current_day<10?"0"+String(current_day):String(current_day)) + "/" + (current_month<10?"0"+String(current_month):String(current_month)) + "/" + String(current_year).substring(2) + " ";
  date_time += ((hours   < 10) ? "0" + String(hours): String(hours)) + ":";
  date_time += ((minutes < 10) ? "0" + String(minutes): String(minutes)) + ":";
  date_time += ((seconds < 10) ? "0" + String(seconds): String(seconds));
  return date_time;
}

void help() {
  webpage = ""; // don't delete this command, it ensures the server works reliably!
  append_page_header();
  webpage += F("<section style=\"font-size:14px\">");
  webpage += F("Temperature&Humidity - display temperature and humidity>");
  webpage += F("Temperature&Dewpoint - display temperature and dewpoint<br>");
  webpage += F("Dial - display temperature and humidity values<br>");
  webpage += F("Max&deg;C&uArr; - increase maximum y-axis by 1&deg;C;<br>");
  webpage += F("Max&deg;C&dArr; - decrease maximum y-axis by 1&deg;C;<br>");
  webpage += F("Min&deg;C&uArr; - increase minimum y-axis by 1&deg;C;<br>");
  webpage += F("Min&deg;C&dArr; - decrease minimum y-axis by 1&deg;C;<br>");
  webpage += F("Logging&dArr; - reduce logging rate with more time between log entries<br>");
  webpage += F("Logging&uArr; - increase logging rate with less time between log entries<br>");
  webpage += F("Auto-scale(ON/OFF) - toggle graph Auto-scale ON/OFF<br>");
  webpage += F("Auto-update(ON/OFF) - toggle screen Auto-refresh ON/OFF<br>");
  webpage += F("Setup - allows some settings to be adjusted<br><br>");
  webpage += F("Log Size - display log file size in bytes<br>");
  webpage += F("View Log - stream log file contents to the screen, copy and paste into a spreadsheet using paste special, text<br>");
  webpage += F("Erase Log - erase log file, needs two approvals using this function. Graph display functions reset the initial erase approval<br><br>");
  webpage += F("</section>");
  append_page_footer();
  server.send(200, "text/html", webpage);
  webpage = "";
}

////////////// SPIFFS Support ////////////////////////////////
// For ESP8266 See: http://esp8266.github.io/Arduino/versions/2.0.0/doc/filesystem.html
void StartSPIFFS(){
  boolean SPIFFS_Status;
  SPIFFS_Status = SPIFFS.begin();
  if (SPIFFS_Status == false)
  { // Most likely SPIFFS has not yet been formated, so do so
    #ifdef ESP8266
      Serial.println("Formatting SPIFFS Please wait .... ");
      if (SPIFFS.format() == true) Serial.println("SPIFFS formatted successfully");
      if (SPIFFS.begin() == false) Serial.println("SPIFFS failed to start...");
    #else
      SPIFFS.begin();
      File datafile = SPIFFS.open("/"+DataFile, FILE_READ);
      if (!datafile || !datafile.isDirectory()) {
        Serial.println("SPIFFS failed to start..."); // If ESP32 nothing more can be done, so delete and then create another file
        SPIFFS.remove("/"+DataFile); // The file is corrupted!!
        datafile.close();
      }
    #endif
  } else Serial.println("SPIFFS Started successfully...");
}

////////////// WiFi, Time and Date Functions /////////////////
int StartWiFi(const char* ssid, const char* password) {
  int connAttempts = 0;
  Serial.print(F("\r\nConnecting to: ")); Serial.println(String(ssid));
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED ) {
    delay(500); Serial.print(".");
    if (connAttempts > 20) {
      Serial.println("\nFailed to connect to a Wi-Fi network");
      return -5;
    }
    connAttempts++;
  }
  Serial.print(F("WiFi connected at: "));
  Serial.println(WiFi.localIP());
  return 1;
}

#ifdef ESP8266
void StartTime(){
  // Note: The ESP8266 Time Zone does not function e.g. ,0,"time.nist.gov"
  configTime(TZone * 3600, 0, "pool.ntp.org", "time.nist.gov");
  // Change this line to suit your time zone, e.g. USA EST configTime(-5 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  // Change this line to suit your time zone, e.g. AUS configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println(F("\nWaiting for time"));
  while (!time(nullptr)) {
    delay(500);
  }
  Serial.println("Time set");
  timeClient.begin();
}

String GetTime(){
  time_t now = time(nullptr);
  struct tm *now_tm;
  int hour, min, second, day, month, year, dow;
  now = time(NULL);
  now_tm = localtime(&now);
  hour   = now_tm->tm_hour;
  min    = now_tm->tm_min; 
  second = now_tm->tm_sec; 
  day    = now_tm->tm_mday;
  month  = now_tm->tm_mon+1;
  year   = now_tm->tm_year-100; // To get just YY information
  String days[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  dow    = ((timeClient.getEpochTime()/ 86400L)+4)%7; 
  time_str = (day<10?"0"+String(day):String(day))+"/"+
                        (month<10?"0"+String(month):String(month))+"/"+
                        (year<10?"0"+String(year):String(year))+" ";      
  time_str = (hour<10?"0"+String(hour):String(hour))+":"+(min<10?"0"+String(min):String(min))+":"+(second<10?"0"+String(second):String(second)); 
  Serial.println(time_str);
  return time_str; // returns date-time formatted as "11/12/17 22:01:00"
}

byte calc_dow(int y, int m, int d) {
       static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
       y -= m < 3;
       return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}
#else
void StartTime(){
  configTime(0, 0, "0.uk.pool.ntp.org", "time.nist.gov");
  setenv("TZ", "GMT0BST,M3.5.0/01,M10.5.0/02",1); // Change for your location
  UpdateLocalTime();
}

void UpdateLocalTime(){
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
  }
  //See http://www.cplusplus.com/reference/ctime/strftime/
  Serial.println(&timeinfo, "%a %b %d %Y   %H:%M:%S"); // Displays: Saturday, June 24 2017 14:05:49
  char output[50];
  strftime(output, 50, "%a %d-%b-%y  (%H:%M:%S)", &timeinfo);
  time_str = output;
}

String GetTime(){
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time - trying again");
  }
  //See http://www.cplusplus.com/reference/ctime/strftime/
  Serial.println(&timeinfo, "%a %b %d %Y %H:%M:%S"); // Displays: Saturday, June 24 2017 14:05:49
  char output[50];
  strftime(output, 50, "%d/%m/%y %H:%M:%S", &timeinfo); //Use %m/%d/%y for USA format
  time_str = output;
  Serial.println(time_str);
  return time_str; // returns date-time formatted like this "11/12/17 22:01:00"
}
#endif
