/**
    * @authors Ole Sveinung Berget, Are Berntsen, David Holt, Storm Selvig, Eivind Olsøy Solberg
*/
#include "mbed.h"
#include "HTS221Sensor.h"
#include "DFRobot_RGBLCD.h"
#include "math.h"
#include "rtos.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include "stm32l4xx_hal.h"
#include "TLSSocket.h"
#include "mbed_wait_api.h"
#include "nsapi_types.h"
#include "WiFiInterface.h"
#include "json.hpp"
#include "weather.h"
#include "ipgeolocation_io_ca_root_certificate.h"
#include "http_request.h"
#include "http_response.h"
#include "ctime"

using json = nlohmann::json;
double unix_epoch_time = 0;
double latitude = 0;
double longitude = 0;
std::string city;

std::string weather;
double temperature = 0;

bool inTitleTag = false;
bool inItemTag = false;
bool buttonP = false;
string buffer;
vector<string> headlines;

struct AlarmTime {
    int hour;
    int minute;
};
struct Note {
    float frequency;
    int duration;
};
struct Note tetrisMelody[] = {
    {659.26, 250},
    {494.13, 125},
    {523.25, 125},
    {587.33, 250},
    {523.25, 125},
    {494.13, 125},
    {440.00, 250},
    {440.00, 125},
    {523.25, 125},
    {659.26, 250},
    {587.33, 125},
    {523.25, 125},
    {493.88, 250},
    {493.88, 125},
    {523.25, 125},
    {587.33, 250},
    {659.26, 250},
    {523.25, 250},
    {440.00, 125},
    {1, 125},
    {440.00, 375},
};

bool alarmOn;
bool stopAlarm = false;
int melodyLength = sizeof(tetrisMelody) / sizeof(Note);

AlarmTime alarmTime;

volatile int functionState = 0;
WiFiInterface *wifi;

DevI2C i2c(PB_11, PB_10);
HTS221Sensor hts221(&i2c);
DFRobot_RGBLCD lcd(16, 2, PC_1, PC_0);
PwmOut buzzer(PB_4);

InterruptIn button(PA_0, PullUp);
DigitalIn button1(PD_14, PullUp);
DigitalIn button2(PA_1, PullUp);
InterruptIn swOn(PA_3, PullUp);

Thread alarmThread;

