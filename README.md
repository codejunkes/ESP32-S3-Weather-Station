# **ESP32-S3 Advanced Weather Station**

This project transforms an ESP32-S3 into a sophisticated, multi-interface weather and air quality monitoring station. It reads data from a BME688 sensor, displays it on a local Nokia 5110 LCD, and hosts a feature-rich web server for remote monitoring and control. The firmware is designed for stability and responsiveness, utilizing PSRAM for extensive data logging and providing Over-the-Air (OTA) update capabilities for convenient development.

## **Features**

* **Multi-Sensor Data Logging**: Measures and records Temperature, Humidity, Barometric Pressure, and Gas Resistance (Air Quality).  
* **Dual Interface**:  
  * **Local LCD**: A Nokia 5110 display provides at-a-glance readings and historical data plots, navigated with a rotary encoder.  
  * **Web Server**: A responsive web interface accessible from any device on the network, featuring live data cards and interactive historical charts.  
* **Intelligent Gas Sensing**: To prevent sensor self-heating from affecting temperature and humidity readings, gas is measured on an hourly cycle, followed by a 5-minute cool-down period. The device performs an initial gas reading on first boot.  
* **Onboard Device Control**:  
  * Adjust LCD backlight brightness directly from the device.  
  * Control the color of the ESP32-S3's onboard RGB LED locally via the encoder or remotely from the web UI.  
* **Data Persistence**: Remembers your last-set brightness and RGB color settings even after a reboot, thanks to the Preferences library.  
* **Developer Friendly**:  
  * **Over-the-Air (OTA) Updates**: Upload new firmware wirelessly over WiFi without needing a physical connection.  
  * **Web Serial Monitor**: A real-time serial monitor accessible in your browser at /webserial for easy wireless debugging.  
* **Efficient & Stable**:  
  * Uses PSRAM to store a large history of sensor readings without running out of memory.  
  * Employs a non-blocking sensor reading method to keep the UI and web server responsive at all times.  
  * Uses a dedicated hardware peripheral (PCNT) for the rotary encoder to ensure stable, interrupt-driven performance without conflicting with other components.

## **Hardware Setup**

### **Required Components**

* ESP32-S3 Development Board (with PSRAM)  
* BME688 Sensor Breakout Board  
* Nokia 5110 LCD Display  
* KY-040 Rotary Encoder Module  
* Breadboard and Jumper Wires

### **Wiring Diagram**

| ESP32-S3 Pin | Component | Connection |
| :---- | :---- | :---- |
| **GPIO 14** | BME688 Sensor | VCC |
| **GND** | BME688 / Nokia / Encoder | GND |
| **GPIO 8** | BME688 Sensor | SDA |
| **GPIO 9** | BME688 Sensor | SCL |
| **GPIO 12** | Nokia 5110 LCD | CLK (SCK) |
| **GPIO 11** | Nokia 5110 LCD | DIN (MOSI) |
| **GPIO 6** | Nokia 5110 LCD | DC |
| **GPIO 7** | Nokia 5110 LCD | CE (CS) |
| **GPIO 15** | Nokia 5110 LCD | RST |
| **GPIO 13** | Nokia 5110 LCD | BL (Backlight) |
| **GPIO 1** | Rotary Encoder | CLK |
| **GPIO 2** | Rotary Encoder | DT |
| **GPIO 3** | Rotary Encoder | SW (Switch) |
| **3.3V** | Rotary Encoder | \+ |

## **Software Setup**

### **1\. Arduino IDE Configuration**

* Ensure you have the ESP32 board support package installed.  
* Go to **Tools \-\> Board \-\> ESP32 Arduino** and select your specific ESP32-S3 board model.  
* **Crucially**, go to **Tools \-\> PSRAM** and select **"OPI PSRAM"**. The project will not run without this setting enabled.

### **2\. Library Dependencies**

Install the following libraries through the Arduino IDE's Library Manager (**Tools \-\> Manage Libraries...**):

* Adafruit BME680 Library  
* Adafruit GFX Library  
* Adafruit PCD8544 Nokia 5110 LCD library  
* Adafruit NeoPixel  
* ESP32Encoder by madhephaestus  
* WebSockets by Markus Sattler

## **How to Use**

### **Local LCD Interface**

The local interface is controlled entirely by the rotary encoder.

* **Rotate**: Turn the knob to cycle through the different pages (0-6).  
* **Click (Press)**: The function of the click depends on the current page.

**Page Guide:**

* **Page 0 (Main)**: Shows the latest readings for all sensors.  
* **Page 1 (Temp Plot)**: Shows a mini-chart of the last \~80 temperature readings.  
* **Page 2 (Humi Plot)**: Shows a mini-chart of the last \~80 humidity readings.  
* **Page 3 (Pres Plot)**: Shows a mini-chart of the last \~80 pressure readings.  
* **Page 4 (Gas Info)**: Displays the last valid gas reading and the time it was taken.  
* **Page 5 (Settings)**:  
  * Displays the device's IP address.  
  * **Click** to enter/exit backlight brightness edit mode.  
  * When in edit mode, **rotate** to change the brightness (0-255). The setting is saved when you exit edit mode.  
* **Page 6 (RGB Control)**:  
  * **Click** to enter edit mode (starts on Red).  
  * **Click again** to cycle through Green and Blue.  
  * **Click a final time** to exit edit mode and save the color.  
  * When in edit mode, **rotate** to change the selected color's value (0-255).

### **Web Interface**

1. Connect the device to power. The LCD will display the IP address on the Settings page.  
2. Open a web browser on any device on the same WiFi network and navigate to that IP address.  
3. **Main Page**: View live data and historical charts. Use the \< and \> buttons to navigate back and forth through the sensor history.  
4. **RGB Control**: Use the sliders to change the LED color in real-time. Click the "Save Color" button to make the setting permanent.  
5. **Web Serial**: Navigate to http://\<your-ip-address\>/webserial to view a live stream of the device's serial debug messages.

## **Future Improvements & Fixes**

* **WiFi Manager**: Implement a library like WiFiManager to allow for configuring WiFi credentials through a captive portal, removing the need to hard-code them.  
* **Web UI Sync**: Synchronize the web UI sliders (e.g., for RGB) with the device's current state on page load.  
* **Data Export**: Add a feature to the web UI to download the historical sensor data as a CSV file.  
* **Alerts**: Implement user-configurable thresholds on the web UI that could trigger the RGB LED to flash if a sensor value goes too high or low.