/**
    * @author Are Berntsen
    * @function connect_to_wifi()
*/
// Function to connect to wifi
int connect_to_wifi() {

        // Print a message signalling that the connection process is starting
        printf("Connecting to Wi-Fi...\n");

        // Get the default WiFi inteface, if not found print error message and return -1
        wifi = WiFiInterface::get_default_instance();
        if (!wifi) {
                printf("ERROR: No Wi-Fi interface found.\n");
                return -1;
        }

        // Attempt to connect to WiFi with WPA2
        int ret = wifi->connect(MBED_CONF_NSAPI_DEFAULT_WIFI_SSID, MBED_CONF_NSAPI_DEFAULT_WIFI_PASSWORD, NSAPI_SECURITY_WPA_WPA2);
        if (ret != 0) {
                printf("Failed to connect using WPA2, attempting WPA3...\n"); // If connecting with WPA2 fails, try with WPA3
                ret = wifi->connect(MBED_CONF_NSAPI_DEFAULT_WIFI_SSID, MBED_CONF_NSAPI_DEFAULT_WIFI_PASSWORD, NSAPI_SECURITY_WPA3);
        }

        // If connection still fails, print an error message and return -1
        if (ret != 0) {
                printf("ERROR: %d, could not connect to Wi-Fi.\n", ret);
                return -1;
        }

        // If the connection is successfull, get the IP-address
        SocketAddress ip_address;
        wifi->get_ip_address(&ip_address);

        // Print a message for successful connection and the device's IP address
        printf("Connected to Wi-Fi: %s\n", ip_address.get_ip_address());
        return 0;
}



 /**
    * @authors , David Holt, Ole Sveinung Berget, Storm Selvig
    * @function geo_Loc
*/
void soundAlarm() {
    while (stopAlarm == false) {
        for (int i = 0; i < melodyLength; i++) {
            Note &note = tetrisMelody[i];
            buzzer.period(1.0 / note.frequency);
            buzzer.write(0.5f);
            ThisThread::sleep_for(std::chrono::milliseconds(static_cast<int>(note.duration)));
            buzzer.write(0.0f);
            ThisThread::sleep_for(50ms);

            if (button2 == 0) {
                ThisThread::sleep_for(300s);
                i = -1;
            }
            if (button1 == 0) {
                stopAlarm = true;
                i = melodyLength + 1;
            }
        }
        ThisThread::sleep_for(125ms);
    }
    stopAlarm = false;
}




 /**
    * @author Are Berntsen
    * @function extract_json_data
*/
// Function to extract JSON data from the HTTP response
std::string extract_json_data(const std::string& response) {
        // Find the position of the first opening bracket which signifies the start of the JSON data
        size_t start_pos = response.find("{");
        // Check if the start position was found
        if (start_pos != std::string::npos) {
                // If so, extract the JSON data by using substring-function
                return response.substr(start_pos);
        }
        // If no JSON data was found, return an empty string
        return "";
}



 /**
    * @authors Are Berntsen, Ole Sveinung Berget, David Holt
    * @function geo_Loc
*/
// Function to get the geo-location data based on IP
void geo_Loc(AlarmTime* alarmTime){
    lcd.setRGB(50, 100, 100);
    // Create a TLS socket instance
    TLSSocket socket;

// Set up the constants for the API key, API host and API port
    const char* API_KEY = "9a5da66a9f0444efb01a05a61c3fad83";
    const char* API_HOST = "api.ipgeolocation.io";
    const int API_PORT = 443;

// Prepare the API request using snprintf
    char api_request[128];
    snprintf(api_request, sizeof(api_request), "GET /timezone?apiKey=%s HTTP/1.1\r\nHost: %s\r\n\r\n", API_KEY, API_HOST);
// Try to open the socket and check if there is any error
    nsapi_error_t open_result = socket.open(wifi);
    if (open_result != 0) {
        printf("Opening the socket failed... %d\n", open_result);
        return;
    }

// Set the root certificate for SSL/TLS connection and check if there is any error
    printf("Setting the root certificate...\n");
    if (socket.set_root_ca_cert(ipgeolocation_io_ca_root) != 0) {
        printf("Setting the root certificate failed...\n");
        return;
    }

// Get the IP address for the API host and check if there is any error
    SocketAddress api_address;
    nsapi_error_t address_result = wifi->gethostbyname(API_HOST, &api_address);
    if (address_result != NSAPI_ERROR_OK) {
        printf("Resolving the API server hostname failed... %d\n", address_result);
        return;
    }

// Set the port number for the IP address
    api_address.set_port(API_PORT);

    const char host[] = "api.ipgeolocation.io";
    socket.set_hostname(host);

// Try to connect the socket to the API server and check if there is any error
    printf("Connecting to the API server...\n");
    nsapi_error_t connect_result = socket.connect(api_address);
    if (connect_result != NSAPI_ERROR_OK) {
        printf("Failed to connect to the API server... %d, error: %s\n", connect_result, strerror(-connect_result));
        return;
    }

// Send the API request and check if there is any error
    printf("Sending the API request...\n");
    nsapi_error_t send_result = socket.send(api_request, strlen(api_request));
    if (send_result < 0) {
        printf("Sending the API request failed... %d\n", send_result);
        return;
    }

// Receive the API response and parse it
    printf("Receiving and parsing the API response...\n");

    char buffer[2048];
    nsapi_size_or_error_t recv_result = socket.recv(buffer, sizeof(buffer));
    if (recv_result < 0) {
        printf("Receiving the API response failed... %d\n", recv_result);
        return;
    }

// Parse the response to a string
    std::string response(buffer, recv_result);
// Extract the JSON data from the response    
    std::string json_data = extract_json_data(response);

// Check if JSON data is not empty
    if (!json_data.empty()) {

// Parse the JSON data to a JSON object
        json parsed_json = json::parse(json_data);

// Extract unix epoch time from the JSON object
        unix_epoch_time = parsed_json["date_time_unix"];

// Check if daylight saving is currently active
        bool is_dst = parsed_json["is_dst"];
// Extract timezone offset, and account for daylight saving time if active
        double timezone_offset = is_dst ? parsed_json["timezone_offset_with_dst"] : parsed_json["timezone_offset"];

// Adjust the unix epoch time by the timezone offset
        unix_epoch_time += timezone_offset * 3600;

// Extract latitude and longitude from the JSON object and convert to double
        latitude = std::stod(parsed_json["geo"]["latitude"].get<std::string>());
        longitude = std::stod(parsed_json["geo"]["longitude"].get<std::string>());

// Extract the city from the JSON object
        std::string city = parsed_json["geo"]["city"];

// Loop as long as the button isn't pressed and the functionState is 0
        while (!buttonP && functionState == 0) {
            if(functionState == 0){
                // Display Unix epoch time on the LCD
                lcd.clear();    
                lcd.setCursor(0, 0);
                lcd.printf("Unix epoch time: ");
                lcd.setCursor(0, 1);
                lcd.printf("%f", unix_epoch_time);
                ThisThread::sleep_for(2s);
            }
            if(functionState == 0){
                // Display latitude and longitude on the LCD
                lcd.clear();
                lcd.setCursor(0, 0);
                lcd.printf("Lat: %f", latitude);
                lcd.setCursor(0, 1);
                lcd.printf("Lon: %f", longitude);
                ThisThread::sleep_for(2s);
            }
            if(functionState == 0){
                // Display the city on the LCD
                lcd.clear();
                lcd.setCursor(0, 0);
                lcd.printf("City: %s", city.c_str());
                ThisThread::sleep_for(2s);
            }

            // Get the current local time
            time_t now = unix_epoch_time -1 ;
            struct tm *currentTime = localtime(&now);
            // Check if the alarm is set
            if (alarmOn == true) {
                if (alarmTime->hour == currentTime->tm_hour && alarmTime->minute == currentTime->tm_min && currentTime->tm_sec < 6) {
                    alarmThread.start(callback(soundAlarm));
                }
            }
            // Check if the button is pressed
            if (button == 0) {
                buttonP = true;
            }
        }
        // Reset buttonP flag
        buttonP = false;
    }
}



 /**
    * @authors David Holt, Eivind Olsøy Solberg
    * @function currentAlarm()
    */
// Function to display the current alarm on the bottom of the screen
void currentAlarm(AlarmTime* alarmTime) {

    // Fills all 16 spaces to avoid overlap, and makes sure that all numbers under 10 have a 0 in front
    lcd.setCursor(0, 1);
    if (alarmTime->hour < 10) {
        if (alarmTime->minute < 10) {
            lcd.printf("0%d:0%d           ", alarmTime->hour, alarmTime->minute);
        } else {
            lcd.printf("0%d:%d           ", alarmTime->hour, alarmTime->minute);
        }
    } else {
        if (alarmTime->minute < 10) {
            lcd.printf("%d:0%d           ", alarmTime->hour, alarmTime->minute);
        } else {
            lcd.printf("%d:%d           ", alarmTime->hour, alarmTime->minute);              
        }
    }
}



 /**
    * @authors Ole Sveinung Berget, David Holt
    * @function LoopDayMonth
    */
void loopDayMonth(AlarmTime* alarmTime) {
    //Calling on geo_Loc()
    geo_Loc(alarmTime);
    lcd.setCursor(0, 0);
    //setting unix time
    time_t now = unix_epoch_time;
    struct tm *currentTime = localtime(&now);
    char buffer[32];

    // Printing current time
    printf("Time: %d:%d\n%d", currentTime->tm_hour, currentTime->tm_min, currentTime->tm_sec);
    lcd.setRGB(50, 200, 50); 
    if (currentTime->tm_hour < 10) {
        strftime(buffer, sizeof(buffer), "%a %d %b 0%H:%M", currentTime);
    } else {
        strftime(buffer, sizeof(buffer), "%a %d %b %H:%M", currentTime);
    }
    lcd.setCursor(0, 0);
    lcd.printf(buffer);
    lcd.setCursor(0, 1);
    lcd.printf("                ");
    // CHECKING IF ALARM is enabled
    if (alarmOn == true) {
        currentAlarm(alarmTime);
        if (alarmTime->hour == currentTime->tm_hour && alarmTime->minute == currentTime->tm_min && currentTime->tm_sec < 4) {
            alarmThread.start(callback(soundAlarm));
        }
    }
}



/**
    * @function setAlarm();
    * @authors David Holt
*/
// Function to set the alarm time
void setAlarm(AlarmTime* alarmTime) {

    int tempTime = 0;

    currentAlarm(alarmTime);
    ThisThread::sleep_for(20ms);
    lcd.setCursor(0, 0);

    // Tells you if the alarm is currently on or off
    if (alarmOn == true) {
        lcd.printf("ALARM (enabled) ");
    } else if (alarmOn == false){
        lcd.printf("ALARM (disabled)");
    }

    // Start setting an alarm by pressing button2
    if (button2 == 0) {
        lcd.setCursor(0, 1);
        lcd.printf("[00]:00         ");

        // Using button as enter
        while (button != 0) {

            // Input for hours
            if (button2 == 0) {
                tempTime++;
                if (tempTime >= 24) {
                    tempTime = 0;
                }
                lcd.setCursor(0, 1);

                // If the given hour is less than 9, it is one digit
                // Here we add a 0, so that the format becomes 01:00, instead of 1:00
                if (tempTime < 10) {
                    lcd.printf("[0%d]:00         ", tempTime);
                } else {                        
                    lcd.printf("[%d]:00         ", tempTime);
                }
                ThisThread::sleep_for(100ms);
            }
        }
        
        // Saves the given hour as alarmTime->hour
        alarmTime->hour = tempTime;
        ThisThread::sleep_for(100ms);
        tempTime = 0;

        // Keeps displaying the current set time
        lcd.setCursor(0, 1);
        if (alarmTime->hour < 10) {
            lcd.printf("0%d:[00]         ", alarmTime->hour);
        } else {
            lcd.printf("%d:[00]         ", alarmTime->hour);                
        }

        ThisThread::sleep_for(50ms);

        // Using button as enter
        while (button != 0) {

            // Input for minutes
            if (button2 == 0) {
                tempTime = tempTime + 1;
                if (tempTime >= 60 ) {
                    tempTime = 0;
                }
                lcd.setCursor(0, 1);

                // Here we add a 0 in front of both hour and minute, for any combination that includes a number lower than 10
                if (alarmTime->hour < 10) {
                    if (tempTime < 10) {
                        lcd.printf("0%d:[0%d]         ", alarmTime->hour, tempTime);
                    } else {
                        lcd.printf("0%d:[%d]         ", alarmTime->hour, tempTime);
                    }
                } else {
                    if (tempTime < 10) {
                            lcd.printf("%d:[0%d]         ", alarmTime->hour, tempTime);
                    } else {
                        lcd.printf("%d:[%d]         ", alarmTime->hour, tempTime);              
                    }
                }
                ThisThread::sleep_for(100ms);
            }
        }

        // Sets the given minute as current alarmTime->minute
        alarmTime->minute = tempTime;
        lcd.setCursor(0, 0);

        // Confirms the alarmTime on submit
        lcd.printf("ALARM SET TO    ");
        currentAlarm(alarmTime);

        // Displays loopDayMonth
        functionState = 1;
    }
    ThisThread::sleep_for(std::chrono::milliseconds(20));
}



 /**
    * @authors Ole Sveinung Berget
    * @function loopTempHum()
*/
void loopTempHum() {
    lcd.setRGB(10, 150, 50); 
  if (hts221.init(NULL) != 0) {
    printf("Failed to initilize HTS221 device\n");
  }
  if (hts221.enable() != 0){
    printf("Failed to power up HTS221 device\n");
  }

  float temperature;
  float humidity;
  
  hts221.get_temperature(&temperature);
  hts221.get_humidity(&humidity);

  lcd.setCursor(0, 0);
  lcd.printf("Temp: %.2fC    ", temperature);
  lcd.setCursor(0, 1);
  lcd.printf("Hum: %.2f%%    ", humidity);
}



 /**
    * @authors Are Berntsen, Ole Sveinung Berget
    * @function fetch_and_display_weather()
*/
// Function to fetch weather data from the API and display it on the LCD
void fetch_and_display_weather() {

    // Constants for the API
    const char* WEATHER_API_KEY = "b0fe3804e85da7d90a8ebab966cc1695";
    const char* WEATHER_API_HOST = "api.openweathermap.org";
    const int WEATHER_API_PORT = 80;
    
    // Creating a TCP socket
    TCPSocket socket;

    // Formatting request for the API
    char weather_api_request[256];
    snprintf(weather_api_request, sizeof(weather_api_request), 
    "GET /data/2.5/weather?lat=%.2f&lon=%.2f&appid=%s HTTP/1.1\r\nHost: %s\r\n\r\n",
    latitude, longitude, WEATHER_API_KEY, WEATHER_API_HOST);

    // Opening the socket and checking for errors
    nsapi_error_t open_result_weather = socket.open(wifi);
    if (open_result_weather != 0) {
        printf("Opening the weather socket failed... %d\n", open_result_weather);
        return;
    }

    // Resolving host name for the weather API and checking for errors
    SocketAddress weather_api_address;
    nsapi_error_t address_result_weather = wifi->gethostbyname(WEATHER_API_HOST, &weather_api_address);
    if (address_result_weather != NSAPI_ERROR_OK) {
        printf("Resolving the weather API server hostname failed... %d\n", address_result_weather);
        return;
    }

    // Setting the port for the weather API server
    weather_api_address.set_port(WEATHER_API_PORT);
    printf("Connecting to the weather API server...\n");
    nsapi_error_t connect_result_weather = socket.connect(weather_api_address);
    if (connect_result_weather != NSAPI_ERROR_OK) {
        printf("Failed to connect to the weather API server... %d, error: %s\n", connect_result_weather, strerror(-connect_result_weather));
        return;
    }

    // Sending request to the weather API server
    printf("Sending the weather API request...\n");
    nsapi_error_t send_result_weather = socket.send(weather_api_request, strlen(weather_api_request));
    if (send_result_weather < 0) {
        printf("Sending the weather API request failed... %d\n", send_result_weather);
        return;
    }


    // Recieve response and check for errors
    char weather_buffer[2048];
    nsapi_size_or_error_t recv_result_weather = socket.recv(weather_buffer, sizeof(weather_buffer));
    if (recv_result_weather < 0) {
        printf("Receiving the weather API response failed... %d\n", recv_result_weather);
        return;
    }

    // Parse JSON data from the response
    std::string weather_response(weather_buffer, recv_result_weather);
    std::string weather_json_data = extract_json_data(weather_response);


    // Set RGB color
    lcd.setRGB(50, 100, 100);

    // Display data on the LCD
    if (!weather_json_data.empty()) {
        json weather_parsed_json = json::parse(weather_json_data);

        const std::string weather_main = weather_parsed_json["weather"][0]["main"];
        double temperature = weather_parsed_json["main"]["temp"];

        weather = weather_main;
        float temp = temperature - 273.15; // Convert temperature from Kelvin to Celsius
        
    if (functionState == 4) {    
    lcd.setCursor(0, 0);
    lcd.printf("Weather: %s", weather.c_str());
    lcd.setCursor(0, 1);
    lcd.printf("Temp: %.2f C", temp);
    ThisThread::sleep_for(2s);}
    } else {
        printf("Failed to extract JSON data from the weather response.\n");
    }
    socket.close(); // Closing the socket
}



 /**
    * @authors Ole Sveinung Berget, Are Berntsen
    * @function iChooseYou()
*/
void iChooseYou() {
    // Function to let the user input their own geographic coordinates

    // Clear LCD
    lcd.clear();
    // Set LCD backlight to blue
    lcd.setRGB(0, 0, 255);
    // Move cursor to the top
    lcd.setCursor(0, 0);
    lcd.printf("Choose location   ");
    // Move cursor to the beginning
    lcd.setCursor(0, 1);
    lcd.printf("Lat and Long      ");

    // Check if second button is not pressed, if so the user can type coordinates
    if(button2 == 0){
    lcd.setRGB(0, 0, 255);
    lcd.setCursor(0, 0);
    lcd.printf("Enter latitude");
    lcd.setCursor(0, 1);
    lcd.printf("and longitude");

    // Declare variables for the users input for latitude and longitude
    double user_latitude;
    double user_longitude;

    // Print prompts to the console for the user to input and then get the input for the variables
    printf("Enter latitude: ");
    scanf("%lf", &user_latitude);
    printf("Enter longitude: ");
    scanf("%lf", &user_longitude);

     // Check if the user's input for latitude and longitude aren't both zero 
        if (user_latitude != 0.0 && user_longitude != 0.0) {
            // If so, assign these values to the global variables
            latitude = user_latitude;
            longitude = user_longitude;
            // Fetch and display the weather for the user's new coordinates
            fetch_and_display_weather();
            // Change function state to the previous one
            functionState = (functionState - 1);
        } else {
            // Print message if user's coordinates are invalid  
            printf("Invalid coordinates. Weather will not be displayed.\n");
            // Move to the next function state
            functionState = (functionState + 1);
        }
    }

}



/**
    * @author Storm Selvig
*/
//Funksion to extract the relevant data adn filter out junk
void body_callback(const char* data, uint32_t data_len) {
    string chunk(data, data_len);
    size_t pos = 0;
//while loop to get the relevent information and filtering out some junk we don't want to print
    while (pos < chunk.size() && headlines.size() < 3) {
        if (!inItemTag) {
            size_t itemPos = chunk.find("<item>", pos);
            if (itemPos != string::npos) {
                inItemTag = true;
                pos = itemPos + 6;
            } else {
                break;
            }
        } else if (!inTitleTag) {
            size_t titlePos = chunk.find("<title>", pos);
            if (titlePos != string::npos) {
                inTitleTag = true;
                pos = titlePos + 14;
            } else {
                break;
            }
        } else {
            size_t endPos = chunk.find("</title>", pos);
            if (endPos != string::npos) {
                buffer += chunk.substr(pos, endPos - pos);
                
                buffer.erase(std::remove_if(buffer.begin(), buffer.end(), [](unsigned char boks) { 
                    return boks == '[' || boks == ']' || boks == '<' || boks == '>'; 
                }), buffer.end());
                
                headlines.push_back(buffer);
                buffer.clear();
                inTitleTag = false;
                inItemTag = false;
                pos = endPos + 20;
            } else {
                buffer += chunk.substr(pos);
                break;
            }
        }
    }
}



/**
    * @authors Storm Selvig
*/
//Funksion to display the headlines
void displayHeadlines() {

    //Choose the max length of the line showed at a time (Matching the lcd screen)
    int linelenght = 16; 
    lcd.setRGB(240, 17, 143);
    lcd.setCursor(0, 0);  
    lcd.printf("Source: CNN      ");
    //For loop to print 3 news
    for (int i = 0; i < 3; i++) {
        //if the user has jumped out of news while it is running
        if(functionState == 6){
        
        string headline = headlines[i];
        size_t length = headline.length();
        //for loop for printing the headline
        for (size_t j = 0; j <= length; j++) {
             if(functionState == 6){
            lcd.setCursor(0, 1);  
            if (j + linelenght > length) {
                lcd.printf("%-*s             ", linelenght, headline.substr(j).c_str());
            } else {
                lcd.printf("%s", headline.substr(j, linelenght).c_str());
            }
          }
          ThisThread::sleep_for(200ms);
        }
      }
    }   
}



/**
    * @authors Storm Selvig
*/
//funksion to fetch the news and display it
void newsfeed() {
    //Choose where to fetch the news from
    headlines.clear();
    HttpRequest* request = new HttpRequest(wifi, HTTP_GET, "http://rss.cnn.com/rss/edition_world.rss", body_callback);
    request->send(NULL, 0);
    delete request;

    displayHeadlines();
}



 /**
    * @authors Ole Sveinung Berget
    * @function buttonPressed()
*/
void buttonPressed() {
    functionState = (functionState + 1) % 7;
}



/**
   *@authors David Holt
*/
// Checks if the alarm-switch is on or off
void switchOn() {

    // Toggles alarmOn according to the switch state
    if (swOn == 0) {
        alarmOn = true;} else {
        alarmOn = false;}
}




 /**
    * @authors Ole Sveinung Berget, Are Berntsen, David Holt, Storm Selvig, Eivind Olsøy Solberg
    * @main()
*/
int main() {
    if (connect_to_wifi() != 0) {
        return 1;
    }
    
    button.fall(&buttonPressed);
    swOn.fall(&switchOn);
    swOn.rise(&switchOn);


    alarmTime.hour = 0;
    alarmTime.minute = 0;

    lcd.init();
    lcd.display();

 /**
    * @authors Ole Sveinung Berget
    * @buttonPressed()
*/ 
// while loop to change screens
    while(true) {
        if (functionState == 0) {
            geo_Loc(&alarmTime);
        } else if (functionState == 1) {
            loopDayMonth(&alarmTime);
        } else if (functionState == 2) {
            setAlarm(&alarmTime);
        } else if (functionState == 3) {
            loopTempHum();
        } else if (functionState == 4) {
            fetch_and_display_weather();
        } else if (functionState == 5) {
            iChooseYou();
        } else if (functionState == 6) {
            newsfeed();
        }
    }
    wifi->disconnect();
}
